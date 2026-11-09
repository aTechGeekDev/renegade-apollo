/*
 * apollogpu - Adreno 650 bring-up on Windows for the Xiaomi Mi 10T / 10T Pro (apollo, SM8250).
 *
 * Milestone 1 of GPU acceleration: the GPU powered, its GMU running its firmware, the SMMU programmed, the 3D core's
 * command processor started with its microcode, out of secure mode through TrustZone's zap shader, a test write and a
 * frame drawn by the GPU's 2D engine and copied to the panel. Then everything is taken down again.
 *
 * This is ios-apollo's xnu/bsd/dev/sweet_gpu.c (sweet_gpu_probe, steps 1-22 and its teardown, as left by the xnu patch
 * series 0096-0186), proven on this phone under XNU (boot 26: "the GPU draws, and its picture is on the panel"),
 * ported to Windows with the hardware sequence, values and checks unchanged. That file is itself modelled on HackiOS
 * (github.com/oosexgang/hackios-xnu-kernel, bsd/dev/redwood_adreno.c, APSL 2.0) and Xiaomi's apollo kgsl / Linux a6xx.
 * Its step-by-step comments are kept below. What changed for Windows:
 *   - register access, cache maintenance and the SMC are in apollogpu_asm.asm (exact instructions, see there);
 *   - memory the GPU reads is allocated write-combined (normal non-cacheable), never cacheable-plus-alias;
 *   - the firmware comes from \SystemRoot\System32\drivers\apollogpu\ (a650_gmu.bin, a650_sqe.fw, a650_zap.mdt,
 *     a650_zap.b02: the files ios-apollo's tools/pull-gpu-fw.sh takes from the phone) instead of the ADT;
 *   - boot-args are registry values under HKLM\SYSTEM\CurrentControlSet\Services\apollogpu\Parameters:
 *       Steps (DWORD, default 22) = sweetgpu, Level (1..6, default 1) = sweetgpulevel, Zap (0..3, default 3) = sweetgpuzap;
 *   - every line is written to Parameters\Log (REG_SZ) and Parameters\Status; the hive is flushed to disk before each
 *     step and each TrustZone call, so after a freeze the log shows where it stopped (no debugger needed);
 *   - the GPU is never kept on (sweetgpukeep): milestone 2 adds that.
 * Plain legacy driver, demand start: it runs when started (sc start apollogpu), never at boot.
 */
#include <ntddk.h>
#include <ntstrsafe.h>

typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
typedef __int32 int32_t;
typedef ULONG_PTR vm_offset_t;
typedef SIZE_T vm_size_t;
typedef int bool;
#define true  1
#define false 0

ULONG apollo_rd32(ULONG_PTR Address);
VOID apollo_wr32(ULONG_PTR Address, ULONG Value);
VOID apollo_dc_civac(ULONG_PTR Start, SIZE_T Size);
VOID apollo_dsb_sy(VOID);
VOID apollo_smc(UINT64 *Regs);

#pragma warning(disable : 4127)   /* conditional expression is constant */

#define REG_PARAMS L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\apollogpu\\Parameters"
#define FW_DIR     L"\\SystemRoot\\System32\\drivers\\apollogpu\\"
#define POOL_TAG   'upgA'

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD Unload;
static KSTART_ROUTINE ProbeThread;
static PKTHREAD Thread;

/* ---------- the log: every line to DbgPrint and to the registry (Parameters\Log, \Status) ---------- */
#define LOG_SIZE (192u * 1024u)
static CHAR *LogBuf;
static ULONG LogLen;

static VOID RegSetString(HANDLE Key, PCWSTR Name, const CHAR *Text, ULONG Len)
{
    UNICODE_STRING val;
    WCHAR *w = ExAllocatePool2(POOL_FLAG_NON_PAGED, ((SIZE_T)Len + 1) * sizeof(WCHAR), POOL_TAG);
    ULONG i;
    if (!w)
        return;
    for (i = 0; i < Len; i++)
        w[i] = (WCHAR)(UCHAR)Text[i];
    w[Len] = 0;
    RtlInitUnicodeString(&val, Name);
    (void)ZwSetValueKey(Key, &val, 0, REG_SZ, w, (Len + 1) * sizeof(WCHAR));
    ExFreePoolWithTag(w, POOL_TAG);
}

static HANDLE OpenParams(VOID)
{
    UNICODE_STRING key;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    RtlInitUnicodeString(&key, REG_PARAMS);
    InitializeObjectAttributes(&oa, &key, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE | KEY_QUERY_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL)))
        return NULL;
    return h;
}

/* The line as Status; with Flush also the whole log, and the hive on disk (before anything that may freeze the phone). */
static VOID LogSave(const CHAR *Status, ULONG StatusLen, bool Flush)
{
    HANDLE h = OpenParams();
    if (!h)
        return;
    if (Flush && LogBuf)
        RegSetString(h, L"Log", LogBuf, LogLen);
    if (Status)
        RegSetString(h, L"Status", Status, StatusLen);
    if (Flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

static bool Contains(const CHAR *Text, size_t Len, const CHAR *What)
{
    size_t n = 0, i;
    while (What[n])
        n++;
    for (i = 0; i + n <= Len; i++)
        if (RtlCompareMemory(Text + i, What, n) == n)
            return true;
    return false;
}

static VOID glog(const char *Fmt, ...)
{
    CHAR line[512];
    va_list args;
    size_t n = 0;
    bool flush;

    va_start(args, Fmt);
    (void)RtlStringCbVPrintfA(line, sizeof(line), Fmt, args);
    va_end(args);
    (void)RtlStringCbLengthA(line, sizeof(line), &n);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "apollogpu: %s", line);
    if (LogBuf && LogLen + n + 2 < LOG_SIZE) {
        RtlCopyMemory(LogBuf + LogLen, line, n);
        LogLen += (ULONG)n;
        if (n == 0 || line[n - 1] != '\n')
            LogBuf[LogLen++] = '\n';
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    /* "SWEET GPU: step N: ..." says what is about to be touched; so does "about to ask TrustZone" */
    flush = (n >= 15 && RtlCompareMemory(line, "SWEET GPU: step", 15) == 15) ||
            Contains(line, n, "about to ask TrustZone") ||
            (n >= 15 && RtlCompareMemory(line, "SWEET GPU: done", 15) == 15);
    LogSave(line, (ULONG)n, flush);
}
#define printf glog

static ULONG ReadParam(PCWSTR Name, ULONG Default)
{
    UNICODE_STRING key, val;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len = 0, v = Default;

    RtlInitUnicodeString(&key, REG_PARAMS);
    InitializeObjectAttributes(&oa, &key, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) {
        RtlInitUnicodeString(&val, Name);
        if (NT_SUCCESS(ZwQueryValueKey(h, &val, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
            info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            v = *(ULONG *)info->Data;
        ZwClose(h);
    }
    return v;
}

/* ---------- what XNU gave sweet_gpu.c ---------- */
static VOID delay(ULONG Us)
{
    if (Us < 1000) {
        KeStallExecutionProcessor(Us);
    } else {
        LARGE_INTEGER t;
        t.QuadPart = -(LONGLONG)Us * 10;
        (void)KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
}

static uint64_t now_ns(VOID)
{
    LARGE_INTEGER f, c = KeQueryPerformanceCounter(&f);
    return f.QuadPart ? (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull +
                        (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart : 0;
}

#define MAX_MAPS 32
static struct { PVOID va; SIZE_T size; } Maps[MAX_MAPS];
static ULONG NMaps;

static vm_offset_t io_map(uint64_t Pa, vm_size_t Size, ULONG Kind)
{
    PHYSICAL_ADDRESS p;
    PVOID va;
    p.QuadPart = (LONGLONG)Pa;
    va = MmMapIoSpaceEx(p, Size, PAGE_READWRITE | Kind);
    if (va && NMaps < MAX_MAPS) {
        Maps[NMaps].va = va;
        Maps[NMaps++].size = Size;
    }
    return (vm_offset_t)va;
}
#define ml_io_map(pa, size)        io_map((pa), (size), PAGE_NOCACHE)
#define ml_io_map_wcomb(pa, size)  io_map((pa), (size), PAGE_WRITECOMBINE)

static VOID ml_io_unmap(vm_offset_t Va, vm_size_t Size)
{
    ULONG i;
    for (i = 0; i < NMaps; i++)
        if (Maps[i].va == (PVOID)Va) {
            MmUnmapIoSpace(Maps[i].va, Maps[i].size);
            Maps[i] = Maps[--NMaps];
            return;
        }
    MmUnmapIoSpace((PVOID)Va, Size);
}

static uint64_t kvtophys(vm_offset_t Va)
{
    return (uint64_t)MmGetPhysicalAddress((PVOID)Va).QuadPart;
}

/* Memory the GMU / the 3D core reach: zeroed, write-combined (normal non-cacheable, as the SMMU entries' attribute 0),
 * any pages below 64 GiB (the SMMU's 36-bit output). Contiguous only where one physical address is handed over. */
#define MAX_ALLOCS 1024
static struct { PMDL mdl; PVOID va; SIZE_T size; bool contig; } Allocs[MAX_ALLOCS];
static ULONG NAllocs;

static vm_offset_t gpu_mem_alloc(vm_size_t Size, bool Contiguous)
{
    PHYSICAL_ADDRESS low, high, skip;
    PVOID va = NULL;
    PMDL mdl = NULL;
    Size = ROUND_TO_PAGES(Size);
    if (NAllocs == MAX_ALLOCS)
        return 0;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFLL;
    skip.QuadPart = 0;
    if (Contiguous) {
        va = MmAllocateContiguousMemorySpecifyCache(Size, low, high, skip, MmWriteCombined);
        if (va) {
            volatile UCHAR *b = (volatile UCHAR *)va;
            SIZE_T i;
            for (i = 0; i < Size; i++)
                b[i] = 0;
        }
    } else {
        mdl = MmAllocatePagesForMdlEx(low, high, skip, Size, MmWriteCombined, MM_ALLOCATE_FULLY_REQUIRED);
        if (mdl) {
            va = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmWriteCombined, NULL, FALSE,
                                              NormalPagePriority | MdlMappingNoExecute);
            if (!va) {
                MmFreePagesFromMdl(mdl);
                ExFreePool(mdl);
                mdl = NULL;
            }
        }
    }
    if (!va)
        return 0;
    Allocs[NAllocs].mdl = mdl;
    Allocs[NAllocs].va = va;
    Allocs[NAllocs].size = Size;
    Allocs[NAllocs++].contig = Contiguous;
    return (vm_offset_t)va;
}

static VOID gpu_mem_free_all(VOID)
{
    while (NAllocs) {
        NAllocs--;
        if (Allocs[NAllocs].contig) {
            MmFreeContiguousMemorySpecifyCache(Allocs[NAllocs].va, Allocs[NAllocs].size, MmWriteCombined);
        } else {
            MmUnmapLockedPages(Allocs[NAllocs].va, Allocs[NAllocs].mdl);
            MmFreePagesFromMdl(Allocs[NAllocs].mdl);
            ExFreePool(Allocs[NAllocs].mdl);
        }
    }
}

/* A firmware file from FW_DIR, whole, in non-paged pool (NULL: missing or not between MinSize and MaxSize bytes). */
static uint8_t *fw_load(PCWSTR Name, ULONG MinSize, ULONG MaxSize, unsigned int *Size)
{
    WCHAR path[128];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION si;
    HANDLE h;
    uint8_t *data = NULL;
    *Size = 0;
    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"%s%s", FW_DIR, Name)))
        return NULL;
    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                 FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        return NULL;
    if (NT_SUCCESS(ZwQueryInformationFile(h, &io, &si, sizeof(si), FileStandardInformation)) &&
        si.EndOfFile.QuadPart >= MinSize && si.EndOfFile.QuadPart <= MaxSize) {
        ULONG n = (ULONG)si.EndOfFile.QuadPart;
        data = ExAllocatePool2(POOL_FLAG_NON_PAGED, n, POOL_TAG);
        if (data) {
            LARGE_INTEGER at;
            at.QuadPart = 0;
            if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &io, data, n, &at, NULL)) && io.Information == n) {
                *Size = n;
            } else {
                ExFreePoolWithTag(data, POOL_TAG);
                data = NULL;
            }
        }
    }
    ZwClose(h);
    return data;
}

/* ======================================================================================================================
 * From here on: ios-apollo sweet_gpu.c (Adreno 650 bring-up in steps: look before touching), its comments included.
 * ====================================================================================================================== */

/*
 *   1  read   main clock controller (GCC 0x100000), always readable: the
 *             GPU config-bus clock (gcc_gpu_cfg_ahb, the gate in front of
 *             the GPU clock controller), memnoc/snoc paths, GPLL0 votes
 *   2  write  gcc_gpu_cfg_ahb on, only if step 1 found it off (one bit, as
 *             Linux's clk_branch enable does)
 *   3  read   GPU clock controller (GPUCC 0x3d90000) as the bootloader left
 *             it: PLL1, the GMU clock source and branches, the CX power
 *             switch (GDSC) and the GX domain. Refused unless the config-bus
 *             clock is confirmed on.
 *   4  write  CX power on: clear the CX GDSC's collapse bit, wait for power
 *   5  write  clocks: GCC votes for the GPLL0 taps to the GPU, the GMU clock
 *             to 200 MHz (GPLL0 300 MHz tap / 1.5, as Linux), the GPUCC
 *             branches the GMU and its SMMU need
 *   6  read   the GMU (0x3d6a000): reset and boot state, HFI, power state, and its always-on counter,
 *             which counts the 19.2 MHz crystal once the block is alive
 *   7  read   the GMU's RPMh channel (RSCC 0x3de0000)
 *   8  read   the GPU's SMMU (kgsl-smmu 0x3da0000, 64 KiB, MMU-500 under the
 *             hypervisor): ID registers, stream match table, context kinds
 *   9  read   SMMU translation context 0 (the GPU's own)
 *  10  write  GMU processor held in reset; two words into its data memory
 *             (DTCM) and back, then restored: can its firmware be loaded?
 * The SMMU's global registers are the hypervisor's emulation: it traps the access and can only carry out plain
 * single-register loads and stores; all accesses are one ldr/str (apollogpu_asm.asm).
 *  11  write  memory for the GMU: the blocks a650_gmu.bin asks for, its log and HFI
 *             page, page tables, and SMMU context 1 with stream 5 (the GMU)
 *  12  write  the firmware: TCM parts through the GMU's registers (checked),
 *             the rest into that memory
 *  13  write  RSCC sleep/wake program and PDC enable (apollo kgsl does this
 *             before the GMU starts), the GMU's configuration, chip ID
 *             0x06050200, then out of reset; FW_INIT_RESULT [8:0] = 0x100 is
 *             a running firmware
 *  14  write  start the HFI message queues
 *  15  read   the boot loader's RPMh command database (cmd_db@80860000, RAM
 *             outside the kernel's): the GPU, GMU and memory rails' levels and
 *             the bus resources, and from them the votes for the GMU's tables
 *  16  write  HFI messages: the table of GPU speeds with their rail votes,
 *             the bus vote table, "core firmware start", "start"
 *  17  write  a GPU speed (Level, 1 = 305 MHz .. 6 = 587 MHz) and
 *             the request to power the 3D core (OOB "GPU set"); once the GMU
 *             grants it, read the 3D core's own registers (0x3d00000)
 *  18  write  the 3D core's memory: its ring and its microcode (a650_sqe.fw)
 *             behind SMMU context 0, stream 0 mask 0x401 as the DT's gfx3d_user has it
 *  19  write  the core's registers as Linux's hw_init for the a650 sets them
 *             before the command processor starts, then start it and give it
 *             its first command, CP_ME_INIT (Linux a6xx_cp_init)
 *  20  asks    TrustZone to authenticate and run the vendor's signed "zap
 *             shader" (a650_zap.mdt/.b02); only then can the
 *             command processor be told to leave secure mode, which it comes
 *             out of reset locked in (CP_SET_SECURE_MODE 0). The SCM calls are
 *             a ladder set by Zap (0 off; 1 the no-memory queries; 2 +=
 *             the signature; 3 += the load and leaving secure mode, the default)
 *  21  write  a page of ours mapped for the core; CP_MEM_WRITE puts a word in
 *             it, read back by the CPU: the core writes our memory on command
 *  22  write  a 1080x2400 picture in our memory (rows of 4352 bytes, the 2D
 *             engine's pitch is in 64-byte units), filled by the core's 2D
 *             engine in three bands (orange, white, blue from the top), its
 *             caches flushed, checked by the CPU, then copied to the panel's
 *             framebuffer (0x9c000000, rows of 4320 bytes)
 * After step 4 or later the CX half is collapsed again before returning. A 3D core powered by step 17 is given
 * back first (OOB clear, "prepare slumber", RPMh channel to sleep); a zap shader loaded in step 20 is handed back
 * to TrustZone.
 *
 * Addresses: apollo live DT (qcom,gpucc@3d90000 "qcom,gpucc-kona", gpu_cx_gdsc 0x3d9106c, gpu_gx_domain_addr
 * 0x3d91508, gpu_cx_hw_ctrl 0x3d91540, kgsl-3d0@3d00000 chipid 0x06050002, gmu@3d6a000, kgsl-smmu@3da0000).
 */

#define GPU_GCC_PA            0x00100000u
#define GPU_GCC_SIZE          0x90000
#define GCC_GPU_CFG_AHB       0x71004u    /* hwcg bit 1 */
#define GCC_GPU_MEMNOC_GFX    0x7100cu
#define GCC_GPU_SNOC_DVM_GFX  0x71018u
#define GCC_GPU_IREF_EN       0x8c014u
#define GCC_VOTE_0            0x52000u    /* bit 15 gpu_gpll0, bit 16 gpu_gpll0_div */

#define GPU_CC_PA             0x03d90000u
#define GPU_CC_SIZE           0x9000
#define GPUCC_PLL1            0x100u      /* Lucid: +0 mode, +4 L */
#define GPUCC_GMU_RCG         0x1120u     /* +0 cmd, +4 cfg */
#define GPUCC_CXO_AON         0x1004u
#define GPUCC_GX_GMU          0x1064u
#define GPUCC_CX_GDSC         0x106cu     /* bit 31 PWR_ON, bit 0 SW_COLLAPSE */
#define GPUCC_AHB             0x1078u
#define GPUCC_CX_SNOC_DVM     0x108cu
#define GPUCC_CX_GMU          0x1098u
#define GPUCC_CXO             0x109cu
#define GPUCC_GX_DOMAIN       0x1508u
#define GPUCC_CX_HW_CTRL      0x1540u
#define GPUCC_SMMU_VOTE       0x5000u

#define CBCR_OFF(v)           (((v) >> 31) & 1u)
#define GCC_DDRSS_GPU_AXI     0x71154u    /* voted: status not reliable */
#define GDSC_SW_COLLAPSE      (1u << 0)
#define GDSC_HW_CONTROL       (1u << 1)
#define GDSC_PWR_ON           (1u << 31)
#define RCG_UPDATE            (1u << 0)
#define RCG_CFG_MASK          0x371fu     /* [4:0] hid, [10:8] source, [13:12] mode */
#define GMU_RCG_200MHZ        ((6u << 8) | 2u)  /* GPLL0_OUT_MAIN_DIV (300 MHz) / 1.5 */
#define GPUCC_CX_APB          0x1088u

#define GMU_PA                0x03d6a000u
#define GMU_SIZE              0x30000
#define GMU_WORD(n)           (4u * (n))
#define GMU_CM3_SYSRESET      GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG   GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT GMU_WORD(0x501c)
#define GMU_CM3_CFG           GMU_WORD(0x502d)
#define GMU_AO_COUNTER_L      GMU_WORD(0x5088)
#define GMU_AO_COUNTER_H      GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK   GMU_WORD(0x50d0)
#define GMU_RPMH_POWER_STATE  GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS   GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO  GMU_WORD(0x5181)
#define GMU_AO_HOST_IRQ_STATUS GMU_WORD(0x9305)
#define GMU_AO_GPU_CX_BUSY    GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS  GMU_WORD(0x9313)
#define GMU_RSCC_CONTROL_ACK  GMU_WORD(0x9308)
#define GMU_DTCM              GMU_WORD(0x1c00)

#define RSCC_PA               0x03de0000u
#define RSCC_SIZE             0x10000
#define RSCC_DRV_CONFIG       0x00cu
#define RSCC_RSC_STATUS0_DRV0 (4u * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0 (4u * 0x00b)
#define RSCC_SEQ_BUSY_DRV0    (4u * 0x101)
#define RSCC_SEQ_MEM_0_DRV0   (4u * 0x180)
#define RSCC_TCS0_DRV0_STATUS (4u * 0x346)

#define SMMU_PA               0x03da0000u
#define SMMU_SIZE             0x10000u    /* apollo DT: <0x3da0000 0x10000> */
#define SMMU_SCR0             0x000u
#define SMMU_IDR0             0x020u
#define SMMU_IDR1             0x024u
#define SMMU_IDR2             0x028u
#define SMMU_IDR7             0x03cu
#define SMMU_SGFSR            0x048u
#define SMMU_SMR(n)           (0x800u + 4u * (n))
#define SMMU_S2CR(n)          (0xc00u + 4u * (n))
#define SMMU_CB_SCTLR         0x00u
#define SMMU_CB_TTBR0         0x20u
#define SMMU_CB_TCR           0x30u
#define SMMU_CB_FSR           0x58u
#define SMMU_CB_TCR2          0x10u
#define SMMU_CB_TTBR1         0x28u
#define SMMU_CB_MAIR0         0x38u
#define SMMU_CB_MAIR1         0x3cu
#define SMMU_CB_FAR           0x60u
#define SMMU_CB_FSYNR0        0x68u
#define SMMU_CB_TLBIASID      0x610u
#define SMMU_CB_TLBSYNC       0x7f0u
#define SMMU_CB_TLBSTATUS     0x7f4u
#define SMMU_CBA2R(n)         (0x800u + 4u * (n))   /* in page 1, like CBAR */
#define SMMU_SMR_VALID        (1u << 31)
#define SMMU_STREAM_MASK      0x400u                /* DT: second cell of the GMU's iommus */
#define SMMU_S2CR_FAULT       (2u << 16)
#define SMMU_CBAR_STAGE_1     ((1u << 16) | (3u << 8) | (0xfu << 12))
#define SMMU_CBA2R_64_BIT     1u
#define SMMU_SCTLR_ON         ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12))
#define SMMU_TCR_39_BITS      (25u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23))
#define SMMU_TCR2             (1u | (1u << 4) | (7u << 15))   /* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0            0xf404ff44u
#define SMMU_FSR_ALL          0xc00001feu

/* GMU start (vendor a6xx_gmu_fw_start / a6xx_gmu_power_config / a6xx_gmu_start) */
#define GMU_ITCM              GMU_WORD(0x0c00)
#define GMU_DTCM_VERSIONS     (GMU_DTCM + 4u * 0xff8)
#define GMU_DTCM_BREADCRUMB   (GMU_DTCM + 0x3fdcu)
#define GMU_ICACHE_CONFIG     GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG     GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG    GMU_WORD(0x4c0f)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x50c0)
#define GMU_RPMH_CTRL         GMU_WORD(0x50e8)
#define GMU_RPMH_ENABLE_MASK  0xf11u
#define GMU_CX_FAL_INTF       GMU_WORD(0x50f0)
#define GMU_PWR_COL_CP_MSG    GMU_WORD(0x5100)
#define GMU_PWR_COL_CP_RESP   GMU_WORD(0x5101)
#define GMU_HFI_SFR_ADDR      GMU_WORD(0x5182)
#define GMU_HFI_QTBL_INFO     GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR     GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT     GMU_WORD(0x5186)
#define GMU_GMU2HOST_INTR_CLR GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO GMU_WORD(0x5192)
#define GMU_GENERAL_7         GMU_WORD(0x51cc)
#define GMU_RSCC_CONTROL_REQ  GMU_WORD(0x9307)
#define GMU_AHB_FENCE_RANGE_0 GMU_WORD(0x9311)
#define GMU_FENCE_RANGE_MASK  ((1u << 31) | ((0xau << 2) << 18) | 0x8a0u)
#define GMU_CHIPID            0x06050200u   /* DT chipid 0x06050002, minor/patch packed */
#define GMU_TCM_BOOT          2u
#define GMU_STARTED_MASK      0x1ffu
#define GMU_STARTED           0x100u
#define GBIF_HALT_PA          (0x03d00000u + 4u * 0x3c45)

#define GMU_ITCM_ADDRESS      0x00000000u
#define GMU_DTCM_ADDRESS      0x10004000u   /* kgsl gmu_vma: SZ_256M + SZ_16K */
#define GMU_TCM_SIZE          0x4000u
#define GMU_NONCACHED_KERNEL  0x60000000u
#define GMU_LOG_SIZE          0x4000u
#define GMU_STREAM            5u            /* upstream sm8250 gmu iommus <&adreno_smmu 5 0x400> */
#define GMU_MATCH             1u
#define GMU_CONTEXT           1u
#define GMU_FW_PREALLOC       1u
#define GMU_FW_PREALLOC_KEEP  7u

#define RSCC_PDC_SEQ_START    (4u * 0x008)
#define RSCC_PDC_MATCH_LO     (4u * 0x009)
#define RSCC_PDC_MATCH_HI     (4u * 0x00a)
#define RSCC_HIDDEN_CMD0_ADDR (4u * 0x00d)
#define RSCC_HIDDEN_CMD0_DATA (4u * 0x00e)
#define RSCC_OVERRIDE_START   (4u * 0x100)
#define RSCC_ASLEEP           (1u << 16)
#define PDC_CFG_PA            0x0b290000u   /* DT kgsl_gmu_pdc_cfg */
#define PDC_GPU_ENABLE        (4u * 0x1140)
#define PDC_GPU_SEQ_START     (4u * 0x1148)

/* GMU talk and the 3D core (vendor kgsl_hfi.c/h, adreno_a6xx_gmu.c; Linux a6xx_hfi.c, a6xx_gmu.c) */
#define GMU_CX_FALNEXT_INTF   GMU_WORD(0x50f1)
#define GMU_HOST2GMU_INTR_SET GMU_WORD(0x5194)
#define GMU_GPU_SET           (1u << 30)    /* OOB "GPU set" (non-legacy): keep the 3D core up */
#define GMU_GPU_CLEAR         (1u << 31)
#define GMU_GPU_GRANTED       (1u << 31)    /* of gmu2host_intr_info */
#define GMU_GX_OFF_BITS       0xc0u         /* sptprac_pwr_clk_status: GX GDSC off, GX clock off */
#define GMU_CX_BUSY_GPU       (1u << 23)    /* ao_gpu_cx_busy_status: GPUBUSYIGNAHB */
#define GMU_LOG_TAIL          16u           /* events of 16 bytes shown when the GMU stops answering */
#define HFI_BW_TABLE          3u
#define HFI_PERF_TABLE        4u
#define HFI_START             10u
#define HFI_CORE_FW_START     14u
#define HFI_GX_BW_PERF_VOTE   30u
#define HFI_PREPARE_SLUMBER   33u
#define HFI_F2H_ERR           100u
#define HFI_F2H_DEBUG         101u
#define HFI_ACK               1u            /* message type, bits 19:16 */
#define HFI_QUEUE_WORDS       1024u
#define HFI_PERF_WORDS        59u           /* hdr, 2 counts, 16 x {vote, acd, kHz}, 4 x {vote, kHz} */
#define HFI_BW_WORDS          160u          /* hdr, 5 counts, cnoc 6 + 2x6, ddr 8 + 16x8 */
#define GPUCC_PLL0            0x000u        /* the 3D core's PLL, the GMU's to program */
#define GPUCC_GX_SW_RESET     0x1008u
#define GPUCC_GX_GDSC         0x100cu       /* kona.dtsi gpu_gx_gdsc 0x3d9100c */
#define GPU_PA                0x03d00000u   /* kgsl_3d0_reg_memory */
#define GPU_SIZE              0x40000u
#define GPU_RBBM_CLOCK_CNTL   (4u * 0x00ae)
#define GPU_RBBM_INT_0_STATUS (4u * 0x0201)
#define GPU_RBBM_STATUS       (4u * 0x0210)
#define GPU_CP_RB_BASE        (4u * 0x0800)
/* The 3D core's command processor (Linux a6xx_gpu.c hw_init for the a650, Mesa a6xx.xml; words) */
#define GPU_WORD(n)           (4u * (n))
#define GPU_RBBM_GBIF_CLIENT_QOS GPU_WORD(0x0011)
#define GPU_RBBM_GBIF_HALT    GPU_WORD(0x0016)
#define GPU_RBBM_GBIF_HALT_ACK GPU_WORD(0x0017)
#define GPU_RBBM_HANG_INT_CNTL GPU_WORD(0x001f)
#define GPU_RBBM_PERFCTR_CNTL GPU_WORD(0x0500)
#define GPU_RBBM_PERFCTR_BUSY_MASKED GPU_WORD(0x050b)
#define GPU_CP_RB_CNTL        GPU_WORD(0x0802)
#define GPU_CP_RB_RPTR        GPU_WORD(0x0806)
#define GPU_CP_RB_WPTR        GPU_WORD(0x0807)
#define GPU_CP_SQE_CNTL       GPU_WORD(0x0808)
#define GPU_CP_HW_FAULT       GPU_WORD(0x0821)
#define GPU_CP_INTERRUPT_STATUS GPU_WORD(0x0823)
#define GPU_CP_PROTECT_STATUS GPU_WORD(0x0824)
#define GPU_CP_SQE_INSTR_BASE GPU_WORD(0x0830)
#define GPU_CP_APRIV_CNTL     GPU_WORD(0x0844)
#define GPU_CP_ROQ_THRESHOLDS_1 GPU_WORD(0x08c1)
#define GPU_CP_ROQ_THRESHOLDS_2 GPU_WORD(0x08c2)
#define GPU_CP_MEM_POOL_SIZE  GPU_WORD(0x08c3)
#define GPU_CP_AHB_CNTL       GPU_WORD(0x098d)
#define GPU_UCHE_MODE_CNTL    GPU_WORD(0x0e01)
#define GPU_UCHE_WRITE_RANGE_MAX GPU_WORD(0x0e05)
#define GPU_UCHE_WRITE_THRU_BASE GPU_WORD(0x0e07)
#define GPU_UCHE_TRAP_BASE    GPU_WORD(0x0e09)
#define GPU_UCHE_CACHE_WAYS   GPU_WORD(0x0e17)
#define GPU_UCHE_FILTER_CNTL  GPU_WORD(0x0e18)
#define GPU_UCHE_CLIENT_PF    GPU_WORD(0x0e19)
#define GPU_GBIF_QSB_SIDE0    GPU_WORD(0x3c03)
#define GPU_GBIF_HALT         GPU_WORD(0x3c45)
#define GPU_GBIF_HALT_ACK     GPU_WORD(0x3c46)
#define GPU_RB_NC_MODE_CNTL   GPU_WORD(0x8e08)
#define GPU_PC_DBG_ECO_CNTL   GPU_WORD(0x9e00)
#define GPU_SP_NC_MODE_CNTL   GPU_WORD(0xae02)
#define GPU_TPL1_NC_MODE_CNTL GPU_WORD(0xb604)
#define GPU_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define GPU_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define GPU_SECVID_TSB_CNTL   GPU_WORD(0xf803)
#define GPU_BUSY              (1u << 23)    /* rbbm_status: GPU_BUSY_IGN_AHB (Linux _a6xx_check_idle) */
#define GPU_RB_CNTL_VALUE     (12u | (2u << 8) | (1u << 27))   /* 32 KiB ring, 32-byte blocks, no rptr copy */
#define GPU_APRIV_VALUE       ((1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1))  /* a650 has HW APRIV */
#define GPU_SQE_MIN_VERSION   0x095u        /* Linux a6xx_ucode_check_version for a650_sqe.fw */
#define CP_ME_INIT            0x48u
#define CP_SET_SECURE_MODE    0x66u         /* the command that takes the 3D core out of secure mode */
/* TrustZone (SCM) calls that authenticate a firmware image, convention qcom_scm-smc.c */
#define SCM_FAST              (1ull << 31)  /* atomic: answered at once (the queries). PAS calls omit it and may yield */
#define SCM_64                (1ull << 30)
#define SCM_SIP               (2ull << 24)  /* a call of the SoC maker's, not an Arm standard one */
#define SCM_CALL(svc, cmd)    (((uint64_t)(svc) << 8) | (cmd))
#define SCM_IS_CALL_AVAILABLE SCM_CALL(6, 1) /* the no-memory query Linux asks TrustZone first */
#define SCM_PAS_INIT_IMAGE    SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP     SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN      SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED  SCM_CALL(2, 7) /* a no-memory query: does TrustZone manage this image? */
#define SCM_ONE_VALUE         1u            /* argument descriptor: one plain number */
#define SCM_VALUE_AND_BUFFER  (2u | (2u << 6)) /* two: a number, then an address of memory TrustZone may use */
#define SCM_THREE_VALUES      3u            /* three plain numbers */
#define SCM_INTERRUPTED       1             /* a preempted call says so, and is made again so */
#define PAS_GPU               13u           /* the GPU's image number (Linux qcom_scm: the zap shader) */
#define ZAP_MEMORY_PA         0x8681a000u   /* apollo live DT reserved-memory pil_gpu_mem (the zap image's place) */
#define ZAP_MEMORY_SIZE       0x1000u       /* the a650 zap shader's one loadable segment is under a page */
#define CORE_STREAM           0u            /* apollo DT gfx3d_user iommus <0x0 0x401>: streams 0 and 1 */
#define CORE_STREAM_MASK      0x401u        /* boot 19: mask 0x400 read back 0; written as the DT has it */
#define CORE_MATCH            0u            /* match 1 is the GMU's */
#define CORE_CONTEXT          0u
#define CORE_RING_VA          0x01000000u
#define CORE_RING_SIZE        0x8000u
#define CORE_RING_WORDS       (CORE_RING_SIZE / 4u)
#define CORE_SQE_VA           0x01100000u
#define PTE_UNPRIVILEGED      (1ull << 6)   /* AP[1]: the core's unprivileged requests too */
#define PTE_READ_ONLY         (1ull << 7)   /* AP[2]: the microcode (Linux MSM_BO_GPU_READONLY) */
#define PTE_CORE              (PTE_GMU | PTE_UNPRIVILEGED)
#define SMMU_SCTLR_HUPCF      (1u << 8)     /* serve other requests after a fault (Linux qcom_adreno_smmu) */

/* Steps 21-22: commands (Mesa adreno_pm4.xml) and the 2D engine (a6xx.xml, register numbers in words) */
#define CP_WAIT_FOR_IDLE      0x26u
#define CP_BLIT               0x2cu
#define CP_MEM_WRITE          0x3du
#define CP_EVENT_WRITE        0x46u
#define CP_SET_MARKER         0x65u
#define CP_BLIT_OP_SCALE      3u
#define RM6_BLIT2DSCALE       12u           /* CP_SET_MARKER mode: 2D blits follow */
#define EVENT_FLUSH_COLOR_TS  0x1du         /* PC_CCU_FLUSH_COLOR_TS */
#define EVENT_CACHE_FLUSH_TS  0x04u
#define REG_GRAS_2D_BLIT_CNTL 0x8400u
#define REG_GRAS_2D_DST_TL    0x8405u
#define REG_GRAS_2D_DST_BR    0x8406u
#define REG_RB_2D_BLIT_CNTL   0x8c00u
#define REG_RB_2D_PIXEL_CNTL  0x8c01u
#define REG_RB_2D_DST_INFO    0x8c17u
#define REG_RB_2D_DST         0x8c18u
#define REG_RB_2D_DST_PITCH   0x8c1au
#define REG_RB_2D_SOLID_C0    0x8c2cu
#define REG_RB_CCU_CNTL       0x8e07u
#define REG_SP_2D_DST_FORMAT  0xacc0u
#define FMT6_8_8_8_8_UNORM    0x30u
#define BLIT_SOLID_8888       ((1u << 7) | (FMT6_8_8_8_8_UNORM << 8) | (0xfu << 20) | (0x10u << 24))
#define DST_BGRA_LINEAR       (FMT6_8_8_8_8_UNORM | (1u << 10))   /* blue first in memory, as the panel's */
#define CCU_CNTL_BYPASS       0x18000000u   /* color offset = 3 CCUs x 64 KiB (Mesa a650 num_ccu 3), sysmem */
#define CORE_SCRATCH_VA       0x01200000u
#define CORE_TARGET_VA        0x10000000u
#define TARGET_W              1080u
#define TARGET_H              2400u
#define TARGET_ROW            4352u         /* 1088 pixels: the next multiple of 64 bytes */
#define FB_PA                 0x9c000000u   /* Mu-Silicium "Display Reserved": the panel's framebuffer, a8r8g8b8 */
#define FB_ROW                (TARGET_W * 4u)
#define TEST_WORD             0x600dc0deu

#define CMD_DB_PA             0x80860000u   /* kona.dtsi reserved-memory cmd_db, qcom,cmd-db */
#define CMD_DB_SIZE           0x20000u
#define CMD_DB_DATA           144u          /* version, magic, 8 slave headers of 16 bytes, checksum, reserved */
#define CMD_DB_ENTRY          24u           /* id[8], priority[2], addr, len.16, offset.16 */
#define RPMH_MIN_SVS          48u           /* the GMU's own level (vendor rpmh_gmu_arc_votes_init) */

#define PTE_TABLE             3ull
#define PTE_PAGE              3ull
#define PTE_SHARED            (2ull << 8)
#define PTE_ACCESSED          (1ull << 10)
#define PTE_NOT_GLOBAL        (1ull << 11)
#define PTE_ADDRESS           0x0000fffffffff000ull
#define PTE_GMU               (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)

/* One plain ldr / str each: the only forms the hypervisor's emulation handles (apollogpu_asm.asm). */
static uint32_t
gpu_rd(vm_offset_t base, uint32_t off)
{
	return apollo_rd32(base + off);
}

static void
gpu_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	apollo_wr32(base + off, val);
}

static void
gpu_cbcr(vm_offset_t base, const char *name, uint32_t off)
{
	uint32_t v = gpu_rd(base, off);
	printf("SWEET GPU:   %-22s %#07x = %#010x  %s%s%s\n", name, off, v,
	    CBCR_OFF(v) ? "off" : "running", (v & 1u) ? ", enabled" : "",
	    (v & 2u) ? ", hw-gated" : "");
}

/* Set a branch's enable bit; true if CLK_OFF clears (or the status is not trusted). */
static bool
gpu_clock_on(vm_offset_t base, const char *name, uint32_t off, bool trust)
{
	uint32_t v;
	gpu_wr(base, off, gpu_rd(base, off) | 1u);
	for (unsigned i = 0; i < 100 && CBCR_OFF(gpu_rd(base, off)); ++i)
		delay(10);
	v = gpu_rd(base, off);
	printf("SWEET GPU:   %-22s %#07x = %#010x  %s\n", name, off, v,
	    !CBCR_OFF(v) ? "running" : trust ? "STAYS OFF" : "off (status not reliable)");
	return (!CBCR_OFF(v) || !trust) ? true : false;
}

static bool
gpu_cx_on(vm_offset_t cc)
{
	return ((gpu_rd(cc, GPUCC_CX_GDSC) | gpu_rd(cc, GPUCC_CX_HW_CTRL)) & GDSC_PWR_ON) != 0;
}

/* ---- memory the GMU and the 3D core reach through the SMMU (HackiOS's gpu_space) ---- */
static void
gpu_to_memory(vm_offset_t start, vm_size_t size)
{
	apollo_dc_civac(start, size);
}

/* 4 KiB tables: the root, an L2 per GiB and an L3 per 2 MiB in use */
#define GPU_SPACE_TABLES 8192
#define GPU_SPACE_HASH   16384
struct gpu_space {
	const char *name;
	unsigned count;
	uint64_t *table[GPU_SPACE_TABLES];
	uint64_t phys[GPU_SPACE_TABLES];
	uint16_t by_phys[GPU_SPACE_HASH];     /* index + 1 of the table at a physical address (open addressing) */
};
static struct gpu_space gmu_space = { "GMU" }, core_space = { "core" };

static uint64_t *
gpu_new_table(struct gpu_space *sp)
{
	uint64_t *t, phys;
	if (sp->count == GPU_SPACE_TABLES)
		return NULL;
	t = (uint64_t *)gpu_mem_alloc(4096, false);
	if (!t)
		return NULL;
	phys = kvtophys((vm_offset_t)t);
	for (uint32_t h = (uint32_t)(phys >> 12) % GPU_SPACE_HASH;; h = (h + 1) % GPU_SPACE_HASH) {
		if (sp->by_phys[h] == 0) {
			sp->by_phys[h] = (uint16_t)(sp->count + 1);
			break;
		}
	}
	sp->phys[sp->count] = phys;
	sp->table[sp->count++] = t;
	gpu_to_memory((vm_offset_t)t, 4096);
	return t;
}

/* Our address of the table at a physical address; NULL: not one of this space's. */
static uint64_t *
gpu_find_table(struct gpu_space *sp, uint64_t phys)
{
	for (uint32_t h = (uint32_t)(phys >> 12) % GPU_SPACE_HASH; sp->by_phys[h] != 0; h = (h + 1) % GPU_SPACE_HASH)
		if (sp->phys[sp->by_phys[h] - 1] == phys)
			return sp->table[sp->by_phys[h] - 1];
	return NULL;
}

static uint64_t *
gpu_next_table(struct gpu_space *sp, uint64_t *entry)
{
	uint64_t *t;
	if (*entry & 1)
		return gpu_find_table(sp, *entry & PTE_ADDRESS);
	t = gpu_new_table(sp);
	if (t) {
		*entry = kvtophys((vm_offset_t)t) | PTE_TABLE;
		gpu_to_memory((vm_offset_t)entry, sizeof(*entry));
	}
	return t;
}

/* One 4K page; three levels, 39-bit addresses (DT qcom,use-3-lvl-tables). */
static bool
gpu_map(struct gpu_space *sp, uint64_t va, uint64_t pa, uint64_t attrs)
{
	uint64_t *l2, *l3;
	if ((va >> 39) || (sp->count == 0 && !gpu_new_table(sp)))
		return false;
	l2 = gpu_next_table(sp, &sp->table[0][(va >> 30) & 0x1ff]);
	l3 = l2 ? gpu_next_table(sp, &l2[(va >> 21) & 0x1ff]) : NULL;
	if (!l3)
		return false;
	l3[(va >> 12) & 0x1ff] = pa | attrs | PTE_PAGE;
	gpu_to_memory((vm_offset_t)&l3[(va >> 12) & 0x1ff], sizeof(uint64_t));
	return true;
}

struct gpu_block {
	uint32_t va, size;
	vm_offset_t mem;
};

static bool
gpu_block_new(struct gpu_space *sp, struct gpu_block *b, uint32_t va, uint32_t size, uint64_t attrs)
{
	b->va = va;
	b->size = (size + 4095u) & ~4095u;
	b->mem = gpu_mem_alloc(b->size, false);
	if (!b->mem)
		return false;
	for (uint32_t o = 0; o < b->size; o += 4096u)
		if (!gpu_map(sp, va + o, kvtophys(b->mem + o), attrs))
			return false;
	gpu_to_memory(b->mem, b->size);
	printf("SWEET GPU:   %s %#x..%#x -> our %#llx (%#x bytes)\n", sp->name, va, va + b->size - 1,
	    (unsigned long long)kvtophys(b->mem), b->size);
	return true;
}

/*
 * A translation context of the SMMU with our tables (stage 1, 39 bits, 4K
 * pages), then a stream sent through it; every write is a single str for
 * the hypervisor's emulation. True if the global registers read back as
 * written (the hypervisor's to grant).
 */
static void
gpu_smmu_context(vm_offset_t smmu, uint32_t pg, uint32_t npg, uint32_t context, struct gpu_space *sp, uint32_t sctlr)
{
	uint32_t cb = (npg + context) * pg;
	uint64_t root = sp->phys[0] | ((uint64_t)context << 48);
	for (unsigned i = 0; i < sp->count; ++i)
		gpu_to_memory((vm_offset_t)sp->table[i], 4096);
	gpu_wr(smmu, cb + SMMU_CB_SCTLR, 0);
	gpu_wr(smmu, pg + SMMU_CBA2R(context), SMMU_CBA2R_64_BIT);
	gpu_wr(smmu, pg + 4u * context, SMMU_CBAR_STAGE_1);
	gpu_wr(smmu, cb + SMMU_CB_TCR2, SMMU_TCR2);
	gpu_wr(smmu, cb + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	gpu_wr(smmu, cb + SMMU_CB_TTBR0, (uint32_t)root);
	gpu_wr(smmu, cb + SMMU_CB_TTBR0 + 4u, (uint32_t)(root >> 32));
	gpu_wr(smmu, cb + SMMU_CB_TTBR1, 0);
	gpu_wr(smmu, cb + SMMU_CB_TTBR1 + 4u, 0);
	gpu_wr(smmu, cb + SMMU_CB_MAIR0, SMMU_MAIR0);
	gpu_wr(smmu, cb + SMMU_CB_MAIR1, 0);
	gpu_wr(smmu, cb + SMMU_CB_FSR, SMMU_FSR_ALL);
	gpu_wr(smmu, cb + SMMU_CB_TLBIASID, context);
	gpu_wr(smmu, cb + SMMU_CB_TLBSYNC, 1);
	for (unsigned i = 0; i < 100 && (gpu_rd(smmu, cb + SMMU_CB_TLBSTATUS) & 1u); ++i)
		delay(10);
	gpu_wr(smmu, cb + SMMU_CB_SCTLR, SMMU_SCTLR_ON | sctlr);
}

static bool
gpu_smmu_stream(vm_offset_t smmu, uint32_t pg, uint32_t npg, uint32_t match, uint32_t stream, uint32_t mask,
    uint32_t context, struct gpu_space *sp)
{
	uint32_t cb = (npg + context) * pg;
	uint64_t root = sp->phys[0] | ((uint64_t)context << 48);
	uint32_t want = SMMU_SMR_VALID | (mask << 16) | stream;
	uint32_t smr, s2cr, cbar, sctlr, tt0, tt1;
	gpu_wr(smmu, SMMU_S2CR(match), context);
	gpu_wr(smmu, SMMU_SMR(match), want);
	smr = gpu_rd(smmu, SMMU_SMR(match));
	s2cr = gpu_rd(smmu, SMMU_S2CR(match));
	cbar = gpu_rd(smmu, pg + 4u * context);
	sctlr = gpu_rd(smmu, cb + SMMU_CB_SCTLR);
	tt0 = gpu_rd(smmu, cb + SMMU_CB_TTBR0);
	tt1 = gpu_rd(smmu, cb + SMMU_CB_TTBR0 + 4u);
	printf("SWEET GPU:   read back smr%u=%#x s2cr%u=%#x cbar%u=%#x sctlr=%#x ttbr0=%#x:%#x tcr=%#x fsr=%#x sgfsr=%#x\n",
	    match, smr, match, s2cr, context, cbar, sctlr, tt1, tt0,
	    gpu_rd(smmu, cb + SMMU_CB_TCR), gpu_rd(smmu, cb + SMMU_CB_FSR), gpu_rd(smmu, SMMU_SGFSR));
	return (smr == want && (s2cr & 0x300ffu) == context && (cbar & 0x30000u) == (SMMU_CBAR_STAGE_1 & 0x30000u) &&
	       (sctlr & 1u) && tt0 == (uint32_t)root && tt1 == (uint32_t)(root >> 32)) ? true : false;
}

/* A context's fault record; true if it refused something. */
static bool
gpu_smmu_faulted(vm_offset_t smmu, uint32_t pg, uint32_t npg, uint32_t context, const char *who)
{
	uint32_t cb = (npg + context) * pg, fsr = gpu_rd(smmu, cb + SMMU_CB_FSR);
	if (!(fsr & SMMU_FSR_ALL))
		return false;
	printf("SWEET GPU:   THE SMMU REFUSED THE %s: fsr %#x far %#x:%#x fsynr0 %#x\n", who, fsr,
	    gpu_rd(smmu, cb + SMMU_CB_FAR + 4u), gpu_rd(smmu, cb + SMMU_CB_FAR), gpu_rd(smmu, cb + SMMU_CB_FSYNR0));
	return true;
}

static void
gpu_gmu_reg(vm_offset_t gmu, const char *name, uint32_t off)
{
	printf("SWEET GPU:   gmu %-22s %#07x = %#010x\n", name, off, gpu_rd(gmu, off));
}

#define GMU_MAX_BLOCKS 12
static const uint8_t *gmu_fw;
static uint32_t gmu_fw_size, gmu_log_va, gmu_hfi_va;
static struct gpu_block gmu_blocks[GMU_MAX_BLOCKS], gmu_log, gmu_hfi;
static unsigned gmu_nblocks;
static bool gmu_smmu_on, gmu_started;
static vm_offset_t gmu_va, smmu_va, gmu_rscc; /* for the teardown */
static uint32_t smmu_pg, smmu_npg;

/* ---- the RPMh command database (Linux drivers/soc/qcom/cmd-db.c) ---- */
static const volatile uint8_t *cmd_db;

static uint32_t
cmd_db_le(uint32_t at, unsigned bytes)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < bytes; ++i)
		v |= (uint32_t)cmd_db[at + i] << (8 * i);
	return v;
}

/* RPMh's address of a resource (0: none) and, for a rail, its levels (vendor rpmh_arc_cmds). */
static uint32_t
gpu_cmd_db(const char *name, uint16_t *lvl, unsigned *nlvl)
{
	char id[8] = { 0 };
	if (nlvl)
		*nlvl = 0;
	if (!cmd_db || cmd_db[4] != 0xdb || cmd_db[5] != 0x30 || cmd_db[6] != 0x03 || cmd_db[7] != 0x0c)
		return 0;
	for (unsigned i = 0; i < sizeof(id) && name[i]; ++i)
		id[i] = name[i];
	for (uint32_t s = 0; s < 8; ++s) {
		uint32_t slave = 8u + 16u * s, hoff = cmd_db_le(slave + 2, 2), doff = cmd_db_le(slave + 4, 2);
		uint32_t cnt = cmd_db_le(slave + 6, 2);
		if (cmd_db_le(slave, 2) == 0 || cnt > 512)
			continue;
		for (uint32_t i = 0; i < cnt; ++i) {
			uint32_t e = CMD_DB_DATA + hoff + CMD_DB_ENTRY * i;
			bool same = (e + CMD_DB_ENTRY <= CMD_DB_SIZE) ? true : false;
			uint32_t len, d;
			for (unsigned k = 0; k < sizeof(id) && same; ++k)
				same = (cmd_db[e + k] == (uint8_t)id[k]) ? true : false;
			if (!same)
				continue;
			len = cmd_db_le(e + 20, 2);
			d = CMD_DB_DATA + doff + cmd_db_le(e + 22, 2);
			if (lvl && nlvl && len >= 2 && len <= 32 && d + len <= CMD_DB_SIZE) {
				unsigned n;
				for (unsigned k = 0; k < len / 2; ++k)
					lvl[k] = (uint16_t)cmd_db_le(d + 2 * k, 2);
				for (n = 1; n < len / 2; ++n)
					if (lvl[n - 1] != 0 && lvl[n] == 0)
						break;
				*nlvl = n;
			}
			return cmd_db_le(e + 16, 4);
		}
	}
	return 0;
}

/* A rail vote: the first level >= vlvl, its index, and the memory rail's index that covers it. */
static bool
gpu_rail_vote(uint16_t vlvl, const uint16_t *pri, unsigned npri, const uint16_t *mx, unsigned nmx, uint32_t *vote)
{
	unsigned k, j;
	for (k = 0; k < npri && pri[k] < vlvl; ++k)
		;
	if (k == npri || nmx == 0)
		return false;
	for (j = 0; j + 1 < nmx && mx[j] < pri[k]; ++j)
		;
	*vote = ((uint32_t)pri[k] << 16) | (j << 8) | k;
	return true;
}

/* PM4 packet header parity (Linux adreno_pm4 PM4_PARITY) */
static uint32_t
gpu_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

/* ---- TrustZone: one SMC, for the zap shader of step 20 (HackiOS's scm(), convention qcom_scm-smc.c) ---- */
/*
 * The hypervisor passes the SMC on to TrustZone. A call that may be preempted
 * returns SCM_INTERRUPTED and is made again with x0 = SCM_INTERRUPTED, the
 * other words unchanged, until it completes (Linux __scm_smc_do). Result: 0 or
 * TrustZone's own error, plus one word of answer.
 */
static int32_t
gpu_scm(const char *what, uint64_t function, uint64_t args, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *answer)
{
	uint64_t session = 0;
	printf("SWEET GPU:   about to ask TrustZone %s: smc %#llx, args %#llx (%#llx, %#llx, %#llx)\n", what,
	    (unsigned long long)function, (unsigned long long)args, (unsigned long long)a0, (unsigned long long)a1,
	    (unsigned long long)a2);
	for (int i = 0; i < 100; ++i) {
		UINT64 r[8];
		r[0] = (i == 0) ? function : SCM_INTERRUPTED;
		r[1] = args;
		r[2] = a0;
		r[3] = a1;
		r[4] = a2;
		r[5] = 0;
		r[6] = session;
		r[7] = 0;
		apollo_smc(r);
		session = r[6];
		if (r[0] != SCM_INTERRUPTED) {
			*answer = r[1];
			printf("SWEET GPU:   TrustZone returns %d, answer %#llx%s\n", (int32_t)r[0], (unsigned long long)r[1],
			    i ? " (after resuming an interrupted call)" : "");
			return (int32_t)r[0];
		}
	}
	printf("SWEET GPU:   TrustZone keeps saying the call was interrupted\n");
	return -1;
}

/*
 * The zap shader's metadata (a650_zap.mdt = the 32-bit ELF header, its program
 * headers and the hash segment) copied into a page-aligned, physically contiguous piece of our memory
 * (TrustZone reads memory); its physical address goes back in *phys. The file is checked to be exactly the
 * header, the headers and the hashes (HackiOS zap_header, Linux
 * qcom_mdt_pas_init): a 32-bit ELF, program headers from byte 52, 32 bytes
 * each, the hash segment's size the file size of program header 1.
 */
static bool
gpu_zap_header(const uint8_t *mdt, unsigned int size, uint64_t *phys)
{
	vm_offset_t page;
	uint32_t segments, hashes;
	if (size < 52u + 2u * 32u || mdt[0] != 0x7f || mdt[1] != 'E' || mdt[2] != 'L' || mdt[3] != 'F' || mdt[4] != 1) {
		printf("SWEET GPU: step 20: zap-metadata is not a 32-bit ELF header (%02x %02x %02x %02x)\n", mdt[0], mdt[1],
		    mdt[2], mdt[3]);
		return false;
	}
	segments = mdt[44] | ((uint32_t)mdt[45] << 8);
	hashes = mdt[100] | ((uint32_t)mdt[101] << 8) | ((uint32_t)mdt[102] << 16) | ((uint32_t)mdt[103] << 24);
	printf("SWEET GPU: step 20: %u ELF segments, header and table %u bytes, hashes %u, file %u\n", segments,
	    52u + 32u * segments, hashes, size);
	if (segments < 2 || 52u + 32u * segments + hashes != size) {
		printf("SWEET GPU: step 20: not the header followed by the hashes alone: not what TrustZone takes\n");
		return false;
	}
	page = gpu_mem_alloc(size, true);
	if (!page) {
		printf("SWEET GPU: step 20: no memory for the zap header\n");
		return false;
	}
	for (unsigned int i = 0; i < size; ++i)
		((volatile uint8_t *)page)[i] = mdt[i];
	gpu_to_memory(page, ROUND_TO_PAGES(size));
	*phys = kvtophys(page);
	return true;
}

/* TrustZone is to give up the zap shader loaded here (vendor kgsl_zap_shader_unload). */
static bool
gpu_zap_unload(void)
{
	uint64_t answer = ~0ull;
	/* like Linux's qcom_scm_pas_shutdown, a call that may be preempted (no SCM_FAST) */
	return (gpu_scm("give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0,
	    &answer) == 0 && answer == 0) ? true : false;
}

/* ---- the core's ring (steps 21-22) ---- */
static void
ring_pkt7(volatile uint32_t *ring, uint32_t *at, uint32_t op, uint32_t words)
{
	ring[(*at)++] = (7u << 28) | words | (gpu_parity(words) << 15) | (op << 16) | (gpu_parity(op) << 23);
}

/* One register of the core written by command, in order with the others. */
static void
ring_reg(volatile uint32_t *ring, uint32_t *at, uint32_t reg, uint32_t val)
{
	ring[(*at)++] = (4u << 28) | 1u | (gpu_parity(1) << 7) | (reg << 8) | (gpu_parity(reg) << 27);
	ring[(*at)++] = val;
}

/* After pages are added to a live context: its tables out to memory, its TLB forgotten. */
static void
gpu_smmu_refresh(vm_offset_t smmu, uint32_t pg, uint32_t npg, uint32_t context, struct gpu_space *sp)
{
	uint32_t cb = (npg + context) * pg;
	for (unsigned i = 0; i < sp->count; ++i)
		gpu_to_memory((vm_offset_t)sp->table[i], 4096);
	gpu_wr(smmu, cb + SMMU_CB_TLBIASID, context);
	gpu_wr(smmu, cb + SMMU_CB_TLBSYNC, 1);
	for (unsigned i = 0; i < 100 && (gpu_rd(smmu, cb + SMMU_CB_TLBSTATUS) & 1u); ++i)
		delay(10);
}

/* Ring words [from, to) out to memory, the core told, and waited for: read to the end, idle, no fault. */
static bool
ring_run(vm_offset_t gpu, vm_offset_t smmu, uint32_t pg, uint32_t npg, vm_offset_t ring_mem, uint32_t from, uint32_t to,
    const char *what)
{
	uint32_t rptr = ~0u, status = ~0u, fault;
	bool refused;
	gpu_to_memory(ring_mem + 4u * from, 4u * (to - from));
	printf("SWEET GPU:   ring words %u..%u: %s\n", from, to - 1, what);
	gpu_wr(gpu, GPU_CP_RB_WPTR, to);
	for (unsigned i = 0; i < 20000 && (rptr != to || (status & GPU_BUSY)); ++i) {
		delay(100);
		rptr = gpu_rd(gpu, GPU_CP_RB_RPTR);
		status = gpu_rd(gpu, GPU_RBBM_STATUS);
	}
	fault = gpu_rd(gpu, GPU_CP_HW_FAULT);
	refused = gpu_smmu_faulted(smmu, pg, npg, CORE_CONTEXT, "CORE");
	printf("SWEET GPU:   rptr %u of %u, status %#x int %#x cp fault %#x cp int %#x protect %#x\n", rptr, to, status,
	    gpu_rd(gpu, GPU_RBBM_INT_0_STATUS), fault, gpu_rd(gpu, GPU_CP_INTERRUPT_STATUS), gpu_rd(gpu, GPU_CP_PROTECT_STATUS));
	return (rptr == to && !(status & GPU_BUSY) && !fault && !refused) ? true : false;
}

/* One band of solid colour by the 2D engine (Mesa fd6_clear_surface; HackiOS ring_fill). */
static void
ring_fill(volatile uint32_t *ring, uint32_t *at, uint32_t top, uint32_t bottom, uint32_t r, uint32_t g, uint32_t b)
{
	ring_reg(ring, at, REG_GRAS_2D_DST_TL, top << 16);
	ring_reg(ring, at, REG_GRAS_2D_DST_BR, (TARGET_W - 1u) | ((bottom - 1u) << 16));
	ring_reg(ring, at, REG_RB_2D_SOLID_C0, r);
	ring_reg(ring, at, REG_RB_2D_SOLID_C0 + 1u, g);
	ring_reg(ring, at, REG_RB_2D_SOLID_C0 + 2u, b);
	ring_reg(ring, at, REG_RB_2D_SOLID_C0 + 3u, 255);
	ring_reg(ring, at, REG_RB_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_reg(ring, at, REG_GRAS_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_reg(ring, at, REG_SP_2D_DST_FORMAT, 1u | (FMT6_8_8_8_8_UNORM << 3) | (0xfu << 12));
	ring_reg(ring, at, REG_RB_2D_PIXEL_CNTL, 0);
	ring_reg(ring, at, REG_RB_2D_DST_INFO, DST_BGRA_LINEAR);
	ring_reg(ring, at, REG_RB_2D_DST, CORE_TARGET_VA);
	ring_reg(ring, at, REG_RB_2D_DST + 1u, 0);
	ring_reg(ring, at, REG_RB_2D_DST_PITCH, TARGET_ROW >> 6);
	ring_pkt7(ring, at, CP_WAIT_FOR_IDLE, 0);
	ring_pkt7(ring, at, CP_BLIT, 1);
	ring[(*at)++] = CP_BLIT_OP_SCALE;
}

/* ---- HFI: messages through the queues of step 13 ---- */
static volatile uint32_t *gmu_hfi_q;          /* uncached view of the HFI page */
static uint32_t hfi_qoff, hfi_seq;
static bool gmu_talks, gmu_tables, gx_asked, rsc_programmed, gmu_log_shown, core_smmu_on, core_started, zap_loaded;
static vm_offset_t gpu_core;                 /* the 3D core's registers, for the teardown */

static volatile uint32_t *
hfi_header(unsigned q)
{
	return gmu_hfi_q + 6 + 12 * q;
}

static volatile uint32_t *
hfi_data(unsigned q)
{
	return gmu_hfi_q + hfi_qoff / 4u + HFI_QUEUE_WORDS * q;
}

/* Everything waiting in GMU queue q; true if the answer to message seq was among it. */
static bool
hfi_drain(unsigned q, uint32_t seq, uint32_t *answer, unsigned answer_words)
{
	volatile uint32_t *h = hfi_header(q), *data = hfi_data(q);
	bool found = false;
	for (unsigned n = 0; n < 64 && h[10] != h[11]; ++n) {
		uint32_t at = h[10] % HFI_QUEUE_WORDS, words = (data[at] >> 8) & 0xffu, msg[24] = { 0 };
		if (words == 0) {
			printf("SWEET GPU:   queue %u: empty message at %u (write %u): skipped\n", q, at, h[11]);
			h[10] = h[11];
			break;
		}
		for (uint32_t i = 0; i < words; ++i) {
			if (i < 23)
				msg[i] = data[at];
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		h[10] = ((at + 3u) & ~3u) % HFI_QUEUE_WORDS;
		if (((msg[0] >> 16) & 0xfu) == HFI_ACK && ((msg[1] >> 20) & 0xfffu) == seq) {
			for (unsigned i = 0; i < answer_words; ++i)
				answer[i] = msg[i];
			found = true;
		} else if ((msg[0] & 0xffu) == HFI_F2H_ERR) {
			msg[23] = 0;
			printf("SWEET GPU:   THE GMU REPORTS AN ERROR: %u %u \"%s\"\n", msg[1] >> 16, msg[1] & 0xffffu,
			    (const char *)&msg[2]);
		} else if ((msg[0] & 0xffu) == HFI_F2H_DEBUG) {
			printf("SWEET GPU:   gmu debug: type %u time %#x data %#x\n", msg[1], msg[2], msg[3]);
		} else {
			printf("SWEET GPU:   queue %u, %u words: %#x %#x %#x %#x %#x %#x\n", q, words, msg[0], msg[1], msg[2],
			    msg[3], msg[4], msg[5]);
		}
	}
	return found;
}

/* The tail of the GMU's log (16 bytes an event; pwr_col_cp_resp [15:0] is where it writes next). */
static void
gmu_log_tail(void)
{
	uint32_t next, events = GMU_LOG_SIZE / 16u;
	const volatile uint32_t *log;
	if (gmu_log_shown)
		return;
	gmu_log_shown = true;
	next = gpu_rd(gmu_va, GMU_PWR_COL_CP_RESP) & 0xffffu;
	log = (const volatile uint32_t *)gmu_log.mem;
	gpu_to_memory(gmu_log.mem, GMU_LOG_SIZE);
	printf("SWEET GPU:   gmu log: next event %u; the last %u (event, time, two values):\n", next, GMU_LOG_TAIL);
	for (uint32_t i = next > GMU_LOG_TAIL ? next - GMU_LOG_TAIL : 0; i < next && i < events; ++i)
		printf("SWEET GPU:     %u: %#x %#x %#x %#x\n", i, log[4 * i], log[4 * i + 1], log[4 * i + 2], log[4 * i + 3]);
	printf("SWEET GPU:   gmu fw_init_result %#x gmu2host %#x ao_host_irq %#x rpmh_power_state %#x sptprac %#x ahb_fence %#x\n",
	    gpu_rd(gmu_va, GMU_CM3_FW_INIT_RESULT), gpu_rd(gmu_va, GMU_GMU2HOST_INTR_INFO),
	    gpu_rd(gmu_va, GMU_AO_HOST_IRQ_STATUS), gpu_rd(gmu_va, GMU_RPMH_POWER_STATE),
	    gpu_rd(gmu_va, GMU_SPTPRAC_PWR_CLK), gpu_rd(gmu_va, GMU_AHB_FENCE_STATUS));
}

/* Send msg (its first word is made here) and wait up to 1 s for the GMU's verdict (0: done). */
static bool
hfi_send(const char *what, uint32_t id, uint32_t *msg, uint32_t words)
{
	volatile uint32_t *h = hfi_header(0), *data = hfi_data(0);
	uint32_t padded = (words + 3u) & ~3u, wr = h[11] % HFI_QUEUE_WORDS, rd = h[10] % HFI_QUEUE_WORDS;
	uint32_t room = wr >= rd ? HFI_QUEUE_WORDS - (wr - rd) : rd - wr, answer[4] = { 0 };
	uint32_t seq = ++hfi_seq & 0xfffu;
	bool got = false;
	if (room <= padded) {
		printf("SWEET GPU:   no room for %s: read %u write %u\n", what, rd, wr);
		return false;
	}
	msg[0] = id | (words << 8) | (seq << 20);
	printf("SWEET GPU:   send %s: message %u, %u words, seq %u\n", what, id, words, seq);
	for (uint32_t i = 0; i < padded; ++i) {
		data[wr] = i < words ? msg[i] : 0xfafafafau;
		wr = (wr + 1) % HFI_QUEUE_WORDS;
	}
	apollo_dsb_sy();
	h[11] = wr;
	apollo_dsb_sy();
	gpu_wr(gmu_va, GMU_HOST2GMU_INTR_SET, 1);
	for (unsigned i = 0; i < 10000 && !got; ++i) {
		delay(100);
		if (gpu_rd(gmu_va, GMU_GMU2HOST_INTR_INFO) & 1u) {
			gpu_wr(gmu_va, GMU_GMU2HOST_INTR_CLR, 1);
			got = hfi_drain(1, seq, answer, 4);
		}
	}
	(void)hfi_drain(2, ~0u, NULL, 0);
	if (!got || answer[2] != 0) {
		printf("SWEET GPU:   the GMU %s %s (verdict %#x); fw_init_result %#x gmu2host %#x queue0 read %u write %u\n",
		    got ? "REFUSES" : "DOES NOT ANSWER", what, answer[2], gpu_rd(gmu_va, GMU_CM3_FW_INIT_RESULT),
		    gpu_rd(gmu_va, GMU_GMU2HOST_INTR_INFO), h[10], h[11]);
		gmu_log_tail();
		return false;
	}
	printf("SWEET GPU:   the GMU accepts %s\n", what);
	return true;
}

/* (xnu 0163) step 19's registers of the 3D core (Linux a6xx hw_init, a650) */
static void
core_hw_regs(vm_offset_t gpu)
{
	static const uint16_t addr_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
		                               0xb601, 0xf810 };
	gpu_wr(gpu, GPU_GBIF_HALT, 0);
	gpu_wr(gpu, GPU_RBBM_GBIF_HALT, 0);
	gpu_wr(gpu, GPU_SECVID_TSB_CNTL, 0);
	gpu_wr(gpu, GPU_SECVID_TSB_TRUSTED_BASE, 0);
	gpu_wr(gpu, GPU_SECVID_TSB_TRUSTED_BASE + 4u, 0);
	gpu_wr(gpu, GPU_SECVID_TSB_TRUSTED_SIZE, 0);
	for (unsigned i = 0; i < sizeof(addr_modes) / sizeof(addr_modes[0]); ++i)
		gpu_wr(gpu, GPU_WORD(addr_modes[i]), 1);           /* 64-bit addresses, every block */
	for (unsigned i = 0; i < 4; ++i)
		gpu_wr(gpu, GPU_GBIF_QSB_SIDE0 + 4u * i, 0x00071620u);
	gpu_wr(gpu, GPU_RBBM_GBIF_CLIENT_QOS, 3);
	gpu_wr(gpu, GPU_RBBM_PERFCTR_BUSY_MASKED, 0xffffffffu);
	gpu_wr(gpu, GPU_UCHE_WRITE_RANGE_MAX, 0xffffffc0u);
	gpu_wr(gpu, GPU_UCHE_WRITE_RANGE_MAX + 4u, 0x0001ffffu);
	gpu_wr(gpu, GPU_UCHE_TRAP_BASE, 0xfffff000u);
	gpu_wr(gpu, GPU_UCHE_TRAP_BASE + 4u, 0x0001ffffu);
	gpu_wr(gpu, GPU_UCHE_WRITE_THRU_BASE, 0xfffff000u);
	gpu_wr(gpu, GPU_UCHE_WRITE_THRU_BASE + 4u, 0x0001ffffu);
	gpu_wr(gpu, GPU_UCHE_FILTER_CNTL, 0x804);
	gpu_wr(gpu, GPU_UCHE_CACHE_WAYS, 0x4);
	gpu_wr(gpu, GPU_CP_ROQ_THRESHOLDS_2, 0x02000140u);
	gpu_wr(gpu, GPU_CP_ROQ_THRESHOLDS_1, 0x8040362cu);
	gpu_wr(gpu, GPU_CP_MEM_POOL_SIZE, 128);
	gpu_wr(gpu, GPU_PC_DBG_ECO_CNTL, 0x00300200u);
	gpu_wr(gpu, GPU_CP_AHB_CNTL, 1);
	gpu_wr(gpu, GPU_RBBM_PERFCTR_CNTL, 1);
	/* UBWC (a6xx_set_ubwc_config, a650: highest bank bit 16 as the DT says) */
	gpu_wr(gpu, GPU_RB_NC_MODE_CNTL, (1u << 11) | (1u << 4) | (3u << 1));
	gpu_wr(gpu, GPU_TPL1_NC_MODE_CNTL, 3u << 1);
	gpu_wr(gpu, GPU_SP_NC_MODE_CNTL, (2u << 4) | (3u << 1));
	gpu_wr(gpu, GPU_UCHE_MODE_CNTL, 3u << 21);
	gpu_wr(gpu, GPU_RBBM_HANG_INT_CNTL, (1u << 30) | 0x1fffffu);
	gpu_wr(gpu, GPU_UCHE_CLIENT_PF, 1);
	gpu_wr(gpu, GPU_CP_APRIV_CNTL, GPU_APRIV_VALUE);
	gpu_wr(gpu, GPU_CP_SQE_INSTR_BASE, CORE_SQE_VA);
	gpu_wr(gpu, GPU_CP_SQE_INSTR_BASE + 4u, 0);
	gpu_wr(gpu, GPU_CP_RB_BASE, CORE_RING_VA);
	gpu_wr(gpu, GPU_CP_RB_BASE + 4u, 0);
	gpu_wr(gpu, GPU_CP_RB_CNTL, GPU_RB_CNTL_VALUE);
}

/* GPU speeds: apollo DT qcom,gpu-pwrlevels (speed bin 0) with their rail levels from kona-v2-gpu.dtsi
 * gpu_opp_table_v2; index 0 is off. */
static const struct {
	uint32_t khz;
	uint16_t vlvl;
} gpu_levels[] = {
	{ 0, 0 }, { 305000, 64 }, { 400000, 128 }, { 441600, 144 }, { 490000, 192 }, { 525000, 224 }, { 587000, 256 },
};
#define NGPU (sizeof(gpu_levels) / sizeof(gpu_levels[0]))

static uint8_t *sqe_file, *zap_mdt_file, *zap_seg_file;
static unsigned int sqe_file_size, zap_mdt_file_size, zap_seg_file_size;
static unsigned gpu_level = 1, gpu_zap_stage = 3;
static unsigned last_step;

/* The steps; returns after the teardown. (In sweet_gpu.c every variable is declared where first used; here they are
 * all declared first, so that every path to the teardown sees them initialised.) */
static void
sweet_gpu_probe(unsigned steps)
{
	vm_offset_t gcc = 0, cc = 0, gmu = 0, rscc = 0, smmu = 0, pdc = 0, gbif = 0, hfi_va = 0, cmd_db_va = 0, gpu = 0;
	uint32_t vote, ahb, mode, l, cx, gcfg, c0, ticks, idr0, idr1, pg = 0, npg = 0, nctx, nsmr, cb0, was0, was1, back0, back1;
	uint32_t nc_end, ctx = 0, result, fsr, gfx_addr, cx_addr, mx_addr, bus[4], granted, sptprac, pll0, sqe_ver, at = 0;
	uint32_t rptr, status, fault, srptr, sstatus, sfault, from, word;
	uint64_t t0, ns, hz, zap_header_pa = 0, zap_answer = ~0ull;
	bool ahb_ok, was_on, refused, srefused;
	unsigned wrong = 0, ngfx = 0, ncx = 0, nmx = 0, level;
	int32_t sig;
	uint16_t gfx_lvl[16], cx_lvl[16], mx_lvl[16];
	static uint32_t perf[HFI_PERF_WORDS], bw[HFI_BW_WORDS];
	static struct gpu_block core_ring, core_sqe, core_scratch, core_target;
	volatile uint32_t *hfi = NULL, *ring = NULL;
	uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, perf_vote[4];

	printf("SWEET GPU: Adreno 650 bring-up, steps 1..%u\n", steps);

	gcc = ml_io_map(GPU_GCC_PA, GPU_GCC_SIZE);
	if (!gcc) {
		printf("SWEET GPU: GCC map failed\n");
		return;
	}
	last_step = 1;
	printf("SWEET GPU: step 1: read GCC %#x GPU clocks\n", GPU_GCC_PA);
	gpu_cbcr(gcc, "gcc_gpu_cfg_ahb", GCC_GPU_CFG_AHB);
	gpu_cbcr(gcc, "gcc_gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX);
	gpu_cbcr(gcc, "gcc_gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX);
	gpu_cbcr(gcc, "gcc_gpu_iref_en", GCC_GPU_IREF_EN);
	vote = gpu_rd(gcc, GCC_VOTE_0);
	printf("SWEET GPU:   gcc vote %#x = %#010x  gpu_gpll0 %u, gpu_gpll0_div %u\n",
	    GCC_VOTE_0, vote, (vote >> 15) & 1u, (vote >> 16) & 1u);
	ahb = gpu_rd(gcc, GCC_GPU_CFG_AHB);
	ahb_ok = ((ahb & 1u) || (ahb & 2u)) ? true : false;
	if (steps < 2)
		goto done;

	last_step = 2;
	if (!ahb_ok) {
		printf("SWEET GPU: step 2: write GCC %#x |= 1 (gcc_gpu_cfg_ahb on)\n", GCC_GPU_CFG_AHB);
		gpu_wr(gcc, GCC_GPU_CFG_AHB, ahb | 1u);
		for (unsigned i = 0; i < 200 && CBCR_OFF(gpu_rd(gcc, GCC_GPU_CFG_AHB)); ++i)
			delay(10);
		ahb = gpu_rd(gcc, GCC_GPU_CFG_AHB);
		ahb_ok = (!CBCR_OFF(ahb) || (ahb & 2u)) ? true : false;
		printf("SWEET GPU: step 2: gcc_gpu_cfg_ahb now %#010x (%s)\n", ahb, ahb_ok ? "on" : "STILL OFF");
	} else {
		printf("SWEET GPU: step 2: gcc_gpu_cfg_ahb already on (%#010x); nothing written\n", ahb);
	}
	if (steps < 3)
		goto done;

	last_step = 3;
	if (!ahb_ok) {
		printf("SWEET GPU: step 3 refused: GPU config-bus clock not on\n");
		goto done;
	}
	cc = ml_io_map(GPU_CC_PA, GPU_CC_SIZE);
	if (!cc) {
		printf("SWEET GPU: GPUCC map failed\n");
		goto done;
	}
	printf("SWEET GPU: step 3: read GPUCC %#x\n", GPU_CC_PA);
	mode = gpu_rd(cc, GPUCC_PLL1);
	l = gpu_rd(cc, GPUCC_PLL1 + 4u);
	printf("SWEET GPU:   pll1 mode=%#010x (%s) L=%#x (%u MHz if locked)\n", mode,
	    (mode >> 31) ? "locked" : "not locked", l, (l & 0xffffu) * 192u / 10u);
	printf("SWEET GPU:   gmu rcg cmd=%#010x cfg=%#010x\n",
	    gpu_rd(cc, GPUCC_GMU_RCG), gpu_rd(cc, GPUCC_GMU_RCG + 4u));
	gpu_cbcr(cc, "gpu_cc_ahb", GPUCC_AHB);
	gpu_cbcr(cc, "gpu_cc_cxo", GPUCC_CXO);
	gpu_cbcr(cc, "gpu_cc_cxo_aon", GPUCC_CXO_AON);
	gpu_cbcr(cc, "gpu_cc_cx_gmu", GPUCC_CX_GMU);
	gpu_cbcr(cc, "gpu_cc_gx_gmu", GPUCC_GX_GMU);
	gpu_cbcr(cc, "gpu_cc_cx_snoc_dvm", GPUCC_CX_SNOC_DVM);
	gpu_cbcr(cc, "gpu_cc_hlos1_vote_smmu", GPUCC_SMMU_VOTE);
	cx = gpu_rd(cc, GPUCC_CX_GDSC);
	printf("SWEET GPU:   cx gdsc %#x = %#010x  %s%s\n", GPUCC_CX_GDSC, cx,
	    (cx >> 31) ? "powered" : "off", (cx & 1u) ? ", sw-collapse requested" : "");
	printf("SWEET GPU:   gx domain %#x = %#010x, cx hw ctrl %#x = %#010x\n",
	    GPUCC_GX_DOMAIN, gpu_rd(cc, GPUCC_GX_DOMAIN), GPUCC_CX_HW_CTRL, gpu_rd(cc, GPUCC_CX_HW_CTRL));
	if (steps < 4)
		goto done;

	/* ---- step 4: CX power ---- */
	last_step = 4;
	if (cx & GDSC_HW_CONTROL) {
		printf("SWEET GPU: step 4 refused: CX GDSC is under hardware control (%#010x)\n", cx);
		goto done;
	}
	was_on = gpu_cx_on(cc);
	if (!was_on) {
		printf("SWEET GPU: step 4: write GPUCC %#x &= ~1 (CX power switch on)\n", GPUCC_CX_GDSC);
		gpu_wr(cc, GPUCC_CX_GDSC, gpu_rd(cc, GPUCC_CX_GDSC) & ~GDSC_SW_COLLAPSE);
		for (unsigned i = 0; i < 500 && !gpu_cx_on(cc); ++i)
			delay(100);
	}
	printf("SWEET GPU: step 4: cx gdsc = %#010x, hw ctrl = %#010x: CX %s%s\n",
	    gpu_rd(cc, GPUCC_CX_GDSC), gpu_rd(cc, GPUCC_CX_HW_CTRL),
	    gpu_cx_on(cc) ? "ON" : "DOES NOT COME ON", was_on ? " (was already on)" : "");
	if (!gpu_cx_on(cc) || steps < 5)
		goto collapse;

	/* ---- step 5: clocks ---- */
	last_step = 5;
	printf("SWEET GPU: step 5: GCC votes gpu_gpll0 + gpu_gpll0_div (%#x bits 15,16), GPU bus clocks\n", GCC_VOTE_0);
	gpu_wr(gcc, GCC_VOTE_0, gpu_rd(gcc, GCC_VOTE_0) | (1u << 15) | (1u << 16));
	if (!gpu_clock_on(gcc, "gcc_gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX, false) ||
	    !gpu_clock_on(gcc, "gcc_gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX, false) ||
	    !gpu_clock_on(gcc, "gcc_ddrss_gpu_axi", GCC_DDRSS_GPU_AXI, false))
		goto collapse;
	gcfg = gpu_rd(cc, GPUCC_GMU_RCG + 4u);
	printf("SWEET GPU: step 5: gmu clock source %#010x -> 200 MHz (%#x)\n", gcfg, GMU_RCG_200MHZ);
	if ((gcfg & RCG_CFG_MASK) != GMU_RCG_200MHZ) {
		gpu_wr(cc, GPUCC_GMU_RCG + 4u, (gcfg & ~RCG_CFG_MASK) | GMU_RCG_200MHZ);
		gpu_wr(cc, GPUCC_GMU_RCG, gpu_rd(cc, GPUCC_GMU_RCG) | RCG_UPDATE);
		for (unsigned i = 0; i < 100 && (gpu_rd(cc, GPUCC_GMU_RCG) & RCG_UPDATE); ++i)
			delay(10);
		if (gpu_rd(cc, GPUCC_GMU_RCG) & RCG_UPDATE) {
			printf("SWEET GPU: step 5: gmu clock source did not switch (cmd %#010x)\n",
			    gpu_rd(cc, GPUCC_GMU_RCG));
			goto collapse;
		}
	}
	printf("SWEET GPU: step 5: gmu rcg cmd=%#010x cfg=%#010x; GPUCC branches on\n",
	    gpu_rd(cc, GPUCC_GMU_RCG), gpu_rd(cc, GPUCC_GMU_RCG + 4u));
	if (!gpu_clock_on(cc, "gpu_cc_cxo", GPUCC_CXO, true) ||
	    !gpu_clock_on(cc, "gpu_cc_ahb", GPUCC_AHB, false) ||
	    !gpu_clock_on(cc, "gpu_cc_cx_apb", GPUCC_CX_APB, false) ||
	    !gpu_clock_on(cc, "gpu_cc_cx_snoc_dvm", GPUCC_CX_SNOC_DVM, false) ||
	    !gpu_clock_on(cc, "gpu_cc_cx_gmu", GPUCC_CX_GMU, true) ||
	    !gpu_clock_on(cc, "gpu_cc_hlos1_vote_smmu", GPUCC_SMMU_VOTE, false))
		goto collapse;
	if (steps < 6)
		goto collapse;

	/* ---- step 6: the first words from the GMU ---- */
	last_step = 6;
	gmu = ml_io_map(GMU_PA, GMU_SIZE);
	gmu_va = gmu;
	if (!gmu) {
		printf("SWEET GPU: GMU map failed\n");
		goto collapse;
	}
	printf("SWEET GPU: step 6: read GMU %#x\n", GMU_PA);
	gpu_gmu_reg(gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	gpu_gmu_reg(gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	gpu_gmu_reg(gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	gpu_gmu_reg(gmu, "cm3_cfg", GMU_CM3_CFG);
	gpu_gmu_reg(gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	gpu_gmu_reg(gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	gpu_gmu_reg(gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	gpu_gmu_reg(gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK);
	gpu_gmu_reg(gmu, "ao_host_irq_status", GMU_AO_HOST_IRQ_STATUS);
	gpu_gmu_reg(gmu, "ao_gpu_cx_busy", GMU_AO_GPU_CX_BUSY);
	gpu_gmu_reg(gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	t0 = now_ns();
	c0 = gpu_rd(gmu, GMU_AO_COUNTER_L);
	delay(10000);
	ticks = gpu_rd(gmu, GMU_AO_COUNTER_L) - c0;
	ns = now_ns() - t0;
	hz = ns ? (uint64_t)ticks * 1000000000ull / ns : 0;
	printf("SWEET GPU: step 6: GMU always-on counter %#x:%#x counts %llu Hz: %s\n",
	    gpu_rd(gmu, GMU_AO_COUNTER_H), gpu_rd(gmu, GMU_AO_COUNTER_L), (unsigned long long)hz,
	    (hz > 18000000ull && hz < 20500000ull) ? "the 19.2 MHz crystal, THE GPU BLOCK IS ALIVE" :
	    "not 19.2 MHz (the counter may need GMU firmware)");
	if (steps < 7)
		goto collapse;

	/* ---- step 7: the GMU's RPMh channel ---- */
	last_step = 7;
	rscc = ml_io_map(RSCC_PA, RSCC_SIZE);
	gmu_rscc = rscc;
	if (!rscc) {
		printf("SWEET GPU: RSCC map failed\n");
		goto collapse;
	}
	printf("SWEET GPU: step 7: read RSCC %#x\n", RSCC_PA);
	printf("SWEET GPU:   rscc drv_config=%#x rsc_status0=%#x pdc_slave_id=%#x seq_busy=%#x seq_mem0=%#x tcs0_status=%#x; gmu rscc_control_ack=%#x\n",
	    gpu_rd(rscc, RSCC_DRV_CONFIG), gpu_rd(rscc, RSCC_RSC_STATUS0_DRV0),
	    gpu_rd(rscc, RSCC_PDC_SLAVE_ID_DRV0), gpu_rd(rscc, RSCC_SEQ_BUSY_DRV0),
	    gpu_rd(rscc, RSCC_SEQ_MEM_0_DRV0), gpu_rd(rscc, RSCC_TCS0_DRV0_STATUS),
	    gpu_rd(gmu, GMU_RSCC_CONTROL_ACK));
	if (steps < 8)
		goto collapse;

	/* ---- step 8: the SMMU's global registers (answered by the hypervisor) ---- */
	last_step = 8;
	smmu = ml_io_map(SMMU_PA, SMMU_SIZE);
	if (!smmu) {
		printf("SWEET GPU: SMMU map failed\n");
		goto collapse;
	}
	printf("SWEET GPU: step 8: read SMMU %#x ID registers\n", SMMU_PA);
	idr0 = gpu_rd(smmu, SMMU_IDR0);
	idr1 = gpu_rd(smmu, SMMU_IDR1);
	pg = (idr1 >> 31) ? 0x10000u : 0x1000u;
	npg = 2u << ((idr1 >> 28) & 7u);
	smmu_va = smmu;
	smmu_pg = pg;
	smmu_npg = npg;
	nctx = idr1 & 0xffu;
	nsmr = idr0 & 0xffu;
	printf("SWEET GPU:   smmu idr0=%#x idr1=%#x idr2=%#x idr7=%#x scr0=%#x sgfsr=%#x: %u stream matches, %u contexts, %u pages of %#x\n",
	    idr0, idr1, gpu_rd(smmu, SMMU_IDR2), gpu_rd(smmu, SMMU_IDR7), gpu_rd(smmu, SMMU_SCR0),
	    gpu_rd(smmu, SMMU_SGFSR), nsmr, nctx, npg, pg);
	if (2u * npg * pg != SMMU_SIZE) {
		printf("SWEET GPU: step 8: that is %#x of registers, the DT says %#x: not reading further\n",
		    2u * npg * pg, SMMU_SIZE);
		goto collapse;
	}
	for (uint32_t i = 0; i < nsmr && i < 16; ++i)
		printf("SWEET GPU:   smmu smr%u=%#010x s2cr%u=%#010x\n", i, gpu_rd(smmu, SMMU_SMR(i)),
		    i, gpu_rd(smmu, SMMU_S2CR(i)));
	if (steps < 9)
		goto collapse;

	/* ---- step 9: context kinds (SMMU page 1) and translation context 0 ---- */
	last_step = 9;
	for (uint32_t i = 0; i < nctx && i < 16; ++i) {
		printf("SWEET GPU: step 9: read cbar%u at %#x\n", i, SMMU_PA + pg + 4u * i);
		printf("SWEET GPU:   smmu cbar%u=%#010x\n", i, gpu_rd(smmu, pg + 4u * i));
	}
	cb0 = npg * pg;
	printf("SWEET GPU: step 9: read SMMU context 0 at %#x\n", SMMU_PA + cb0);
	printf("SWEET GPU:   cb0 sctlr=%#x ttbr0=%#x:%#x tcr=%#x fsr=%#x\n",
	    gpu_rd(smmu, cb0 + SMMU_CB_SCTLR), gpu_rd(smmu, cb0 + SMMU_CB_TTBR0 + 4u),
	    gpu_rd(smmu, cb0 + SMMU_CB_TTBR0), gpu_rd(smmu, cb0 + SMMU_CB_TCR), gpu_rd(smmu, cb0 + SMMU_CB_FSR));
	if (steps < 10)
		goto collapse;

	/* ---- step 10: the GMU's data memory ---- */
	last_step = 10;
	if (gpu_rd(gmu, GMU_CM3_FW_INIT_RESULT) == 0xbabefaceu) {
		printf("SWEET GPU: step 10: firmware is running in the GMU: memory left alone\n");
		goto collapse;
	}
	printf("SWEET GPU: step 10: GMU in reset; write two DTCM words (%#x), read back, restore\n", GMU_PA + GMU_DTCM);
	gpu_wr(gmu, GMU_CM3_SYSRESET, 1);
	was0 = gpu_rd(gmu, GMU_DTCM);
	was1 = gpu_rd(gmu, GMU_DTCM + 4u);
	gpu_wr(gmu, GMU_DTCM, 0x600d0001u);
	gpu_wr(gmu, GMU_DTCM + 4u, 0x0a110002u);
	back0 = gpu_rd(gmu, GMU_DTCM);
	back1 = gpu_rd(gmu, GMU_DTCM + 4u);
	gpu_wr(gmu, GMU_DTCM, was0);
	gpu_wr(gmu, GMU_DTCM + 4u, was1);
	printf("SWEET GPU: step 10: read back %#x %#x: %s\n", back0, back1,
	    (back0 == 0x600d0001u && back1 == 0x0a110002u) ?
	    "THE GMU'S MEMORY TAKES WHAT IS WRITTEN: its firmware can be loaded" : "memory does not hold what was written");
	if (steps < 11 || back0 != 0x600d0001u || back1 != 0x0a110002u)
		goto collapse;

	/* ---- step 11: memory for the GMU, SMMU context 1 ---- */
	last_step = 11;
	if (!gmu_fw) {
		printf("SWEET GPU: step 11: no a650_gmu.bin in %S\n", FW_DIR);
		goto collapse;
	}
	printf("SWEET GPU: step 11: a650_gmu.bin %u bytes; its blocks:\n", gmu_fw_size);
	gmu_nblocks = 0;
	nc_end = GMU_NONCACHED_KERNEL;
	for (uint32_t fat = 0; fat + 16u <= gmu_fw_size;) {
		uint32_t h[4];
		bool tcm;
		for (unsigned k = 0; k < 4; ++k)
			h[k] = gmu_fw[fat + 4u * k] | ((uint32_t)gmu_fw[fat + 4u * k + 1] << 8) |
			    ((uint32_t)gmu_fw[fat + 4u * k + 2] << 16) | ((uint32_t)gmu_fw[fat + 4u * k + 3] << 24);
		printf("SWEET GPU:   block @%#x addr=%#x size=%#x type=%u value=%#x\n", fat, h[0], h[1], h[2], h[3]);
		fat += 16u + h[1];
		tcm = ((h[0] - GMU_ITCM_ADDRESS < GMU_TCM_SIZE) || (h[0] - GMU_DTCM_ADDRESS < GMU_TCM_SIZE)) ? true : false;
		if (h[1] == 0 && (h[2] == GMU_FW_PREALLOC || h[2] == GMU_FW_PREALLOC_KEEP) && !tcm) {
			if (gmu_nblocks == GMU_MAX_BLOCKS || (h[0] & 4095u) || h[3] > 0x1000000u ||
			    !gpu_block_new(&gmu_space, &gmu_blocks[gmu_nblocks++], h[0], h[3], PTE_GMU)) {
				printf("SWEET GPU: step 11: cannot make %#x bytes at %#x\n", h[3], h[0]);
				goto collapse;
			}
			if (h[0] >= GMU_NONCACHED_KERNEL && h[0] < 0x80000000u && h[0] + ((h[3] + 4095u) & ~4095u) > nc_end)
				nc_end = h[0] + ((h[3] + 4095u) & ~4095u);
		}
	}
	/* Data blocks outside the TCMs with no prealloc: back them too (kgsl's cache regions). */
	for (uint32_t fat = 0; fat + 16u <= gmu_fw_size;) {
		uint32_t addr = gmu_fw[fat] | ((uint32_t)gmu_fw[fat + 1] << 8) | ((uint32_t)gmu_fw[fat + 2] << 16) | ((uint32_t)gmu_fw[fat + 3] << 24);
		uint32_t size = gmu_fw[fat + 4] | ((uint32_t)gmu_fw[fat + 5] << 8) | ((uint32_t)gmu_fw[fat + 6] << 16) | ((uint32_t)gmu_fw[fat + 7] << 24);
		bool tcm, covered = false;
		fat += 16u + size;
		tcm = ((addr - GMU_ITCM_ADDRESS < GMU_TCM_SIZE) || (addr - GMU_DTCM_ADDRESS < GMU_TCM_SIZE)) ? true : false;
		if (size == 0 || tcm)
			continue;
		for (unsigned i = 0; i < gmu_nblocks && !covered; ++i)
			covered = (addr >= gmu_blocks[i].va && addr + size <= gmu_blocks[i].va + gmu_blocks[i].size) ? true : false;
		if (!covered) {
			uint32_t va = addr & ~4095u, end = (addr + size + 4095u) & ~4095u;
			printf("SWEET GPU:   data block %#x+%#x has no prealloc: backing %#x..%#x\n", addr, size, va, end - 1);
			if (gmu_nblocks == GMU_MAX_BLOCKS || !gpu_block_new(&gmu_space, &gmu_blocks[gmu_nblocks++], va, end - va, PTE_GMU))
				goto collapse;
			if (va >= GMU_NONCACHED_KERNEL && va < 0x80000000u && end > nc_end)
				nc_end = end;
		}
	}
	gmu_log_va = nc_end;
	gmu_hfi_va = nc_end + GMU_LOG_SIZE;
	if (!gpu_block_new(&gmu_space, &gmu_log, gmu_log_va, GMU_LOG_SIZE, PTE_GMU) ||
	    !gpu_block_new(&gmu_space, &gmu_hfi, gmu_hfi_va, (uint32_t)PAGE_SIZE, PTE_GMU))
		goto collapse;
	ctx = (npg + GMU_CONTEXT) * pg;
	printf("SWEET GPU: step 11: SMMU context %u (%#x) <- tables %#llx; stream %u via match %u\n",
	    GMU_CONTEXT, SMMU_PA + ctx, (unsigned long long)gmu_space.phys[0], GMU_STREAM, GMU_MATCH);
	gpu_smmu_context(smmu, pg, npg, GMU_CONTEXT, &gmu_space, 0);
	gmu_smmu_on = true;
	if (!gpu_smmu_stream(smmu, pg, npg, GMU_MATCH, GMU_STREAM, SMMU_STREAM_MASK, GMU_CONTEXT, &gmu_space)) {
		printf("SWEET GPU: step 11: the SMMU did not take what was written\n");
		goto collapse;
	}
	printf("SWEET GPU: step 11: GMU memory mapped through SMMU context %u\n", GMU_CONTEXT);
	if (steps < 12)
		goto collapse;

	/* ---- step 12: the firmware ---- */
	last_step = 12;
	printf("SWEET GPU: step 12: load a650_gmu.bin (TCM through registers, the rest into memory)\n");
	gpu_wr(gmu, GMU_CM3_SYSRESET, 1);
	wrong = 0;
	for (unsigned pass = 0; pass < 2; ++pass) {
		for (uint32_t fat = 0; fat + 16u <= gmu_fw_size;) {
			uint32_t addr = gmu_fw[fat] | ((uint32_t)gmu_fw[fat + 1] << 8) | ((uint32_t)gmu_fw[fat + 2] << 16) | ((uint32_t)gmu_fw[fat + 3] << 24);
			uint32_t size = gmu_fw[fat + 4] | ((uint32_t)gmu_fw[fat + 5] << 8) | ((uint32_t)gmu_fw[fat + 6] << 16) | ((uint32_t)gmu_fw[fat + 7] << 24);
			const uint8_t *data = gmu_fw + fat + 16u;
			bool itcm, dtcm, placed = false;
			fat += 16u + size;
			if (size == 0 || fat > gmu_fw_size)
				continue;
			itcm = (addr - GMU_ITCM_ADDRESS < GMU_TCM_SIZE) ? true : false;
			dtcm = (addr - GMU_DTCM_ADDRESS < GMU_TCM_SIZE) ? true : false;
			if (itcm || dtcm) {
				uint32_t reg = itcm ? GMU_ITCM + (addr - GMU_ITCM_ADDRESS) : GMU_DTCM + (addr - GMU_DTCM_ADDRESS);
				for (uint32_t o = 0; o + 4u <= size; o += 4u) {
					uint32_t w = data[o] | ((uint32_t)data[o + 1] << 8) | ((uint32_t)data[o + 2] << 16) | ((uint32_t)data[o + 3] << 24);
					if (pass == 0)
						gpu_wr(gmu, reg + o, w);
					else if (gpu_rd(gmu, reg + o) != w)
						wrong++;
				}
				if (pass == 1)
					printf("SWEET GPU:   %#x bytes at GMU %#x (%s, registers)\n", size, addr, itcm ? "ITCM" : "DTCM");
				continue;
			}
			if (pass == 1)
				continue;
			for (unsigned i = 0; i < gmu_nblocks && !placed; ++i) {
				if (addr >= gmu_blocks[i].va && addr + size <= gmu_blocks[i].va + gmu_blocks[i].size) {
					for (uint32_t o = 0; o < size; ++o)
						((volatile uint8_t *)gmu_blocks[i].mem)[addr - gmu_blocks[i].va + o] = data[o];
					gpu_to_memory(gmu_blocks[i].mem, gmu_blocks[i].size);
					printf("SWEET GPU:   %#x bytes at GMU %#x (memory)\n", size, addr);
					placed = true;
				}
			}
			if (!placed) {
				printf("SWEET GPU: step 12: %#x bytes for %#x, where no memory was made\n", size, addr);
				goto collapse;
			}
		}
	}
	printf("SWEET GPU: step 12: %s; versions core %#x power %#x hfi %#x\n",
	    wrong ? "TCM READBACK MISMATCH" : "THE GMU'S FIRMWARE IS LOADED",
	    gpu_rd(gmu, GMU_DTCM_VERSIONS), gpu_rd(gmu, GMU_DTCM_VERSIONS + 8u), gpu_rd(gmu, GMU_DTCM_VERSIONS + 16u));
	if (wrong || steps < 13)
		goto collapse;

	/* ---- step 13: RSCC/PDC, configuration, start ---- */
	last_step = 13;
	pdc = ml_io_map(PDC_CFG_PA, 0x10000);
	gbif = ml_io_map(GBIF_HALT_PA & ~0xfffu, 0x1000);
	if (!pdc || !gbif) {
		printf("SWEET GPU: step 13: PDC/GBIF map failed\n");
		goto collapse;
	}
	printf("SWEET GPU: step 13: RSCC program (a650 family), PDC enable (%#x)\n", PDC_CFG_PA);
	if (gpu_rd(rscc, RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP) {
		gpu_wr(gmu, GMU_RSCC_CONTROL_REQ, 2);
		for (unsigned i = 0; i < 100 && (gpu_rd(gmu, GMU_RSCC_CONTROL_ACK) & 2u) != 2u; ++i)
			delay(10);
		gpu_wr(gmu, GMU_RSCC_CONTROL_REQ, 0);
		printf("SWEET GPU:   rscc woken: ack %#x status0 %#x\n", gpu_rd(gmu, GMU_RSCC_CONTROL_ACK),
		    gpu_rd(rscc, RSCC_RSC_STATUS0_DRV0));
	}
	{
		static const uint32_t rsc_seq[5] = { 0xeaaae5a0u, 0xe1a1ebabu, 0xa2e0a581u, 0xecac82e2u, 0x0020edadu };
		gpu_wr(rscc, RSCC_RSC_STATUS0_DRV0, 1u << 24);
		gpu_wr(rscc, RSCC_PDC_SLAVE_ID_DRV0, 1);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_DATA, 0);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_ADDR, 0);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_DATA + 8u, 0);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_ADDR + 8u, 0);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_DATA + 16u, 0x80000000u);
		gpu_wr(rscc, RSCC_HIDDEN_CMD0_ADDR + 16u, 0);
		gpu_wr(rscc, RSCC_OVERRIDE_START, 0);
		gpu_wr(rscc, RSCC_PDC_SEQ_START, 0x4520);
		gpu_wr(rscc, RSCC_PDC_MATCH_LO, 0x4510);
		gpu_wr(rscc, RSCC_PDC_MATCH_HI, 0x4514);
		for (unsigned i = 0; i < 5; ++i)
			gpu_wr(rscc, RSCC_SEQ_MEM_0_DRV0 + 4u * i, rsc_seq[i]);
		gpu_wr(pdc, PDC_GPU_SEQ_START, 0);
		gpu_wr(pdc, PDC_GPU_ENABLE, 0x80000001u);
	}
	rsc_programmed = true;
	printf("SWEET GPU:   rscc seq_mem0 %#x, pdc enable %#x\n", gpu_rd(rscc, RSCC_SEQ_MEM_0_DRV0), gpu_rd(pdc, PDC_GPU_ENABLE));

	/* sweet_gpu.c maps a write-combined view of the HFI page here; on Windows the page itself is write-combined
	 * (gpu_mem_alloc), so it is used directly: no second mapping of the same RAM with another cache attribute. */
	hfi_va = gmu_hfi.mem;
	hfi = (volatile uint32_t *)hfi_va;
	if (!hfi) {
		printf("SWEET GPU: step 13: HFI page map failed\n");
		goto collapse;
	}
	{
		/* table header (6 words) + 3 queue headers of 12 words; queues 4K each after a 16-byte-aligned table */
		static const uint32_t prio[3] = { 10, 10, 40 };
		uint32_t tbl_words = 6 + 3 * 12, qoff = ((tbl_words * 4u) + 15u) & ~15u;
		gmu_hfi_q = hfi;
		hfi_qoff = qoff;
		for (uint32_t i = 0; i < tbl_words; ++i)
			hfi[i] = 0;
		hfi[0] = 0;
		hfi[1] = tbl_words;
		hfi[2] = 6;
		hfi[3] = 12;
		hfi[4] = 3;
		hfi[5] = 3;
		for (uint32_t q = 0; q < 3; ++q) {
			volatile uint32_t *h = hfi + 6 + 12 * q;
			h[0] = 1;                                   /* status enabled */
			h[1] = gmu_hfi_va + qoff + 4096u * q;       /* start */
			h[2] = q | (prio[q] << 8);                  /* type */
			h[3] = 1024;                                /* queue size, words */
		}
		apollo_dsb_sy();
	}

	printf("SWEET GPU: step 13: GMU configuration, chip %#x, then out of reset\n", GMU_CHIPID);
	gpu_wr(gmu, GMU_CX_FALNEXT_INTF, 1);                /* Linux a6xx_gmu_fw_start, a650 family */
	gpu_wr(gmu, GMU_CX_FAL_INTF, 1);                    /* veto_fal10 */
	gpu_wr(gmu, GMU_GENERAL_7, 1);                      /* TCM retention */
	gpu_wr(gmu, GMU_CM3_FW_INIT_RESULT, 0);
	gpu_wr(gmu, GMU_CM3_BOOT_CONFIG, GMU_TCM_BOOT);
	gpu_wr(gmu, GMU_HFI_QTBL_ADDR, gmu_hfi_va);
	gpu_wr(gmu, GMU_HFI_QTBL_INFO, 1);
	gpu_wr(gmu, GMU_AHB_FENCE_RANGE_0, GMU_FENCE_RANGE_MASK);
	gpu_wr(gmu, GMU_CM3_CFG, 0x4052);
	gpu_wr(gmu, GMU_HFI_SFR_ADDR, GMU_CHIPID);
	gpu_wr(gmu, GMU_PWR_COL_CP_MSG, (gmu_log_va & 0xfffff000u) | ((GMU_LOG_SIZE / 4096u - 1u) & 0xffu));
	gpu_wr(gmu, GMU_SYS_BUS_CONFIG, 1);
	gpu_wr(gmu, GMU_ICACHE_CONFIG, 1);
	gpu_wr(gmu, GMU_DCACHE_CONFIG, 1);
	gpu_wr(gmu, GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400u);
	gpu_wr(gmu, GMU_RPMH_CTRL, gpu_rd(gmu, GMU_RPMH_CTRL) | GMU_RPMH_ENABLE_MASK);
	gpu_wr(gbif, GBIF_HALT_PA & 0xfffu, 0);
	gpu_wr(gmu, GMU_PWR_COL_CP_RESP, 0);
	gpu_wr(gmu, GMU_GMU2HOST_INTR_CLR, 0xffffffffu);
	gmu_started = true;
	gpu_wr(gmu, GMU_CM3_SYSRESET, 0);
	result = 0;
	for (unsigned i = 0; i < 1000 && (result & GMU_STARTED_MASK) != GMU_STARTED && result != 0xbabefaceu; ++i) {
		delay(100);
		result = gpu_rd(gmu, GMU_CM3_FW_INIT_RESULT);
	}
	fsr = gpu_rd(smmu, ctx + SMMU_CB_FSR);
	printf("SWEET GPU: step 13: fw_init_result %#x, gmu2host %#x, hfi_ctrl_status %#x, breadcrumb %#x, smmu fsr %#x far %#x:%#x fsynr0 %#x\n",
	    result, gpu_rd(gmu, GMU_GMU2HOST_INTR_INFO), gpu_rd(gmu, GMU_HFI_CTRL_STATUS),
	    gpu_rd(gmu, GMU_DTCM_BREADCRUMB), fsr, gpu_rd(smmu, ctx + SMMU_CB_FAR + 4u),
	    gpu_rd(smmu, ctx + SMMU_CB_FAR), gpu_rd(smmu, ctx + SMMU_CB_FSYNR0));
	if ((result & GMU_STARTED_MASK) != GMU_STARTED && result != 0xbabefaceu) {
		printf("SWEET GPU: step 13: the GMU's firmware does not come up\n");
		goto collapse;
	}
	printf("SWEET GPU: step 13: THE GMU'S FIRMWARE RUNS\n");
	if (steps < 14)
		goto collapse;

	/* ---- step 14: HFI queues ---- */
	last_step = 14;
	printf("SWEET GPU: step 14: start the HFI message queues\n");
	gpu_wr(gmu, GMU_HFI_CTRL_INIT, 1);
	for (unsigned i = 0; i < 500 && !(gpu_rd(gmu, GMU_HFI_CTRL_STATUS) & 1u); ++i)
		delay(100);
	gmu_talks = (gpu_rd(gmu, GMU_HFI_CTRL_STATUS) & 1u) != 0;
	printf("SWEET GPU: step 14: hfi_ctrl_status %#x hfi_version %#x fsr %#x: %s\n",
	    gpu_rd(gmu, GMU_HFI_CTRL_STATUS), gpu_rd(gmu, GMU_HFI_VERSION_INFO), gpu_rd(smmu, ctx + SMMU_CB_FSR),
	    gmu_talks ? "THE GMU'S MESSAGE QUEUES ARE UP" : "the GMU does not start its queues");
	if (!gmu_talks || steps < 15)
		goto collapse;

	/* ---- step 15: RPMh's levels and bus resources, the GMU's tables ---- */
	last_step = 15;
	printf("SWEET GPU: step 15: read the RPMh command database at %#x\n", CMD_DB_PA);
	cmd_db_va = ml_io_map_wcomb(CMD_DB_PA, CMD_DB_SIZE);
	cmd_db = (const volatile uint8_t *)cmd_db_va;
	if (!cmd_db) {
		printf("SWEET GPU: step 15: cmd-db map failed\n");
		goto collapse;
	}
	printf("SWEET GPU:   cmd-db version %#x magic %02x %02x %02x %02x\n", cmd_db_le(0, 4), cmd_db[4], cmd_db[5],
	    cmd_db[6], cmd_db[7]);
	gfx_addr = gpu_cmd_db("gfx.lvl", gfx_lvl, &ngfx);
	cx_addr = gpu_cmd_db("cx.lvl", cx_lvl, &ncx);
	mx_addr = gpu_cmd_db("mx.lvl", mx_lvl, &nmx);
	{
		static const char *const rails[3] = { "gfx.lvl", "cx.lvl", "mx.lvl" };
		const uint16_t *rail_lvl[3];
		unsigned rail_n[3];
		uint32_t rail_addr[3];
		rail_lvl[0] = gfx_lvl; rail_lvl[1] = cx_lvl; rail_lvl[2] = mx_lvl;
		rail_n[0] = ngfx; rail_n[1] = ncx; rail_n[2] = nmx;
		rail_addr[0] = gfx_addr; rail_addr[1] = cx_addr; rail_addr[2] = mx_addr;
		for (unsigned r = 0; r < 3; ++r) {
			char line[128];
			size_t len = 0;
			line[0] = 0;
			for (unsigned k = 0; k < rail_n[r]; ++k) {
				(void)RtlStringCbLengthA(line, sizeof(line), &len);
				if (len >= sizeof(line) - 8)
					break;
				(void)RtlStringCbPrintfA(line + len, sizeof(line) - len, " %u", rail_lvl[r][k]);
			}
			printf("SWEET GPU:   %s at %#x, %u levels:%s\n", rails[r], rail_addr[r], rail_n[r], line);
		}
	}
	{
		/* boot 17: MC0/SH0/ACV as Linux's a650 table, but CN0 at 0x50074, not Linux's 0x500a4 */
		static const char *const buses[4] = { "MC0", "SH0", "ACV", "CN0" };
		static const uint32_t linux_bus[4] = { 0x50000, 0x50004, 0x5007c, 0x500a4 };
		for (unsigned b = 0; b < 4; ++b) {
			bus[b] = gpu_cmd_db(buses[b], NULL, NULL);
			printf("SWEET GPU:   bus %s at %#x (Linux a650: %#x)%s\n", buses[b], bus[b], linux_bus[b],
			    bus[b] ? "" : ", not in the database: Linux's used");
			if (!bus[b])
				bus[b] = linux_bus[b];
		}
	}
	if (!gfx_addr || !cx_addr || !mx_addr || !ngfx || !ncx || !nmx) {
		printf("SWEET GPU: step 15: the database lacks a rail\n");
		goto collapse;
	}
	perf[1] = (uint32_t)NGPU;
	perf[2] = 2;
	for (unsigned i = 0; i < NGPU; ++i) {
		perf[3 + 3 * i + 1] = 0xffffffffu;          /* no ACD */
		perf[3 + 3 * i + 2] = gpu_levels[i].khz;
		if (!gpu_rail_vote(gpu_levels[i].vlvl, gfx_lvl, ngfx, mx_lvl, nmx, &perf[3 + 3 * i])) {
			printf("SWEET GPU: step 15: gfx.lvl has no level %u\n", gpu_levels[i].vlvl);
			goto collapse;
		}
		printf("SWEET GPU:   gpu level %u: %u kHz, rail %u -> vote %#x\n", i, gpu_levels[i].khz,
		    gpu_levels[i].vlvl, perf[3 + 3 * i]);
	}
	if (!gpu_rail_vote(0, cx_lvl, ncx, mx_lvl, nmx, &perf[51]) ||
	    !gpu_rail_vote(RPMH_MIN_SVS, cx_lvl, ncx, mx_lvl, nmx, &perf[53])) {
		printf("SWEET GPU: step 15: cx.lvl has no level for the GMU\n");
		goto collapse;
	}
	perf[52] = 0;
	perf[54] = 200000;
	printf("SWEET GPU:   gmu levels: 0 kHz vote %#x, 200000 kHz vote %#x\n", perf[51], perf[53]);
	/* Linux a650_build_bw_table: one "off" level, DDR (3 commands) and CNOC (1); addresses from cmd-db */
	bw[1] = 1;
	bw[2] = 1;
	bw[3] = 3;
	bw[4] = 1;
	bw[5] = 1;
	bw[6] = bus[3];
	bw[12] = 0x40000000u;
	bw[18] = 0x60000001u;
	bw[24] = bus[0];
	bw[25] = bus[1];
	bw[26] = bus[2];
	bw[32] = bw[33] = bw[34] = 0x40000000u;
	printf("SWEET GPU: step 15: tables ready (ddr %#x %#x %#x, cnoc %#x)\n", bw[24], bw[25], bw[26], bw[6]);
	if (steps < 16)
		goto collapse;

	/* ---- step 16: HFI messages ---- */
	last_step = 16;
	printf("SWEET GPU: step 16: talk to the GMU\n");
	for (unsigned q = 0; q < 3; ++q)
		if (hfi_header(q)[10] != hfi_header(q)[11]) {
			printf("SWEET GPU:   queue %u read %u write %u before the first message:\n", q, hfi_header(q)[10],
			    hfi_header(q)[11]);
			if (q)
				(void)hfi_drain(q, ~0u, NULL, 0);
		}
	if (!hfi_send("the table of GPU speeds", HFI_PERF_TABLE, perf, HFI_PERF_WORDS))
		goto collapse;
	gmu_tables = true;
	if (!hfi_send("the table of bus votes", HFI_BW_TABLE, bw, HFI_BW_WORDS) ||
	    !hfi_send("core firmware start", HFI_CORE_FW_START, core_start, 2) ||
	    !hfi_send("start", HFI_START, start, 1))
		goto collapse;
	printf("SWEET GPU: step 16: THE GMU HAS ITS TABLES AND IS STARTED; rpmh_power_state %#x\n",
	    gpu_rd(gmu, GMU_RPMH_POWER_STATE));
	if (steps < 17)
		goto collapse;

	/* ---- step 17: a GPU speed, the 3D core powered ---- */
	last_step = 17;
	level = gpu_level;
	if (level < 1 || level >= NGPU)
		level = 1;
	perf_vote[0] = 0;                               /* blocking, speed index, bus index 0 */
	perf_vote[1] = 1;
	perf_vote[2] = level;
	perf_vote[3] = 0;
	printf("SWEET GPU: step 17: ask for %u MHz (level %u), then for the 3D core's power\n",
	    gpu_levels[level].khz / 1000, level);
	if (!hfi_send("the GPU speed", HFI_GX_BW_PERF_VOTE, perf_vote, 4))
		goto collapse;
	printf("SWEET GPU:   before: sptprac %#x gx gdsc %#010x gx domain %#x pll0 mode %#x L %#x\n",
	    gpu_rd(gmu, GMU_SPTPRAC_PWR_CLK), gpu_rd(cc, GPUCC_GX_GDSC), gpu_rd(cc, GPUCC_GX_DOMAIN),
	    gpu_rd(cc, GPUCC_PLL0), gpu_rd(cc, GPUCC_PLL0 + 4u));
	gx_asked = true;
	gpu_wr(gmu, GMU_HOST2GMU_INTR_SET, GMU_GPU_SET);
	granted = 0;
	for (unsigned i = 0; i < 5000 && !(granted & GMU_GPU_GRANTED); ++i) {
		delay(100);
		granted = gpu_rd(gmu, GMU_GMU2HOST_INTR_INFO);
	}
	gpu_wr(gmu, GMU_GMU2HOST_INTR_CLR, GMU_GPU_GRANTED);
	(void)hfi_drain(2, ~0u, NULL, 0);
	sptprac = gpu_rd(gmu, GMU_SPTPRAC_PWR_CLK);
	pll0 = gpu_rd(cc, GPUCC_PLL0);
	printf("SWEET GPU:   after: gmu2host %#x sptprac %#x gx gdsc %#010x gx domain %#x pll0 mode %#x L %#x (%u MHz if locked) rpmh_power_state %#x\n",
	    granted, sptprac, gpu_rd(cc, GPUCC_GX_GDSC), gpu_rd(cc, GPUCC_GX_DOMAIN), pll0,
	    gpu_rd(cc, GPUCC_PLL0 + 4u), (gpu_rd(cc, GPUCC_PLL0 + 4u) & 0xffffu) * 192u / 10u,
	    gpu_rd(gmu, GMU_RPMH_POWER_STATE));
	if (!(granted & GMU_GPU_GRANTED) || (sptprac & GMU_GX_OFF_BITS)) {
		printf("SWEET GPU: step 17: the GMU does not power the 3D core\n");
		gmu_log_tail();
		goto collapse;
	}
	gpu = ml_io_map(GPU_PA, GPU_SIZE);
	if (!gpu) {
		printf("SWEET GPU: step 17: 3D core map failed\n");
		goto collapse;
	}
	printf("SWEET GPU: step 17: read the 3D core (%#x)\n", GPU_PA);
	printf("SWEET GPU:   rbbm_status %#x rbbm_int_0_status %#x rbbm_clock_cntl %#x cp_rb_base %#x\n",
	    gpu_rd(gpu, GPU_RBBM_STATUS), gpu_rd(gpu, GPU_RBBM_INT_0_STATUS), gpu_rd(gpu, GPU_RBBM_CLOCK_CNTL),
	    gpu_rd(gpu, GPU_CP_RB_BASE));
	printf("SWEET GPU: step 17: THE 3D CORE HAS POWER AND ANSWERS\n");
	gpu_core = gpu;
	if (steps < 18)
		goto collapse;

	/* ---- step 18: the 3D core's memory: ring and microcode ---- */
	last_step = 18;
	if (!sqe_file || sqe_file_size < 8 || sqe_file_size > 0x40000) {
		printf("SWEET GPU: step 18: no a650_sqe.fw in %S\n", FW_DIR);
		goto collapse;
	}
	/* the file's first word is not microcode (Linux adreno_fw_create_bo); the next holds the version */
	sqe_ver = sqe_file[4] | ((uint32_t)sqe_file[5] << 8) | ((uint32_t)sqe_file[6] << 16) | ((uint32_t)sqe_file[7] << 24);
	printf("SWEET GPU: step 18: a650_sqe.fw %u bytes, version word %#x (%#x, Linux wants >= %#x)\n", sqe_file_size, sqe_ver,
	    sqe_ver & 0xfffu, GPU_SQE_MIN_VERSION);
	/* apollo's a650_sqe.fw is 0x093. Linux refuses below 0x095 for security fixes that matter once untrusted
	 * programs submit commands; Xiaomi's kgsl runs this file. Only our own commands run here. */
	if ((sqe_ver & 0xfffu) < GPU_SQE_MIN_VERSION)
		printf("SWEET GPU: step 18: older than Linux's minimum (vendor kgsl runs it): used for our own commands only\n");
	if (!gpu_block_new(&core_space, &core_ring, CORE_RING_VA, CORE_RING_SIZE, PTE_CORE) ||
	    !gpu_block_new(&core_space, &core_sqe, CORE_SQE_VA, sqe_file_size - 4u, PTE_CORE | PTE_READ_ONLY)) {
		printf("SWEET GPU: step 18: no memory for the core\n");
		goto collapse;
	}
	for (unsigned int i = 4; i < sqe_file_size; ++i)
		((volatile uint8_t *)core_sqe.mem)[i - 4] = sqe_file[i];
	gpu_to_memory(core_sqe.mem, core_sqe.size);
	printf("SWEET GPU: step 18: SMMU context %u <- tables %#llx; stream %u mask %#x via match %u\n", CORE_CONTEXT,
	    (unsigned long long)core_space.phys[0], CORE_STREAM, CORE_STREAM_MASK, CORE_MATCH);
	gpu_smmu_context(smmu, pg, npg, CORE_CONTEXT, &core_space, SMMU_SCTLR_HUPCF);
	core_smmu_on = true;
	if (!gpu_smmu_stream(smmu, pg, npg, CORE_MATCH, CORE_STREAM, CORE_STREAM_MASK, CORE_CONTEXT, &core_space)) {
		printf("SWEET GPU: step 18: the SMMU did not take what was written\n");
		goto collapse;
	}
	printf("SWEET GPU: step 18: the core's ring and microcode mapped; rbbm_status %#x\n", gpu_rd(gpu, GPU_RBBM_STATUS));
	if (steps < 19)
		goto collapse;

	/* ---- step 19: the core's registers, its command processor, CP_ME_INIT ---- */
	last_step = 19;
	printf("SWEET GPU: step 19: the core's registers (Linux a6xx hw_init, a650)\n");
	core_hw_regs(gpu);
	printf("SWEET GPU:   sqe_instr_base %#x rb_base %#x rb_cntl %#x apriv %#x rbbm_status %#x\n",
	    gpu_rd(gpu, GPU_CP_SQE_INSTR_BASE), gpu_rd(gpu, GPU_CP_RB_BASE), gpu_rd(gpu, GPU_CP_RB_CNTL),
	    gpu_rd(gpu, GPU_CP_APRIV_CNTL), gpu_rd(gpu, GPU_RBBM_STATUS));

	printf("SWEET GPU: step 19: start the command processor\n");
	core_started = true;
	gpu_wr(gpu, GPU_CP_SQE_CNTL, 1);
	delay(20000);
	printf("SWEET GPU:   after start: status %#x int %#x cp fault %#x cp int %#x protect %#x rptr %u\n",
	    gpu_rd(gpu, GPU_RBBM_STATUS), gpu_rd(gpu, GPU_RBBM_INT_0_STATUS), gpu_rd(gpu, GPU_CP_HW_FAULT),
	    gpu_rd(gpu, GPU_CP_INTERRUPT_STATUS), gpu_rd(gpu, GPU_CP_PROTECT_STATUS), gpu_rd(gpu, GPU_CP_RB_RPTR));
	if (gpu_smmu_faulted(smmu, pg, npg, CORE_CONTEXT, "CORE"))
		goto collapse;

	{
		/* CP_ME_INIT, 8 words (a6xx_cp_init): multiple contexts, error detection */
		static const uint32_t me_init[8] = { 0x2f, 0x3, 0x20000000u, 0, 0, 0, 0, 0 };
		ring = (volatile uint32_t *)core_ring.mem;
		at = 0;
		ring[at++] = (7u << 28) | 8u | (gpu_parity(8) << 15) | (CP_ME_INIT << 16) | (gpu_parity(CP_ME_INIT) << 23);
		for (unsigned i = 0; i < 8; ++i)
			ring[at++] = me_init[i];
	}
	gpu_to_memory(core_ring.mem, 4u * at);
	printf("SWEET GPU: step 19: give it CP_ME_INIT (ring words 0..%u)\n", at - 1);
	gpu_wr(gpu, GPU_CP_RB_WPTR, at);
	rptr = ~0u;
	status = ~0u;
	for (unsigned i = 0; i < 20000 && (rptr != at || (status & GPU_BUSY)); ++i) {
		delay(100);
		rptr = gpu_rd(gpu, GPU_CP_RB_RPTR);
		status = gpu_rd(gpu, GPU_RBBM_STATUS);
	}
	fault = gpu_rd(gpu, GPU_CP_HW_FAULT);
	refused = gpu_smmu_faulted(smmu, pg, npg, CORE_CONTEXT, "CORE");
	printf("SWEET GPU:   rptr %u of %u, status %#x int %#x cp fault %#x cp int %#x protect %#x\n", rptr, at, status,
	    gpu_rd(gpu, GPU_RBBM_INT_0_STATUS), fault, gpu_rd(gpu, GPU_CP_INTERRUPT_STATUS), gpu_rd(gpu, GPU_CP_PROTECT_STATUS));
	if (rptr != at || (status & GPU_BUSY) || fault || refused) {
		printf("SWEET GPU: step 19: the command processor does not get through CP_ME_INIT\n");
		goto collapse;
	}
	printf("SWEET GPU: step 19: THE 3D CORE TAKES COMMANDS\n");
	if (steps < 20)
		goto collapse;

	/* ---- step 20: the zap shader, then the 3D core out of secure mode ---- */
	/*
	 * The 3D core came out of reset locked in secure mode; on a phone only
	 * TrustZone lets it out, by authenticating the vendor's signed zap shader
	 * (Linux a6xx_zap_shader_init -> qcom_mdt_load -> qcom_scm_pas_*; the GPU
	 * is PAS image 13), after which the command processor is told to leave
	 * secure mode. The ladder (Zap):
	 *   0            nothing: no SCM call at all (explicit opt-out)
	 *   1            only the atomic, no-memory queries -- HackiOS step 11
	 *   2            + the signature of the zap .mdt (PAS_INIT_IMAGE) -- step 12
	 *   3 (default)  + where the image goes, the segment, authenticate and run,
	 *                then CP_SET_SECURE_MODE -- HackiOS steps 19 and 21
	 * (ios-apollo boot 20 hung on a blind PAS_SHUTDOWN fired first; it is made only in the teardown, and only once a
	 * shader was really authenticated here.)
	 */
	last_step = 20;
	if (gpu_zap_stage == 0) {
		printf("SWEET GPU: step 20: held back (Zap=0). The ladder is verified on apollo (ios-apollo boots 22-24); "
		    "default is 3 (full). 1 = the safe queries only, 2 += the signature\n");
		goto collapse;
	}

	/* stage 1: the atomic (SCM_FAST), no-memory questions Linux asks first (HackiOS step 11) */
	{
		static const struct { const char *name; uint32_t id; } images[] =
		    { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
		uint64_t how = SCM_FAST | SCM_64 | SCM_SIP, ans = 0;
		printf("SWEET GPU: step 20: ask TrustZone what Linux asks first: is \"is-call-available\" available? (64-bit)\n");
		if (gpu_scm("whether it can be asked (64-bit)", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_IS_CALL_AVAILABLE, 0, 0, &ans) != 0 || ans != 1) {
			how = SCM_FAST | SCM_SIP;        /* Linux's second try: 32-bit arguments */
			ans = 0;
			printf("SWEET GPU: step 20: the same, the 32-bit way\n");
			if (gpu_scm("whether it can be asked (32-bit)", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
			    SCM_SIP | SCM_IS_CALL_AVAILABLE, 0, 0, &ans) != 0 || ans != 1) {
				printf("SWEET GPU: step 20: TrustZone does not answer the way it answers Linux; stopping\n");
				goto collapse;
			}
		}
		ans = 0;
		(void)gpu_scm("is the signature call there", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &ans);
		ans = 0;
		(void)gpu_scm("is authenticate-and-reset there", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &ans);
		for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
			ans = 0;
			(void)gpu_scm(images[i].name, how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &ans);
		}
		printf("SWEET GPU: step 20: TrustZone answered the safe queries (the lines above say how)\n");
	}
	if (gpu_zap_stage < 2) {
		printf("SWEET GPU: step 20: stopping after the safe queries (Zap=1); =2 checks the signature\n");
		goto collapse;
	}

	/* the zap metadata (stages 2 and 3); the segment is needed only at stage 3 */
	if (!zap_mdt_file) {
		printf("SWEET GPU: step 20: no a650_zap.mdt in %S\n", FW_DIR);
		goto collapse;
	}

	/* stage 2: the signature. A yielding call that passes the .mdt, but touches no GPU (HackiOS step 12). */
	if (!gpu_zap_header(zap_mdt_file, zap_mdt_file_size, &zap_header_pa))
		goto collapse;
	printf("SWEET GPU: step 20: hand TrustZone the zap shader's signature to check (image %u)\n", PAS_GPU);
	sig = gpu_scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
	    PAS_GPU, zap_header_pa, 0, &zap_answer);
	if (sig == 0 && zap_answer == 0)
		printf("SWEET GPU: step 20: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n");
	else
		printf("SWEET GPU: step 20: TrustZone does not take the signature (returns %d, answer %#llx): a kernel before "
		    "this one may already hold one\n", sig, (unsigned long long)zap_answer);
	if (gpu_zap_stage < 3) {
		printf("SWEET GPU: step 20: stopping after the signature (Zap=2); =3 loads it and leaves secure mode\n");
		goto collapse;
	}

	/* stage 3: where it goes, the segment, authenticate and run; then leave secure mode (HackiOS steps 19, 21) */
	if (!zap_seg_file || zap_seg_file_size == 0 || zap_seg_file_size > ZAP_MEMORY_SIZE) {
		printf("SWEET GPU: step 20: no a650_zap.b02 (at most %u bytes) in %S\n", ZAP_MEMORY_SIZE, FW_DIR);
		goto collapse;
	}
	if (sig == 0 && zap_answer == 0) {
		vm_offset_t zap_va;
		volatile uint8_t *zap;
		zap_answer = ~0ull;
		printf("SWEET GPU: step 20: tell TrustZone where the image goes: %#x (%#x bytes)\n", ZAP_MEMORY_PA, ZAP_MEMORY_SIZE);
		if (gpu_scm("where the image goes", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY_PA,
		    ZAP_MEMORY_SIZE, &zap_answer) != 0 || zap_answer != 0) {
			printf("SWEET GPU: step 20: TrustZone does not take the place (answer %#llx)\n", (unsigned long long)zap_answer);
			goto collapse;
		}
		/* only now is that memory ours to write: TrustZone has just said it is not yet its own */
		zap_va = ml_io_map_wcomb(ZAP_MEMORY_PA, ZAP_MEMORY_SIZE);
		zap = (volatile uint8_t *)zap_va;
		if (!zap) {
			printf("SWEET GPU: step 20: zap memory map failed\n");
			goto collapse;
		}
		printf("SWEET GPU: step 20: reserved memory begins %02x %02x %02x %02x, the segment %02x %02x %02x %02x; "
		    "writing %u bytes at %#x\n", zap[0], zap[1], zap[2], zap[3], zap_seg_file[0], zap_seg_file[1], zap_seg_file[2],
		    zap_seg_file[3], zap_seg_file_size, ZAP_MEMORY_PA);
		for (unsigned int i = 0; i < ZAP_MEMORY_SIZE; ++i)
			zap[i] = (uint8_t)((i < zap_seg_file_size) ? zap_seg_file[i] : 0u);
		apollo_dsb_sy();  /* the write-combine write lands before TrustZone reads it */
		ml_io_unmap(zap_va, ZAP_MEMORY_SIZE);
		zap_answer = ~0ull;
		printf("SWEET GPU: step 20: ask TrustZone to authenticate the zap shader and run it\n");
		if (gpu_scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE,
		    PAS_GPU, 0, 0, &zap_answer) != 0 || zap_answer != 0) {
			printf("SWEET GPU: step 20: TrustZone does not authenticate the zap shader (answer %#llx)\n",
			    (unsigned long long)zap_answer);
			goto collapse;
		}
		zap_loaded = true;
		printf("SWEET GPU: step 20: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n");
	} else {
		printf("SWEET GPU: step 20: the signature was refused; taking it that TrustZone keeps a shader from before and "
		    "trying to leave secure mode with it\n");
	}

	/* command the 3D core out of secure mode; the ring picks up after CP_ME_INIT (wptr 9 -> 11) */
	printf("SWEET GPU: step 20: command the 3D core out of secure mode (CP_SET_SECURE_MODE 0)\n");
	ring[at++] = (7u << 28) | 1u | (gpu_parity(1) << 15) | (CP_SET_SECURE_MODE << 16) | (gpu_parity(CP_SET_SECURE_MODE) << 23);
	ring[at++] = 0;                              /* 0 = not secure */
	gpu_to_memory(core_ring.mem, 4u * at);
	gpu_wr(gpu, GPU_CP_RB_WPTR, at);
	srptr = ~0u;
	sstatus = ~0u;
	for (unsigned i = 0; i < 20000 && (srptr != at || (sstatus & GPU_BUSY)); ++i) {
		delay(100);
		srptr = gpu_rd(gpu, GPU_CP_RB_RPTR);
		sstatus = gpu_rd(gpu, GPU_RBBM_STATUS);
	}
	sfault = gpu_rd(gpu, GPU_CP_HW_FAULT);
	srefused = gpu_smmu_faulted(smmu, pg, npg, CORE_CONTEXT, "CORE");
	printf("SWEET GPU:   rptr %u of %u, status %#x int %#x cp fault %#x cp int %#x protect %#x\n", srptr, at, sstatus,
	    gpu_rd(gpu, GPU_RBBM_INT_0_STATUS), sfault, gpu_rd(gpu, GPU_CP_INTERRUPT_STATUS), gpu_rd(gpu, GPU_CP_PROTECT_STATUS));
	if (srptr != at || (sstatus & GPU_BUSY) || sfault || srefused) {
		printf("SWEET GPU: step 20: the command processor does not get through CP_SET_SECURE_MODE\n");
		goto collapse;
	}
	printf("SWEET GPU: step 20: THE 3D CORE IS OUT OF SECURE MODE\n");
	if (steps < 21)
		goto collapse;

	/* ---- step 21: a word written to our memory on command ---- */
	last_step = 21;
	printf("SWEET GPU: step 21: map a page for the core; have it write %#x there (CP_MEM_WRITE)\n", TEST_WORD);
	if (!gpu_block_new(&core_space, &core_scratch, CORE_SCRATCH_VA, 4096, PTE_CORE)) {
		printf("SWEET GPU: step 21: no memory\n");
		goto collapse;
	}
	gpu_smmu_refresh(smmu, pg, npg, CORE_CONTEXT, &core_space);
	from = at;
	ring_pkt7(ring, &at, CP_MEM_WRITE, 3);
	ring[at++] = CORE_SCRATCH_VA;
	ring[at++] = 0;
	ring[at++] = TEST_WORD;
	if (!ring_run(gpu, smmu, pg, npg, core_ring.mem, from, at, "CP_MEM_WRITE")) {
		printf("SWEET GPU: step 21: the command processor does not get through CP_MEM_WRITE\n");
		goto collapse;
	}
	gpu_to_memory(core_scratch.mem, 64);
	word = *(volatile uint32_t *)core_scratch.mem;
	printf("SWEET GPU: step 21: the page reads %#x: %s\n", word,
	    word == TEST_WORD ? "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what the core was told to write");
	if (word != TEST_WORD)
		goto collapse;
	if (steps < 22)
		goto collapse;

	/* ---- step 22: a frame filled by the core's 2D engine, on the panel ---- */
	last_step = 22;
	printf("SWEET GPU: step 22: a %ux%u picture (%u bytes) for the core, filled in three bands by its 2D engine\n",
	    TARGET_W, TARGET_H, TARGET_ROW * TARGET_H);
	if (!gpu_block_new(&core_space, &core_target, CORE_TARGET_VA, TARGET_ROW * TARGET_H, PTE_CORE)) {
		printf("SWEET GPU: step 22: no memory\n");
		goto collapse;
	}
	gpu_smmu_refresh(smmu, pg, npg, CORE_CONTEXT, &core_space);
	from = at;
	ring_pkt7(ring, &at, CP_SET_MARKER, 1);
	ring[at++] = RM6_BLIT2DSCALE;
	ring_reg(ring, &at, REG_RB_CCU_CNTL, CCU_CNTL_BYPASS);
	ring_fill(ring, &at, 0, 800, 255, 128, 0);           /* orange */
	ring_fill(ring, &at, 800, 1600, 255, 255, 255);      /* white */
	ring_fill(ring, &at, 1600, TARGET_H, 0, 128, 255);   /* blue */
	/* out of the core's caches into memory; each event writes its word when done (Linux a6xx_submit) */
	ring_pkt7(ring, &at, CP_EVENT_WRITE, 4);
	ring[at++] = EVENT_FLUSH_COLOR_TS;
	ring[at++] = CORE_SCRATCH_VA + 32u;
	ring[at++] = 0;
	ring[at++] = 0x0c0c0c0cu;
	ring_pkt7(ring, &at, CP_EVENT_WRITE, 4);
	ring[at++] = EVENT_CACHE_FLUSH_TS;
	ring[at++] = CORE_SCRATCH_VA + 16u;
	ring[at++] = 0;
	ring[at++] = 0x0d0e0d0eu;
	ring_pkt7(ring, &at, CP_WAIT_FOR_IDLE, 0);
	if (!ring_run(gpu, smmu, pg, npg, core_ring.mem, from, at, "three solid fills, flushes")) {
		printf("SWEET GPU: step 22: the core does not get through the fills\n");
		goto collapse;
	}
	gpu_to_memory(core_scratch.mem, 64);
	printf("SWEET GPU:   flush words %#x %#x (told 0xc0c0c0c, 0xd0e0d0e)\n",
	    ((volatile uint32_t *)core_scratch.mem)[8], ((volatile uint32_t *)core_scratch.mem)[4]);
	gpu_to_memory(core_target.mem, core_target.size);
	{
		const volatile uint8_t *px = (const volatile uint8_t *)core_target.mem;
		const uint32_t mid = 1200u * TARGET_ROW + 540u * 4u, last = 2399u * TARGET_ROW + 1079u * 4u;
		vm_size_t fb_size;
		vm_offset_t fb_va;
		printf("SWEET GPU:   pixels (b g r a): first %02x %02x %02x %02x (told 00 80 ff ff), middle %02x %02x %02x %02x "
		    "(ff ff ff ff), last %02x %02x %02x %02x (ff 80 00 ff)\n", px[0], px[1], px[2], px[3], px[mid], px[mid + 1],
		    px[mid + 2], px[mid + 3], px[last], px[last + 1], px[last + 2], px[last + 3]);
		if (px[2] != 0xff || px[1] != 0x80 || px[mid] != 0xff || px[mid + 2] != 0xff || px[last] != 0xff || px[last + 2] != 0) {
			printf("SWEET GPU: step 22: the picture is not what the core was told to draw\n");
			goto collapse;
		}
		printf("SWEET GPU: step 22: THE GPU HAS DRAWN A PICTURE; copying it to the panel (%#x)\n", FB_PA);
		fb_size = ROUND_TO_PAGES(FB_ROW * TARGET_H);
		fb_va = ml_io_map_wcomb(FB_PA, fb_size);
		if (!fb_va) {
			printf("SWEET GPU: step 22: framebuffer map failed\n");
			goto collapse;
		}
		for (uint32_t y = 0; y < TARGET_H; ++y) {
			const volatile uint32_t *src = (const volatile uint32_t *)(core_target.mem + y * TARGET_ROW);
			volatile uint32_t *dst = (volatile uint32_t *)(fb_va + y * FB_ROW);
			for (uint32_t x = 0; x < TARGET_W; ++x)
				dst[x] = src[x];
		}
		apollo_dsb_sy();
		ml_io_unmap(fb_va, fb_size);
	}
	printf("SWEET GPU: step 22: A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top\n");

collapse:
	/* HackiOS gpu_block_stop / Linux a6xx_gmu_shutdown, then a6xx_rpmh_stop (milestone 1: the GPU is never kept) */
	if (core_started && gpu_core) {
		/* Linux a6xx_bus_clear_pending_transactions: new requests, then all AXI, halted; let go */
		printf("SWEET GPU: core before stop: status %#x rptr %u busy %#x\n", gpu_rd(gpu_core, GPU_RBBM_STATUS),
		    gpu_rd(gpu_core, GPU_CP_RB_RPTR), gpu_rd(gmu_va, GMU_AO_GPU_CX_BUSY));
		gpu_wr(gpu_core, GPU_GBIF_HALT, 1);
		for (unsigned i = 0; i < 1000 && !(gpu_rd(gpu_core, GPU_GBIF_HALT_ACK) & 1u); ++i)
			delay(10);
		gpu_wr(gpu_core, GPU_GBIF_HALT, 2);
		for (unsigned i = 0; i < 1000 && !(gpu_rd(gpu_core, GPU_GBIF_HALT_ACK) & 2u); ++i)
			delay(10);
		printf("SWEET GPU: core bus halted (gbif halt_ack %#x), let go again\n", gpu_rd(gpu_core, GPU_GBIF_HALT_ACK));
		gpu_wr(gpu_core, GPU_GBIF_HALT, 0);
	}
	if (gx_asked) {
		gpu_wr(gmu_va, GMU_HOST2GMU_INTR_SET, GMU_GPU_CLEAR);
		printf("SWEET GPU: 3D core given back (OOB clear)\n");
	}
	if (gmu_talks && gmu_tables) {
		uint32_t slumber[3] = { 0, 0, 0 };
		if (hfi_send("prepare slumber", HFI_PREPARE_SLUMBER, slumber, 3)) {
			for (unsigned i = 0; i < 1000 && (gpu_rd(gmu_va, GMU_AO_GPU_CX_BUSY) & GMU_CX_BUSY_GPU); ++i)
				delay(100);
		}
		printf("SWEET GPU: after slumber: cx busy %#x sptprac %#x gx gdsc %#010x rpmh_power_state %#x\n",
		    gpu_rd(gmu_va, GMU_AO_GPU_CX_BUSY), gpu_rd(gmu_va, GMU_SPTPRAC_PWR_CLK),
		    gpu_rd(cc, GPUCC_GX_GDSC), gpu_rd(gmu_va, GMU_RPMH_POWER_STATE));
	}
	if (gmu_started) {
		uint32_t gx;
		gpu_wr(gmu_va, GMU_CM3_SYSRESET, 1);
		printf("SWEET GPU: GMU held in reset again\n");
		gx = gpu_rd(cc, GPUCC_GX_GDSC);
		if (gx & GDSC_PWR_ON) {
			gpu_wr(cc, GPUCC_GX_GDSC, gx | GDSC_SW_COLLAPSE);
			for (unsigned i = 0; i < 1000 && (gpu_rd(cc, GPUCC_GX_GDSC) & GDSC_PWR_ON); ++i)
				delay(100);
			printf("SWEET GPU: GX was still on: gx gdsc %#010x -> %#010x\n", gx, gpu_rd(cc, GPUCC_GX_GDSC));
		}
	}
	if (zap_loaded) {
		/* give the zap shader back to TrustZone, so that the next run (or a later driver) can load its own */
		printf("SWEET GPU: zap shader %s\n", gpu_zap_unload() ? "given back to TrustZone" :
		    "NOT given back: TrustZone keeps it; the next run will find it there");
		zap_loaded = false;
	}
	if (rsc_programmed && gmu_rscc) {
		/* vendor a6xx_rscc_sleep_sequence: the GMU's RPMh votes dropped */
		uint32_t st;
		gpu_wr(gmu_va, GMU_RSCC_CONTROL_REQ, 1);
		for (unsigned i = 0; i < 1000 && !(gpu_rd(gmu_rscc, RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP); ++i)
			delay(100);
		st = gpu_rd(gmu_rscc, RSCC_RSC_STATUS0_DRV0);
		gpu_wr(gmu_va, GMU_RSCC_CONTROL_REQ, 0);
		printf("SWEET GPU: GMU's RPMh channel %s (rsc_status0 %#x)\n", (st & RSCC_ASLEEP) ? "asleep" :
		    "DOES NOT GO TO SLEEP", st);
	}
	if (core_smmu_on) {
		(void)gpu_smmu_faulted(smmu_va, smmu_pg, smmu_npg, CORE_CONTEXT, "CORE");
		gpu_wr(smmu_va, SMMU_SMR(CORE_MATCH), 0);
		gpu_wr(smmu_va, SMMU_S2CR(CORE_MATCH), SMMU_S2CR_FAULT);
		gpu_wr(smmu_va, (smmu_npg + CORE_CONTEXT) * smmu_pg + SMMU_CB_SCTLR, 0);
		printf("SWEET GPU: core streams and SMMU context off again\n");
	}
	if (gmu_smmu_on) {
		gpu_wr(smmu_va, SMMU_SMR(GMU_MATCH), 0);
		gpu_wr(smmu_va, SMMU_S2CR(GMU_MATCH), SMMU_S2CR_FAULT);
		gpu_wr(smmu_va, (smmu_npg + GMU_CONTEXT) * smmu_pg + SMMU_CB_SCTLR, 0);
		printf("SWEET GPU: GMU stream and SMMU context off again\n");
	}
	if (cc) {
		gpu_wr(cc, GPUCC_CX_GMU, gpu_rd(cc, GPUCC_CX_GMU) & ~1u);
		gpu_wr(cc, GPUCC_CX_GDSC, gpu_rd(cc, GPUCC_CX_GDSC) | GDSC_SW_COLLAPSE);
		for (unsigned i = 0; i < 500 && (gpu_rd(cc, GPUCC_CX_GDSC) & GDSC_PWR_ON); ++i)
			delay(100);
		printf("SWEET GPU: CX collapsed again (cx gdsc %#010x)\n", gpu_rd(cc, GPUCC_CX_GDSC));
	}
done:
	(void)hfi_va;
	(void)cmd_db_va;
	printf("SWEET GPU: done after step %u (asked for 1..%u)\n", last_step, steps);
}

/* ======================================================================================================================
 * The driver
 * ====================================================================================================================== */
static VOID ProbeThread(PVOID Context)
{
    unsigned steps = (unsigned)(ULONG_PTR)Context;
    unsigned int n = 0;

    gmu_fw = fw_load(L"a650_gmu.bin", 64, 0x200000, &n);
    gmu_fw_size = n;
    sqe_file = fw_load(L"a650_sqe.fw", 8, 0x40000, &sqe_file_size);
    zap_mdt_file = fw_load(L"a650_zap.mdt", 52 + 64, 0x10000, &zap_mdt_file_size);
    zap_seg_file = fw_load(L"a650_zap.b02", 1, ZAP_MEMORY_SIZE, &zap_seg_file_size);
    printf("SWEET GPU: apollogpu: firmware a650_gmu.bin %u, a650_sqe.fw %u, a650_zap.mdt %u, a650_zap.b02 %u bytes; "
           "Level %u, Zap %u\n", gmu_fw_size, sqe_file_size, zap_mdt_file_size, zap_seg_file_size, gpu_level, gpu_zap_stage);

    sweet_gpu_probe(steps);

    /* the teardown has run: no stream reaches our memory any more */
    gpu_mem_free_all();
    while (NMaps)
        ml_io_unmap((vm_offset_t)Maps[NMaps - 1].va, Maps[NMaps - 1].size);
    if (gmu_fw)
        ExFreePoolWithTag((PVOID)gmu_fw, POOL_TAG);
    if (sqe_file)
        ExFreePoolWithTag(sqe_file, POOL_TAG);
    if (zap_mdt_file)
        ExFreePoolWithTag(zap_mdt_file, POOL_TAG);
    if (zap_seg_file)
        ExFreePoolWithTag(zap_seg_file, POOL_TAG);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID Unload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (Thread) {
        (void)KeWaitForSingleObject(Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Thread);
        Thread = NULL;
    }
    if (LogBuf) {
        ExFreePoolWithTag(LogBuf, POOL_TAG);
        LogBuf = NULL;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    HANDLE h;
    NTSTATUS st;
    ULONG steps;
    OBJECT_ATTRIBUTES oa;

    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = Unload;
    LogBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, LOG_SIZE, POOL_TAG);
    LogLen = 0;
    steps = ReadParam(L"Steps", 22);
    gpu_level = ReadParam(L"Level", 1);
    gpu_zap_stage = ReadParam(L"Zap", 3);
    if (steps > 22)
        steps = 22;
    if (steps == 0) {
        printf("SWEET GPU: apollogpu: Steps = 0: nothing to do\n");
        return STATUS_SUCCESS;
    }
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, ProbeThread, (PVOID)(ULONG_PTR)steps);
    if (!NT_SUCCESS(st)) {
        printf("SWEET GPU: apollogpu: cannot start the thread (%#x)\n", st);
        return STATUS_SUCCESS;
    }
    (void)ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&Thread, NULL);
    ZwClose(h);
    return STATUS_SUCCESS;
}
