/*
 * apollowifi - PCIe0 and the QCA6390 Wi-Fi radio on Windows for the Xiaomi Mi 10T / 10T Pro (apollo, SM8250).
 *
 * Stage 1 of native Wi-Fi: prove from Windows, step by step, everything Windows' own pci.sys and Qualcomm's
 * ARM64 NDIS driver for this chip (qcwlan6800sc8180, PCI\VEN_17CB&DEV_1101) need to find in place:
 *
 *   1  read   GCC PCIe0 clocks/resets and the radio's pins (TLMM north)
 *   2  write  rails (RPMh), wlan-en, PCIe0 clocks, PHY and link (Gen2 x1); the QCA6390's IDs (17cb:1101, its
 *             subsystem ID, which the Windows driver's INF must match), BAR, capabilities (MSI, power), MHI
 *             registers; the apps SMMU's match registers. Bus mastering stays off.
 *   3  write  the config space laid out as ECAM at 0x60000000, as Qualcomm's Kona MCFG table describes it to
 *             pci.sys: bus 0 device 0 = the root port (DBI), bus 1 through an iATU region in CFG-shift mode,
 *             the rest of bus 0 answered with all-ones by the controller's outbound blocker; the memory window
 *             0x60200000..0x61feffff passed through. Then read back the way pci.sys would.
 *   4  write  a free apps SMMU context for the radio's stream 0x1c01 with identity tables (all RAM 1:1, the GIC's
 *             MSI frame as device memory), the BDF-to-SID route; read back (the hypervisor decides).
 * Afterwards PERST and wlan-en go low and the SMMU is put back as found (unless Keep = 1). The rails stay voted
 * (shared with Bluetooth and the PCIe/UFS PHYs).
 *
 * Steps 1, 2 and the SMMU code are ios-apollo's xnu/bsd/dev/sweet_wifi.c, sweet_pcie.c, sweet_pcie_phy.h and
 * sweet_rpmh.c (xnu 0126, proven on this phone under XNU: boots 49-60, Wi-Fi working), which came from the
 * Xiaomi Mi 10 port github.com/JopangJovan/hackios-xnu-kernel-umi (umi_pcie.c, umi_wifi.c, umi_rpmh.c) and
 * HackiOS (github.com/oosexgang/hackios-xnu-kernel, hakios.wifi.c); APSL 2.0. Their hardware sequence, values
 * and checks are unchanged. Step 3 is new: the DesignWare ECAM set-up of Linux's pcie-designware-host.c
 * (dw_pcie_config_ecam_iatu) and Qualcomm's ECAM blocker registers (pcie-qcom.c PARF_ECAM_BASE,
 * PARF_BLOCK_SLV_AXI_*, PARF_SYS_CTRL bit 26). Step 4's tables are new (ios-apollo mapped its own buffers at
 * 0xa0000000; Windows' driver hands the radio physical addresses, so the context maps RAM 1:1).
 *
 * Windows specifics, as apollogpu: every register access is one ldr/str (apollowifi_asm.asm; the hypervisor
 * emulates the SMMU's global registers); boot-args are registry values under
 * HKLM\SYSTEM\CurrentControlSet\Services\apollowifi\Parameters: Steps (1..4, default 2), Keep (0/1), Deep (0/1:
 * also read bus 1 device 1 in step 3); every line goes to Parameters\Log and \Status, and the hive is flushed
 * to disk before each step and each read that could fault, so after a freeze the log shows where it stopped.
 * Plain legacy driver, demand start: it runs when started (sc start apollowifi), never at boot.
 */
#include <ntddk.h>
#include <ntstrsafe.h>

typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
typedef ULONG_PTR vm_offset_t;
typedef int bool;
#define true  1
#define false 0
#define UINT32_MAX 0xffffffffu

ULONG apollo_rd32(ULONG_PTR Address);
VOID apollo_wr32(ULONG_PTR Address, ULONG Value);
VOID apollo_dc_civac(ULONG_PTR Start, SIZE_T Size);

#pragma warning(disable : 4127)   /* conditional expression is constant */

#define REG_PARAMS L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\apollowifi\\Parameters"
#define POOL_TAG   'iwpA'

/* errno values of the XNU source, kept for the log */
#define EIO        5
#define ENOMEM     12
#define EBUSY      16
#define ENODEV     19
#define EINVAL     22
#define ETIMEDOUT  60

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD Unload;
static KSTART_ROUTINE ProbeThread;
static PKTHREAD Thread;
static ULONG wifi_keep, wifi_deep;

/* ---------- the log: every line to DbgPrint and to the registry (Parameters\Log, \Status) ---------- */
#define LOG_SIZE (96u * 1024u)
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

static VOID wlog(const char *Fmt, ...)
{
    CHAR line[512];
    va_list args;
    size_t n = 0;
    bool flush;

    va_start(args, Fmt);
    (void)RtlStringCbVPrintfA(line, sizeof(line), Fmt, args);
    va_end(args);
    (void)RtlStringCbLengthA(line, sizeof(line), &n);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "apollowifi: %s", line);
    if (LogBuf && LogLen + n + 2 < LOG_SIZE) {
        RtlCopyMemory(LogBuf + LogLen, line, n);
        LogLen += (ULONG)n;
        if (n == 0 || line[n - 1] != '\n')
            LogBuf[LogLen++] = '\n';
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    /* "SWEET WIFI: step N: ..." and "about to ..." say what is touched next; "done" ends the run */
    flush = (n >= 16 && RtlCompareMemory(line, "SWEET WIFI: step", 16) == 16) || Contains(line, n, "about to") ||
            (n >= 16 && RtlCompareMemory(line, "SWEET WIFI: done", 16) == 16);
    LogSave(line, (ULONG)n, flush);
}
#define printf wlog

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

/* ---------- registers ---------- */
#define wifi_rd(va)        apollo_rd32(va)
#define wifi_wr(va, value) apollo_wr32((va), (value))

/* Mapped once and unmapped when the run is over. The PCIe space is one piece: DBI + ELBI (0x60000000), the
 * unrolled iATU (0x60001000), bus 0's and bus 1's ECAM megabytes (the endpoint's configuration at 0x60100000). */
static struct {
    uint32_t pa, size;
    vm_offset_t va;
} wifi_regions[] = {
    { 0x00100000u, 0xa0000u, 0 },   /* GCC, including the PCIe TBU clock */
    { 0x0f900000u, 0x80000u, 0 },   /* TLMM north tile: GPIO 20, 79-81, 124 */
    { 0x01c00000u, 0x3000u, 0 },    /* PCIe0 PARF */
    { 0x01c06000u, 0x1000u, 0 },    /* PCIe0 QMP PHY */
    { 0x60000000u, 0x200000u, 0 },  /* DBI, iATU, ECAM bus 0 and bus 1 */
    { 0x60300000u, 0x100000u, 0 },  /* QCA6390 BAR */
};
#define WIFI_BAR        0x60300000u
#define WIFI_NREGIONS   (sizeof(wifi_regions) / sizeof(wifi_regions[0]))

static bool wifi_map_regions(void)
{
    unsigned i;
    for (i = 0; i < WIFI_NREGIONS; i++) {
        PHYSICAL_ADDRESS p;
        if (wifi_regions[i].va)
            continue;
        p.QuadPart = wifi_regions[i].pa;
        wifi_regions[i].va = (vm_offset_t)MmMapIoSpaceEx(p, wifi_regions[i].size, PAGE_READWRITE | PAGE_NOCACHE);
        if (!wifi_regions[i].va) {
            printf("SWEET WIFI: cannot map %#x (+%#x)\n", wifi_regions[i].pa, wifi_regions[i].size);
            return false;
        }
    }
    return true;
}

static void wifi_unmap_regions(void)
{
    unsigned i;
    for (i = 0; i < WIFI_NREGIONS; i++)
        if (wifi_regions[i].va) {
            MmUnmapIoSpace((PVOID)wifi_regions[i].va, wifi_regions[i].size);
            wifi_regions[i].va = 0;
        }
}

static vm_offset_t wifi_reg(uint32_t pa)
{
    unsigned i;
    if (pa & 3)
        return 0;
    for (i = 0; i < WIFI_NREGIONS; i++) {
        uint32_t offset = pa - wifi_regions[i].pa;
        if (wifi_regions[i].va && pa >= wifi_regions[i].pa && offset <= wifi_regions[i].size - 4)
            return wifi_regions[i].va + offset;
    }
    printf("SWEET WIFI: unmapped register %#x\n", pa);
    return 0;
}

static uint32_t rd(uint32_t pa)
{
    vm_offset_t va = wifi_reg(pa);
    return va ? wifi_rd(va) : UINT32_MAX;
}

static void wr(uint32_t pa, uint32_t value)
{
    vm_offset_t va = wifi_reg(pa);
    if (va)
        wifi_wr(va, value);
}

static void report(const char *stage, uint32_t value)
{
    printf("SWEET WIFI: %s = %#x\n", stage, value);
}

/* ---------- RPMh: ACTIVE votes for the radio's and PCIe0's rails (sweet_rpmh.c) ---------- */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DB_PA        0x80860000u
#define RH_DB_SIZE      0x20000u
#define RH_DRV          2u
#define RH_ACTIVE_N     2u
#define RH_NORMAL_N     8u
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_VRM          4u
#define RH_TIMEOUT_US   100000u

static vm_offset_t rh_rsc, rh_tcs, rh_db;
static unsigned rh_ncpt;
static bool rh_poisoned;

static vm_offset_t rh_io(uint32_t pa, uint32_t size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = pa;
    return (vm_offset_t)MmMapIoSpaceEx(p, size, PAGE_READWRITE | PAGE_NOCACHE);
}

static bool rh_sync(uint32_t off, uint32_t value)
{
    unsigned i;
    wifi_wr(rh_tcs + off, value);
    for (i = 0; i < 1000; ++i) {
        if (wifi_rd(rh_tcs + off) == value)
            return true;
        delay(1);
    }
    printf("SWEET RPMH: readback timeout off=%#x want=%#x got=%#x\n", off, value, wifi_rd(rh_tcs + off));
    return false;
}

static uint16_t rh_db16(uint32_t off)
{
    uint32_t v = wifi_rd(rh_db + (off & ~3u));
    return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool rh_range(uint32_t off, uint32_t size)
{
    return off <= RH_DB_SIZE && size <= RH_DB_SIZE - off;
}

static int rh_map(void)
{
    uint32_t id, solver, config;
    unsigned major, count;
    if (!rh_rsc)
        rh_rsc = rh_io(RH_RSC_PA, 0x100);
    if (!rh_tcs)
        rh_tcs = rh_io(RH_TCS_PA, RH_TCS_SIZE);
    if (!rh_db)
        rh_db = rh_io(RH_DB_PA, RH_DB_SIZE);
    if (!rh_rsc || !rh_tcs || !rh_db) {
        printf("SWEET RPMH: RSC/cmd-db mapping failed\n");
        return ENOMEM;
    }
    id = wifi_rd(rh_rsc);
    solver = wifi_rd(rh_rsc + 4);
    config = wifi_rd(rh_rsc + 0x0c);
    major = (id >> 16) & 0xffu;
    count = (config >> (6u * RH_DRV)) & 0x3fu;
    rh_ncpt = (config >> 27) & 0x1fu;
    printf("SWEET RPMH: rsc=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n", RH_RSC_PA, RH_DRV, id, solver,
           config, count, rh_ncpt);
    if ((major != 1 && major != 2) || config == 0xffffffffu || solver == 0xffffffffu || count < RH_NORMAL_N ||
        count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
        printf("SWEET RPMH: unsupported RSC geometry/revision\n");
        return ENODEV;
    }
    if (wifi_rd(rh_db + 4) != RH_DB_MAGIC || wifi_rd(rh_db) != 1 || (wifi_rd(rh_db + 140) & 1u)) {
        printf("SWEET RPMH: no cmd-db at %#x (magic %#x)\n", RH_DB_PA, wifi_rd(rh_db + 4));
        return EINVAL;
    }
    return 0;
}

static int rh_lookup(const char *res, uint32_t *address)
{
    uint32_t key[2] = { 0, 0 };
    unsigned i, j, matches = 0;
    for (i = 0; i < 8 && res[i]; ++i)
        key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
    for (i = 0; i < 8; ++i) {
        uint32_t h = 8u + 16u * i, entries, aux;
        unsigned slave = rh_db16(h), count;
        if (!slave)
            break;
        entries = RH_DB_HEADER + rh_db16(h + 2);
        aux = RH_DB_HEADER + rh_db16(h + 4);
        count = rh_db16(h + 6);
        if (slave < 3 || slave > 5 || (entries & 3u) || !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
            printf("SWEET RPMH: malformed cmd-db slave header %u\n", i);
            return EINVAL;
        }
        for (j = 0; j < count; ++j) {
            uint32_t e = entries + RH_DB_ENTRY * j, addr;
            if (wifi_rd(rh_db + e) != key[0] || wifi_rd(rh_db + e + 4) != key[1])
                continue;
            addr = wifi_rd(rh_db + e + 16);
            if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15u) ||
                !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
                printf("SWEET RPMH: invalid VRM entry for %s\n", res);
                return EINVAL;
            }
            if (matches && *address != addr) {
                printf("SWEET RPMH: conflicting addresses for %s: %#x / %#x\n", res, *address, addr);
                return EINVAL;
            }
            *address = addr;
            ++matches;
        }
    }
    if (matches == 0) {
        printf("SWEET RPMH: %s not in cmd-db; refusing\n", res);
        return ENODEV;
    }
    return 0;
}

static bool rh_free(unsigned t)
{
    uint32_t b = t * RH_TCS_STRIDE;
    return wifi_rd(rh_tcs + b + RH_STATUS) != 0 && wifi_rd(rh_tcs + b + RH_STATUS) != 0xffffffffu &&
           (wifi_rd(rh_tcs + b + RH_CONTROL) & ~RH_AMC_EN) == 0 && wifi_rd(rh_tcs + b + RH_ENABLE) == 0 &&
           wifi_rd(rh_tcs + b + RH_WAIT) == 0 && !(wifi_rd(rh_tcs + RH_IRQ_STATUS) & (1u << t));
}

/* A regulator's voltage (ADDRESS) and enable (ADDRESS + 4) in one free ACTIVE TCS, waited for (rh_send, SMPS form). */
static int rh_send(uint32_t address, unsigned mv)
{
    const uint32_t offsets[2] = { 0, 4 }, values[2] = { mv, 1 };
    const unsigned commands = 2;
    const uint32_t command_mask = 3;
    int chosen = -1;
    unsigned t, c, us;
    uint32_t b, bit, irq_en, inherited_control, irq = 0, idle = 0, status[2] = { 0, 0 };
    bool done = false, progress = false;

    for (t = 0; t < RH_ACTIVE_N; ++t) {
        uint32_t enabled;
        b = t * RH_TCS_STRIDE;
        enabled = wifi_rd(rh_tcs + b + RH_ENABLE);
        if (enabled & ~((1u << rh_ncpt) - 1u))
            return EIO;
        for (c = 0; c < rh_ncpt; ++c) {
            if ((enabled & (1u << c)) && (wifi_rd(rh_tcs + b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address) {
                printf("SWEET RPMH: conflicting inherited request in TCS%u\n", t);
                return EBUSY;
            }
        }
        if (chosen < 0 && rh_free(t))
            chosen = (int)t;
    }
    if (chosen < 0) {
        printf("SWEET RPMH: no empty idle ACTIVE TCS; leaving inherited state intact\n");
        return EBUSY;
    }
    t = (unsigned)chosen;
    b = t * RH_TCS_STRIDE;
    bit = 1u << t;
    irq_en = wifi_rd(rh_tcs + RH_IRQ_EN);
    inherited_control = wifi_rd(rh_tcs + b + RH_CONTROL);
    if (irq_en == 0xffffffffu || !rh_free(t))
        return EBUSY;
    if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
        rh_poisoned = true;
        return EIO;
    }
    if (!rh_free(t)) {
        if (!rh_sync(RH_IRQ_EN, (wifi_rd(rh_tcs + RH_IRQ_EN) & ~bit) | (irq_en & bit)))
            rh_poisoned = true;
        return EBUSY;
    }
    rh_poisoned = true;         /* from here an uncertain write/timeout quarantines the slot */
    for (c = 0; c < commands; ++c) {
        uint32_t cmd = b + c * RH_CMD_STRIDE;
        if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) || !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
            !rh_sync(cmd + RH_DATA, values[c]))
            return EIO;
    }
    if (!rh_sync(b + RH_WAIT, command_mask) || !rh_sync(b + RH_ENABLE, command_mask) || !rh_sync(b + RH_CONTROL, 0) ||
        !rh_sync(b + RH_CONTROL, RH_AMC_EN))
        return EIO;
    wifi_wr(rh_tcs + b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
    for (us = 0; us < RH_TIMEOUT_US; ++us) {
        bool complete = true;
        irq = wifi_rd(rh_tcs + RH_IRQ_STATUS);
        idle = wifi_rd(rh_tcs + b + RH_STATUS);
        for (c = 0; c < commands; ++c) {
            status[c] = wifi_rd(rh_tcs + b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
            if (status[c] == 0xffffffffu || (status[c] & (RH_ISSUED | RH_COMPLETE)) != (RH_ISSUED | RH_COMPLETE))
                complete = false;
        }
        if (idle == 0 || !complete)
            progress = true;
        if (idle && idle != 0xffffffffu && irq != 0xffffffffu && ((irq & bit) || progress) && complete) {
            done = true;
            break;
        }
        delay(1);
    }
    if (!done) {
        printf("SWEET RPMH: TCS%u timeout irq=%#x idle=%#x cmd-status=%#x/%#x; quarantined\n", t, irq, idle, status[0],
               status[1]);
        return ETIMEDOUT;
    }
    if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, inherited_control) ||
        !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0))
        return EIO;
    wifi_wr(rh_tcs + RH_IRQ_CLEAR, bit);
    if ((wifi_rd(rh_tcs + RH_IRQ_STATUS) & bit) ||
        !rh_sync(RH_IRQ_EN, (wifi_rd(rh_tcs + RH_IRQ_EN) & ~bit) | (irq_en & bit)))
        return EIO;
    rh_poisoned = false;
    return 0;
}

/* apollo's QCA6390 and PCIe0 supplies (sweet_rpmh_wifi_power): ACTIVE votes, held until reset, nothing switched off. */
static int sweet_rpmh_wifi_power(void)
{
    static const struct { const char *resource; unsigned mv; } rails[] = {
        { "smpa4", 1800 },      /* PM8150 S4: WLAN IO */
        { "smpa6", 950 },       /* PM8150 S6: WLAN AON and, on apollo, digital */
        { "smpa5", 1900 },      /* PM8150 S5: WLAN RFA1 */
        { "smpc8", 1350 },      /* PM8150A S8: WLAN RFA2 */
        { "ldoa9", 1200 },      /* PM8150 L9: PCIe vreg-1p8, DT asks 1.2 V */
        { "ldoa5", 880 },       /* PM8150 L5: PCIe vreg-0p9 */
    };
    uint32_t addresses[sizeof(rails) / sizeof(rails[0])];
    unsigned i, tries;
    int error = rh_map();
    /* Every resource is found before the first vote is sent. */
    for (i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i)
        error = rh_lookup(rails[i].resource, &addresses[i]);
    for (i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i) {
        if (rh_poisoned) {
            printf("SWEET RPMH: previous uncertain transaction; refusing further access\n");
            error = EIO;
            break;
        }
        printf("SWEET RPMH: Wi-Fi/PCIe %s at least %u mV\n", rails[i].resource, rails[i].mv);
        for (tries = 0; tries < 20; tries++) {
            error = rh_send(addresses[i], rails[i].mv);
            if (error != EBUSY)
                break;
            delay(5000);
        }
    }
    if (error)
        printf("SWEET RPMH: Wi-Fi rails failed errno=%d\n", error);
    return error;
}

/* ---------- PCIe0 bootstrap for the QCA6390 (sweet_pcie.c, unchanged sequence) ---------- */
/* PCIe0 QMP PHY v4.4.14, kona-pcie.dtsi pcie0 qcom,phy-sequence (UMI's and apollo's are the same 99 entries). */
static const struct { uint16_t offset; uint8_t value; } sweet_pcie_phy[] = {
    { 0x840, 0x03 }, { 0x094, 0x08 }, { 0x154, 0x34 }, { 0x16c, 0x08 }, { 0x058, 0x0f }, { 0x0a4, 0x42 },
    { 0x110, 0x24 }, { 0x11c, 0x03 }, { 0x118, 0xb4 }, { 0x10c, 0x02 }, { 0x1bc, 0x11 }, { 0x0bc, 0x82 },
    { 0x0d4, 0x03 }, { 0x0d0, 0x55 }, { 0x0cc, 0x55 }, { 0x0b0, 0x1a }, { 0x0ac, 0x0a }, { 0x0c4, 0x68 },
    { 0x0e0, 0x02 }, { 0x0dc, 0xaa }, { 0x0d8, 0xab }, { 0x0b8, 0x34 }, { 0x0b4, 0x14 }, { 0x158, 0x01 },
    { 0x074, 0x06 }, { 0x07c, 0x16 }, { 0x084, 0x36 }, { 0x078, 0x06 }, { 0x080, 0x16 }, { 0x088, 0x36 },
    { 0x1b0, 0x1e }, { 0x1ac, 0xca }, { 0x1b8, 0x18 }, { 0x1b4, 0xa2 }, { 0x050, 0x07 }, { 0x010, 0x01 },
    { 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 }, { 0x030, 0x4c }, { 0x034, 0x06 },
    { 0x29c, 0x12 }, { 0x284, 0x35 }, { 0x23c, 0x11 }, { 0x51c, 0x03 }, { 0x518, 0x1c }, { 0x524, 0x1e },
    { 0x4e8, 0x00 }, { 0x4ec, 0x0e }, { 0x4f0, 0x4a }, { 0x4f4, 0x0f }, { 0x5b4, 0x04 }, { 0x434, 0x7f },
    { 0x444, 0x70 }, { 0x510, 0x17 }, { 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x598, 0xd4 }, { 0x59c, 0x54 },
    { 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 }, { 0x584, 0x24 }, { 0x588, 0xe4 }, { 0x58c, 0xec },
    { 0x590, 0x3b }, { 0x594, 0x36 }, { 0x570, 0x3f }, { 0x574, 0x3f }, { 0x578, 0xff }, { 0x57c, 0x7f },
    { 0x580, 0x14 }, { 0x4fc, 0x00 }, { 0x4f8, 0xc0 }, { 0x460, 0x30 }, { 0x464, 0x00 }, { 0x5bc, 0x0c },
    { 0x4dc, 0x1b }, { 0x408, 0x0c }, { 0x414, 0x03 }, { 0x5b8, 0x30 }, { 0x9a4, 0x01 }, { 0xc90, 0x00 },
    { 0xc40, 0x01 }, { 0xc48, 0x01 }, { 0xc50, 0x00 }, { 0xcb4, 0x33 }, { 0xcbc, 0x00 }, { 0xce0, 0x58 },
    { 0xca4, 0x0f }, { 0x048, 0x90 }, { 0xc1c, 0xc1 }, { 0x988, 0x77 }, { 0x998, 0x0b }, { 0x8dc, 0x0d },
    { 0x9ec, 0x12 }, { 0x800, 0x00 }, { 0x844, 0x03 },
};
C_ASSERT(sizeof(sweet_pcie_phy) / sizeof(sweet_pcie_phy[0]) == 99);

#define GCC  0x00100000u
#define GPIO 0x0f900000u /* Kona NORTH tile: TLMM base + 0x900000. */
#define PARF 0x01c00000u
#define PHY  0x01c06000u
#define DBI  0x60000000u
#define ATU  0x60001000u
#define CONF 0x60100000u
#define BAR  0x60300000u
#define BIT(n) (1u << (n))
#define TRY(expr) do { error = (expr); if (error) goto fail; } while (0)

static bool pcie_failed, pcie_enumerated;

static int update(uint32_t addr, uint32_t clear, uint32_t set)
{
    uint32_t value = rd(addr);
    if (value == UINT32_MAX)
        return EIO;
    wr(addr, (value & ~clear) | set);
    return 0;
}

static int wait_bits(uint32_t addr, uint32_t mask, uint32_t wanted, unsigned timeout_us)
{
    uint32_t value = 0;
    unsigned us;
    for (us = 0; us < timeout_us; us += 10) {
        value = rd(addr);
        if (value == UINT32_MAX)
            return EIO;
        if ((value & mask) == wanted)
            return 0;
        delay(10);
    }
    report("poll timeout address", addr);
    report("poll timeout value", value);
    return ETIMEDOUT;
}

static int gpio_output(unsigned pin, bool high, uint32_t config)
{
    /* Set output latch before output-enable/mux to avoid a reset glitch. */
    int error = update(GPIO + pin * 0x1000 + 4, BIT(1), high ? BIT(1) : 0);
    if (error)
        return error;
    return update(GPIO + pin * 0x1000, 0x3ff, config | BIT(9));
}

static int source_xo(uint32_t cmd)
{
    /* 19.2 MHz XO, divide by 1. This is the stock Gen1/Gen2 refgen rate. */
    int error = update(cmd + 4, 0x371f, 1);
    if (error)
        return error;
    error = update(cmd, 0, 1);
    return error ? error : wait_bits(cmd, 1, 0, 10000);
}

static int branch(uint32_t offset)
{
    int error = update(GCC + offset, 0, 1);
    return error ? error : wait_bits(GCC + offset, BIT(31), 0, 10000);
}

static int clocks(void)
{
    static const uint32_t gates[] = { 0x6b010, 0x6b014, 0x6b01c, 0x6b024, 0x6b028 };
    static const uint32_t resets[] = { 0x6b000, 0x6c01c };
    unsigned i;
    int error;
    /* GDSC status is checked before accessing the PCIe controller/PHY. */
    TRY(update(GCC + 0x6b004, 7, 0));
    delay(1);
    TRY(wait_bits(GCC + 0x6b004, BIT(31), BIT(31), 10000));
    TRY(source_xo(GCC + 0x6b038));
    TRY(source_xo(GCC + 0x6f014));
    TRY(branch(0x8c004)); /* QCA6390 reference clock/LDO. */
    TRY(branch(0x6f02c));
    TRY(branch(0x9000c));
    TRY(branch(0x8d058));
    /* The AXI and PIPE clocks must not retain memory/peripheral state. */
    TRY(update(GCC + 0x6b01c, 0x6000, 0));
    /* Force diagnostic AHB/SLV clocks on. In HWCG mode the halt bit can
     * assert merely because the bus is idle, so it is not a readiness test. */
    TRY(update(GCC + 0x6b014, 0x6002, 0));
    TRY(update(GCC + 0x6b024, BIT(1), 0));
    TRY(update(GCC + 0x6b02c, 0x6000, 0));
    TRY(update(GCC + 0x52008, 0, 0x2f)); /* All PCIe0 votes except PIPE. */
    for (i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
        TRY(wait_bits(GCC + gates[i], BIT(31), 0, 10000));
    for (i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        TRY(update(GCC + resets[i], 0, 1));
        delay(1000);
        TRY(update(GCC + resets[i], 1, 0));
    }
    return 0;
fail:
    return error;
}

static int outbound(unsigned index, uint32_t type, uint32_t control2, uint32_t start, uint32_t end, uint32_t target)
{
    uint32_t a = ATU + index * 0x200;
    wr(a + 4, 0);
    wr(a, type);
    wr(a + 8, start);
    wr(a + 12, 0);
    wr(a + 16, end);
    wr(a + 20, target);
    wr(a + 24, 0);
    wr(a + 4, BIT(31) | control2);
    return wait_bits(a + 4, BIT(31), BIT(31), 1000);
}

/* Hold the endpoint in reset. Rail/clock votes remain until hardware reset; shared supplies are never disabled. */
static void sweet_pcie_quiesce(void)
{
    uint32_t command = rd(CONF + 4);
    /* Bus mastering is never enabled by this driver; if somebody did, stop it before reset. */
    if (pcie_enumerated && command != UINT32_MAX && (command & 4)) {
        wr(CONF + 4, (command & 0xffff) & ~4u);
        wr(DBI + 4, 2);
    }
    (void)gpio_output(79, false, 1);
    delay(1000); /* PERST propagation before the SMMU can be restored. */
    (void)gpio_output(20, false, 3 | (7u << 6));
    pcie_enumerated = false;
}

static int sweet_pcie_enumerate(void)
{
    int error;
    unsigned i;
    uint32_t control, id = 0, command, bar, version;
    bool cold_pbl;

    TRY(gpio_output(79, false, 1));
    delay(1000);
    /* The PIPE clock is voted on once the PHY runs (below) and stays voted when the endpoint is put in reset:
     * start every enumeration as the first one does, with the vote off. */
    TRY(update(GCC + 0x52008, BIT(4), 0));
    TRY(sweet_rpmh_wifi_power());
    /* UMI stock pinctrl: CLKREQ mux1/pull-up; WAKE input/pull-up. */
    TRY(update(GPIO + 80 * 0x1000, 0x3ff, 7));
    TRY(update(GPIO + 81 * 0x1000, 0x3ff, 3));
    TRY(gpio_output(20, true, 3 | (7u << 6)));
    delay(1000);
    TRY(clocks());
    report("power and clocks ready", rd(GCC + 0x6b004));
    printf("SWEET WIFI: about to touch the PCIe0 controller (PARF %#x) and its PHY (%#x)\n", PARF, PHY);
    wr(PARF + 0x1000, 4); /* Root complex. */
    TRY(update(PARF + 0x40, 1, 0));
    wr(PARF + 0x350, 0);
    wr(PARF, 0x365e);
    TRY(update(PARF + 0x174, 0, BIT(4)));
    wr(PARF + 0x22c, 0); /* No unhandled PCIe interrupts during enumeration. */
    wr(PARF + 0x358, 0x04000000);
    TRY(update(PARF + 0x1a8, 0, BIT(31)));
    for (i = 0; i < sizeof(sweet_pcie_phy) / sizeof(sweet_pcie_phy[0]); i++)
        wr(PHY + sweet_pcie_phy[i].offset, sweet_pcie_phy[i].value);
    delay(1000);
    TRY(update(GCC + 0x52008, 0, BIT(4))); /* PIPE: vendor skips halt polling. */
    TRY(wait_bits(PHY + 0x814, BIT(6), 0, 100000));
    report("PHY ready", rd(PHY + 0x814));
    delay(10000); /* UMI DT ep-latency=10 ms. */
    TRY(gpio_output(79, true, 1));
    delay(1000);
    printf("SWEET WIFI: about to touch the controller's own configuration (DBI %#x) and train the link\n", DBI);
    TRY(update(DBI + 0x80c, 0x1f00, 0x100));
    wr(DBI + 0x8a8, 0x20);
    TRY(update(DBI + 0x890, BIT(0), 0));
    TRY(update(DBI + 0x8bc, 0, BIT(0)));
    wr(DBI + 0x154, 0x55555555);
    TRY(update(DBI + 0x8bc, BIT(0), 0));
    TRY(update(DBI + 0xa0, 0xf, 2)); /* Gen2 x1: stock UMI link rate. */
    /* Write only the control half of the PCIe capability (upper half is W1C). */
    control = rd(DBI + 0x78);
    if (control == UINT32_MAX) {
        error = EIO;
        goto fail;
    }
    wr(DBI + 0x78, (control & 0xffff) & ~0x7000u);
    TRY(update(PARF + 0x1b0, 0, BIT(8)));
    TRY(wait_bits(DBI + 0xf28, BIT(10), BIT(10), 100000));
    TRY(wait_bits(DBI + 0x80, BIT(29), BIT(29), 100000));
    report("link status", rd(DBI + 0x80));
    wr(DBI + 0x18, 0x00010100); /* Primary 0, secondary/subordinate 1. */
    wr(DBI + 0x20, 0x60306030); /* 1 MiB non-prefetchable bridge window. */
    wr(DBI + 0xb40, 0x14); /* AUX 19.2 MHz. */
    TRY(outbound(0, 4, 0, CONF, CONF + 0xfff, 0x01000000)); /* CFG0, 01:00.0 */
    printf("SWEET WIFI: about to read the endpoint's configuration through the link (%#x)\n", CONF);
    for (i = 0; i < 1000; i++) {
        id = rd(CONF);
        if (id == 0x110117cb)
            break;
        delay(1000);
    }
    report("endpoint PCI ID", id);
    if (id != 0x110117cb) {
        error = ENODEV;
        goto fail;
    }
    command = rd(CONF + 4);
    bar = rd(CONF + 0x10);
    if (command == UINT32_MAX || bar == UINT32_MAX || (bar & 1) || ((bar >> 1) & 3) == 1 || ((bar >> 1) & 3) == 3) {
        error = EIO;
        goto fail;
    }
    /* QCA6390's known 1 MiB BAR; never enable DMA before installing its SMMU. */
    wr(CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
    if (rd(CONF + 4) & 4) {
        error = EIO;
        goto fail;
    }
    /* The SID table can contain uninitialized values after reset. */
    for (i = 0; i < 256; i++) {
        wr(PARF + 0x2000 + 4 * i, 0);
        if (rd(PARF + 0x2000 + 4 * i) != 0) {
            error = EIO;
            goto fail;
        }
    }
    report("BDF-to-SID table initialized, DMA disabled", 256);
    wr(CONF + 0x10, BAR | (bar & 15));
    if ((bar & 6) == 4)
        wr(CONF + 0x14, 0);
    TRY(outbound(1, 0, 0, BAR, BAR + 0xfffff, BAR));
    wr(DBI + 4, 2); /* Root bridge memory decode only. */
    wr(CONF + 4, ((command & 0xffff) & ~7u) | 2 | BIT(10));
    report("endpoint command", rd(CONF + 4));
    report("endpoint BAR0", rd(CONF + 0x10));
    if ((rd(CONF + 4) & 7) != 2 || (rd(CONF + 0x10) & ~15u) != BAR) {
        error = EIO;
        goto fail;
    }
    printf("SWEET WIFI: about to read the radio's own registers (BAR %#x)\n", BAR);
    version = rd(BAR + 8);
    report("MHI version (DMA disabled)", version);
    if (version != 0x01000000) {
        report("MHI register length", rd(BAR));
        report("MHI control", rd(BAR + 0x38));
        report("MHICFG", rd(BAR + 0x10));
        report("channel doorbells", rd(BAR + 0x18));
        report("event doorbells", rd(BAR + 0x20));
        report("BHI offset", rd(BAR + 0x28));
        report("BHIE offset", rd(BAR + 0x2c));
        if (rd(BAR + 0x28) == 0x100) {
            report("BHI major version", rd(BAR + 0x104));
            report("BHI execution environment", rd(BAR + 0x128));
        }
    }
    /* Captured on real UMI PBL: MHIVER has a placeholder, BHI is readable. */
    cold_pbl = version == 0xdeadbeef && rd(BAR) == 0x100 && rd(BAR + 0x18) == 0x300 && rd(BAR + 0x20) == 0x700 &&
               rd(BAR + 0x28) == 0x100 && rd(BAR + 0x2c) == 0x224 && rd(BAR + 0x104) == 1 && rd(BAR + 0x128) == 0;
    if (version != 0x01000000 && !cold_pbl) {
        error = ENODEV;
        goto fail;
    }
    if (cold_pbl)
        report("QCA6390 cold PBL signature validated", 1);
    pcie_enumerated = true;
    return 0;
fail:
    pcie_failed = true;
    report("enumeration failed errno", (uint32_t)error);
    sweet_pcie_quiesce();
    return error;
}

/* Step 1: always-readable GCC and TLMM only; nothing behind PCIe0. */
static void wifi_survey(void)
{
    static const struct { const char *name; uint32_t offset; } clks[] = {
        { "pcie0_bcr", 0x6b000 }, { "pcie0_gdsc", 0x6b004 }, { "pcie0_clock_votes", 0x52008 },
        { "pcie0_slv_q2a", 0x6b010 }, { "pcie0_slv_axi", 0x6b014 }, { "pcie0_mstr_axi", 0x6b01c },
        { "pcie0_cfg_ahb", 0x6b024 }, { "pcie0_aux", 0x6b028 }, { "pcie0_pipe", 0x6b02c },
        { "pcie0_aux_cmd", 0x6b038 }, { "pcie0_aux_cfg", 0x6b03c }, { "pcie0_phy_bcr", 0x6c01c },
        { "pcie0_phy_refgen", 0x6f02c }, { "pcie0_clkref", 0x8c004 },
    };
    static const struct { const char *name; uint32_t pin; } pins[] = {
        { "wlan_en", 20 }, { "bt_en", 21 }, { "pcie0_perst", 79 },
        { "pcie0_clkreq", 80 }, { "pcie0_wake", 81 }, { "wlan_sw_ctrl", 124 },
    };
    unsigned i;
    printf("SWEET WIFI: step 1: QCA6390 on PCIe0; read-only GCC/TLMM survey\n");
    for (i = 0; i < sizeof(clks) / sizeof(clks[0]); i++)
        printf("SWEET WIFI: GCC %s (%#x) = %#x\n", clks[i].name, GCC + clks[i].offset, rd(GCC + clks[i].offset));
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        printf("SWEET WIFI: GPIO%u %s cfg=%#x io=%#x\n", pins[i].pin, pins[i].name, rd(GPIO + pins[i].pin * 0x1000u),
               rd(GPIO + pins[i].pin * 0x1000u + 4));
}

/* Step 2, after the link: what pci.sys and the Windows driver will look at. Reads only. */
static void wifi_identity(void)
{
    uint32_t subsys = rd(CONF + 0x2c), link = rd(DBI + 0x80), cap, header;
    unsigned i;
    printf("SWEET WIFI: root port %04x:%04x class/rev %#x header %#x buses %#x; link speed Gen%u width x%u\n",
           rd(DBI) & 0xffff, rd(DBI) >> 16, rd(DBI + 8), (rd(DBI + 0xc) >> 16) & 0xff, rd(DBI + 0x18),
           (link >> 16) & 0xf, (link >> 20) & 0x3f);
    printf("SWEET WIFI: endpoint %04x:%04x class/rev %#x header %#x BAR0 %#x BAR1 %#x interrupt pin/line %#x\n",
           rd(CONF) & 0xffff, rd(CONF) >> 16, rd(CONF + 8), (rd(CONF + 0xc) >> 16) & 0xff, rd(CONF + 0x10),
           rd(CONF + 0x14), rd(CONF + 0x3c) & 0xffff);
    printf("SWEET WIFI: endpoint subsystem %04x:%04x -> hardware ID PCI\\VEN_17CB&DEV_1101&SUBSYS_%04X%04X\n",
           subsys & 0xffff, subsys >> 16, subsys >> 16, subsys & 0xffff);
    printf("SWEET WIFI: Qualcomm's Windows driver (qcwlan6800sc8180.inf) lists SUBSYS_010817CB: %s\n",
           subsys == 0x010817cbu ? "THIS RADIO MATCHES IT" : "this radio differs (one line to add to the INF)");
    cap = rd(CONF + 0x34) & 0xff;
    for (i = 0; i < 48 && cap >= 0x40 && cap <= 0xfc && !(cap & 3); i++) {
        header = rd(CONF + cap);
        if (header == UINT32_MAX)
            break;
        switch (header & 0xff) {
        case 0x01:
            printf("SWEET WIFI: capability %#x: power management, caps %#x, control/status %#x\n", cap, header >> 16,
                   rd(CONF + cap + 4));
            break;
        case 0x05:
            printf("SWEET WIFI: capability %#x: MSI, control %#x (%u vectors possible, %s, %s masking)\n", cap,
                   header >> 16, 1u << ((header >> 17) & 7), (header >> 23) & 1 ? "64-bit" : "32-bit",
                   (header >> 24) & 1 ? "per-vector" : "no");
            break;
        case 0x10:
            printf("SWEET WIFI: capability %#x: PCI Express, devcap %#x devctl/sta %#x linkcap %#x linkctl/sta %#x\n", cap,
                   rd(CONF + cap + 4), rd(CONF + cap + 8), rd(CONF + cap + 0xc), rd(CONF + cap + 0x10));
            break;
        case 0x11:
            printf("SWEET WIFI: capability %#x: MSI-X, control %#x table %#x\n", cap, header >> 16, rd(CONF + cap + 4));
            break;
        default:
            printf("SWEET WIFI: capability %#x: id %#x\n", cap, header & 0xff);
        }
        cap = (header >> 8) & 0xff;
    }
    cap = 0x100;
    for (i = 0; i < 16 && cap >= 0x100 && cap <= 0xffc && !(cap & 3); i++) {
        header = rd(CONF + cap);
        if (header == UINT32_MAX || header == 0)
            break;
        printf("SWEET WIFI: extended capability %#x: id %#x version %u\n", cap, header & 0xffff, (header >> 16) & 0xf);
        cap = header >> 20;
    }
}

/* ---------- step 3: the configuration space as ECAM, the way Qualcomm's MCFG describes it to pci.sys ---------- */
#define ECAM_BASE               DBI                     /* MCFG: segment 0 at 0x60000000 */
#define ECAM(bus, dev, fn)      (ECAM_BASE + ((uint32_t)(bus) << 20) + ((uint32_t)(dev) << 15) + ((uint32_t)(fn) << 12))
#define ATU_CFG_SHIFT_MODE      BIT(28)                 /* the BDF comes from address bits 27:12 */
#define PCI_WINDOW              0x60200000u             /* PCI0 _CRS: Memory32Fixed 0x60200000 + 0x01df0000 */
#define PCI_WINDOW_END          0x61feffffu
#define PARF_SYS_CTRL           0x000u
#define PARF_ECAM_BLOCKER_EN    BIT(26)
#define PARF_BLOCK_WR_BASE      0x360u                  /* base lo/hi, limit lo/hi */
#define PARF_BLOCK_RD_BASE      0x370u
#define PARF_ECAM_BASE          0x380u
#define ECAM_BLOCK_START        ECAM(0, 1, 0)           /* bus 0 has the root port alone, at device 0 */
#define ECAM_BLOCK_END          (ECAM(1, 0, 0) - 1)

static bool ecam_on;

static bool wifi_ecam(void)
{
    static const uint32_t blocker[10][2] = {
        { PARF_ECAM_BASE, ECAM_BASE }, { PARF_ECAM_BASE + 4, 0 },
        { PARF_BLOCK_WR_BASE, ECAM_BLOCK_START }, { PARF_BLOCK_WR_BASE + 4, 0 },
        { PARF_BLOCK_WR_BASE + 8, ECAM_BLOCK_END }, { PARF_BLOCK_WR_BASE + 12, 0 },
        { PARF_BLOCK_RD_BASE, ECAM_BLOCK_START }, { PARF_BLOCK_RD_BASE + 4, 0 },
        { PARF_BLOCK_RD_BASE + 8, ECAM_BLOCK_END }, { PARF_BLOCK_RD_BASE + 12, 0 },
    };
    uint32_t root, endpoint, mhi_before = rd(BAR + 8), sys, value;
    unsigned i;
    bool taken = true, blocked = false;

    printf("SWEET WIFI: step 3: ECAM at %#x for pci.sys: bus 1 by iATU 0 in CFG-shift mode, memory %#x..%#x passed on\n",
           ECAM_BASE, PCI_WINDOW, PCI_WINDOW_END);
    printf("SWEET WIFI: PARF before: sys_ctrl %#x ecam_base %#x blocker wr %#x..%#x rd %#x..%#x\n", rd(PARF + PARF_SYS_CTRL),
           rd(PARF + PARF_ECAM_BASE), rd(PARF + PARF_BLOCK_WR_BASE), rd(PARF + PARF_BLOCK_WR_BASE + 8),
           rd(PARF + PARF_BLOCK_RD_BASE), rd(PARF + PARF_BLOCK_RD_BASE + 8));
    /* bus 1: one megabyte, type 0 requests, device and function from the address (Linux dw_pcie_config_ecam_iatu) */
    if (outbound(0, 4, ATU_CFG_SHIFT_MODE, ECAM(1, 0, 0), ECAM(2, 0, 0) - 1, 0)) {
        printf("SWEET WIFI: iATU 0 did not take the CFG-shift region\n");
        return false;
    }
    /* the bridge window Windows may place BARs in, 1:1 (the radio's BAR at 0x60300000 is inside) */
    if (outbound(1, 0, 0, PCI_WINDOW, PCI_WINDOW_END, PCI_WINDOW)) {
        printf("SWEET WIFI: iATU 1 did not take the memory window\n");
        return false;
    }
    wr(DBI + 0x20, ((PCI_WINDOW_END >> 16) & 0xfff0u) << 16 | ((PCI_WINDOW >> 16) & 0xfff0u));   /* bridge window */
    ecam_on = true;
    printf("SWEET WIFI: about to read the root port (%#x) and the endpoint (%#x) through ECAM\n", ECAM(0, 0, 0),
           ECAM(1, 0, 0));
    root = rd(ECAM(0, 0, 0));
    endpoint = rd(ECAM(1, 0, 0));
    printf("SWEET WIFI: ECAM 00:00.0 = %#x, 01:00.0 = %#x (subsystem %#x); the radio's registers %s\n", root, endpoint,
           rd(ECAM(1, 0, 0) + 0x2c), rd(BAR + 8) == mhi_before ? "still answer" : "DO NOT ANSWER AS BEFORE");
    if (endpoint != 0x110117cb || rd(BAR + 8) != mhi_before) {
        printf("SWEET WIFI: ECAM: the endpoint is not where pci.sys would look for it\n");
        return false;
    }
    /* The rest of bus 0: without a blocker a read there leaves the controller as a memory request nobody claims. */
    for (i = 0; i < 10; i++)
        wr(PARF + blocker[i][0], blocker[i][1]);
    for (i = 0; i < 10; i++) {
        value = rd(PARF + blocker[i][0]);
        if (value != blocker[i][1]) {
            printf("SWEET WIFI: PARF %#x reads %#x, not %#x\n", blocker[i][0], value, blocker[i][1]);
            taken = false;
        }
    }
    sys = rd(PARF + PARF_SYS_CTRL);
    if (taken) {
        wr(PARF + PARF_SYS_CTRL, sys | PARF_ECAM_BLOCKER_EN);
        taken = (rd(PARF + PARF_SYS_CTRL) & PARF_ECAM_BLOCKER_EN) != 0;
    }
    printf("SWEET WIFI: ECAM blocker registers %s (sys_ctrl %#x -> %#x)\n", taken ? "taken" : "NOT TAKEN", sys,
           rd(PARF + PARF_SYS_CTRL));
    if (!taken) {
        printf("SWEET WIFI: ECAM: no blocker on this controller; bus 0 devices 1-31 are not read (pci.sys would)\n");
        return false;
    }
    printf("SWEET WIFI: about to read bus 0 device 1 (%#x), which the blocker should answer with all-ones\n",
           ECAM(0, 1, 0));
    value = rd(ECAM(0, 1, 0));
    printf("SWEET WIFI: ECAM 00:01.0 = %#x\n", value);
    blocked = value == UINT32_MAX;
    if (blocked) {
        printf("SWEET WIFI: about to read bus 0 device 31 function 7 (%#x)\n", ECAM(0, 31, 7));
        value = rd(ECAM(0, 31, 7));
        printf("SWEET WIFI: ECAM 00:1f.7 = %#x\n", value);
        blocked = value == UINT32_MAX;
    }
    root = rd(ECAM(0, 0, 0));
    endpoint = rd(ECAM(1, 0, 0));
    printf("SWEET WIFI: with the blocker on: 00:00.0 = %#x, 01:00.0 = %#x, iATU registers %s\n", root, endpoint,
           (rd(ATU + 4) & (BIT(31) | ATU_CFG_SHIFT_MODE)) == (BIT(31) | ATU_CFG_SHIFT_MODE) ? "readable" : "NOT READABLE");
    if (wifi_deep && blocked) {
        printf("SWEET WIFI: about to read bus 1 device 1 (%#x): a type 0 request for a device number the radio is not\n",
               ECAM(1, 1, 0));
        value = rd(ECAM(1, 1, 0));
        printf("SWEET WIFI: ECAM 01:01.0 = %#x (%s)\n", value, value == 0x110117cb ?
               "the radio answers for every device number: pci.sys must scan device 0 only" : "not the radio");
    }
    if (blocked && endpoint == 0x110117cb && root != UINT32_MAX) {
        printf("SWEET WIFI: ECAM IS READY FOR pci.sys: root port at 00:00.0, the radio at 01:00.0, the rest of bus 0 empty\n");
        return true;
    }
    printf("SWEET WIFI: ECAM: bus 0 is not clean (device 1 answers %#x)\n", value);
    return false;
}

/* ---------- step 4: the radio's view of memory: a context of the apps SMMU (sweet_wifi.c) ---------- */
#define SMMU_PA                 0x15000000u
#define SMMU_SIZE               0x100000u
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800u + 4u * (n))
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))
#define SMMU_CBAR(n)            (4u * (n))              /* second page */
#define SMMU_CBA2R(n)           (0x800u + 4u * (n))     /* second page */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0              0xf404ff44u     /* 0 non-cacheable, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001feu
#define RADIO_STREAM            0x1c01u
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define RADIO_PTE_DEVICE        (RADIO_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)
#define RADIO_BLOCK             (RADIO_PTE & ~2ull)     /* a block at level 1 or 2: descriptor type 01 */
#define PTE_ADDRESS             0x0000fffffffff000ull
/* PCIe0's MSI doorbell: the GIC's message frame (MADT: GIC MSI frame at 0x17a10000; SETSPI at +0x40) */
#define RADIO_MSI_PAGE          0x17a10000ull
#define RADIO_RAM_START         0x080000000ull          /* DRAM begins at 2 GiB */
#define RADIO_RAM_END           0x400000000ull          /* 16 GiB: above any apollo's memory */
#define RADIO_TABLES            3u                      /* level 1, level 2 for the first GiB, level 3 for the MSI frame */

static const uint32_t smmu_saved_offsets[] = {
    SMMU_CB_SCTLR, SMMU_CB_TCR2, SMMU_CB_TTBR0, SMMU_CB_TTBR0 + 4, SMMU_CB_TTBR1,
    SMMU_CB_TTBR1 + 4, SMMU_CB_TCR, SMMU_CB_MAIR0, SMMU_CB_MAIR1,
};
#define SMMU_SAVED      (sizeof(smmu_saved_offsets) / sizeof(smmu_saved_offsets[0]))

static struct {
    vm_offset_t base;
    bool found, installed;
    uint32_t page, pages, match, context;
    uint32_t old_smr, old_s2cr, cbar, cba2r, saved[SMMU_SAVED];
    uint64_t *tables;
} rs;

static vm_offset_t smmu_second(uint32_t offset)
{
    return rs.base + rs.page + offset;
}

static vm_offset_t smmu_ctx(uint32_t offset)
{
    return rs.base + (rs.pages + rs.context) * rs.page + offset;
}

static uint64_t table_pa(unsigned n)
{
    return (uint64_t)MmGetPhysicalAddress((PVOID)(rs.tables + 512u * n)).QuadPart;
}

/* Identity: every GiB of RAM as one level-1 block (non-cacheable attribute 0, as ios-apollo's buffers), and the
 * GIC's MSI frame alone out of the first GiB, as device memory. The radio then uses the addresses Windows'
 * driver gives it, which are physical. */
static bool smmu_tables(void)
{
    PHYSICAL_ADDRESS low, high, skip;
    uint64_t gib;
    if (rs.tables)
        return true;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFLL;      /* the SMMU's 36-bit output */
    skip.QuadPart = 0;
    rs.tables = MmAllocateContiguousMemorySpecifyCache(4096u * RADIO_TABLES, low, high, skip, MmCached);
    if (!rs.tables) {
        printf("SWEET WIFI: no memory for the SMMU tables\n");
        return false;
    }
    RtlZeroMemory(rs.tables, 4096u * RADIO_TABLES);
    for (gib = RADIO_RAM_START >> 30; gib < RADIO_RAM_END >> 30; gib++)
        rs.tables[gib] = (gib << 30) | RADIO_BLOCK;
    rs.tables[0] = table_pa(1) | 3;
    rs.tables[512 + ((RADIO_MSI_PAGE >> 21) & 0x1ff)] = table_pa(2) | 3;
    rs.tables[1024 + ((RADIO_MSI_PAGE >> 12) & 0x1ff)] = RADIO_MSI_PAGE | RADIO_PTE_DEVICE;
    /* The SMMU walks the tables in memory, not in this core's cache. */
    apollo_dc_civac((ULONG_PTR)rs.tables, 4096u * RADIO_TABLES);
    return true;
}

static bool smmu_tlb_sync(void)
{
    unsigned us;
    if (!rs.installed)
        return true;
    wifi_wr(smmu_ctx(SMMU_CB_TLBIASID), rs.context);
    wifi_wr(smmu_ctx(SMMU_CB_TLBSYNC), 1);
    for (us = 0; us < 100000; us++) {
        uint32_t status = wifi_rd(smmu_ctx(SMMU_CB_TLBSTATUS));
        if (status == UINT32_MAX)
            return false;
        if (!(status & 1))
            return true;
        delay(1);
    }
    printf("SWEET WIFI: SMMU TLB sync timeout\n");
    return false;
}

/* Step 2: read-only. Which match registers exist and whether any already covers the radio. */
static void smmu_survey(void)
{
    uint32_t scr0, idr0, idr1;
    unsigned matches, valid = 0, covering = 0, n;
    if (!rs.base) {
        PHYSICAL_ADDRESS p;
        p.QuadPart = SMMU_PA;
        rs.base = (vm_offset_t)MmMapIoSpaceEx(p, SMMU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (!rs.base) {
        printf("SWEET WIFI: cannot map the apps SMMU\n");
        return;
    }
    printf("SWEET WIFI: about to read the apps SMMU's global registers (%#x; the hypervisor answers)\n", SMMU_PA);
    scr0 = wifi_rd(rs.base + SMMU_SCR0);
    idr0 = wifi_rd(rs.base + SMMU_IDR0);
    idr1 = wifi_rd(rs.base + SMMU_IDR1);
    matches = idr0 & 0xff;
    for (n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
        uint32_t smr = wifi_rd(rs.base + SMMU_SMR(n)), s2cr = wifi_rd(rs.base + SMMU_S2CR(n));
        if (!(smr >> 31))
            continue;
        valid++;
        if ((((smr & 0x7fff) ^ RADIO_STREAM) & ~((smr >> 16) & 0x7fff)) == 0) {
            covering++;
            printf("SWEET WIFI: SMMU SMR%u=%#x S2CR=%#x already covers stream %#x\n", n, smr, s2cr, RADIO_STREAM);
        }
    }
    printf("SWEET WIFI: apps SMMU scr0=%#x idr0=%#x idr1=%#x: %u of %u match registers valid, %u cover %#x, %u contexts\n",
           scr0, idr0, idr1, valid, matches, covering, RADIO_STREAM, idr1 & 0xff);
}

/* A free match register and the lowest unused, switched-off context. Reads only. */
static bool smmu_find(void)
{
    static bool used[256];
    uint32_t scr0 = wifi_rd(rs.base + SMMU_SCR0), idr0 = wifi_rd(rs.base + SMMU_IDR0), idr1 = wifi_rd(rs.base + SMMU_IDR1);
    uint32_t matches = idr0 & 0xff, contexts = idr1 & 0xff, n;
    unsigned i;
    if (scr0 == UINT32_MAX || idr0 == UINT32_MAX || idr1 == UINT32_MAX)
        return false;
    if (scr0 & 1u << 3) {
        printf("SWEET WIFI: the SMMU numbers streams with 16 bits; not handled\n");
        return false;
    }
    rs.page = idr1 >> 31 ? 0x10000u : 0x1000u;
    rs.pages = 2u << (idr1 >> 28 & 7);
    if (2u * rs.pages * rs.page != SMMU_SIZE) {
        printf("SWEET WIFI: SMMU says 2 x %u pages of %#x bytes, not 1 MiB; contexts not used\n", rs.pages, rs.page);
        return false;
    }
    RtlZeroMemory(used, sizeof(used));
    rs.match = ~0u;
    for (n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
        uint32_t smr = wifi_rd(rs.base + SMMU_SMR(n)), s2cr = wifi_rd(rs.base + SMMU_S2CR(n));
        if (smr >> 31) {
            if ((((smr & 0x7fff) ^ RADIO_STREAM) & ~((smr >> 16) & 0x7fff)) == 0) {
                printf("SWEET WIFI: stream %#x already belongs to SMR%u; left alone\n", RADIO_STREAM, n);
                return false;
            }
            used[s2cr & 0xff] = true;
        } else if (rs.match == ~0u) {
            rs.match = n;
            rs.old_smr = smr;
            rs.old_s2cr = s2cr;
        }
    }
    for (rs.context = 0; rs.context < contexts && used[rs.context]; rs.context++) {
    }
    if (rs.match == ~0u || rs.context >= contexts) {
        printf("SWEET WIFI: the SMMU has no free match register or context\n");
        return false;
    }
    for (i = 0; i < SMMU_SAVED; i++)
        rs.saved[i] = wifi_rd(smmu_ctx(smmu_saved_offsets[i]));
    rs.cbar = wifi_rd(smmu_second(SMMU_CBAR(rs.context)));
    rs.cba2r = wifi_rd(smmu_second(SMMU_CBA2R(rs.context)));
    printf("SWEET WIFI: free: SMR%u (%#x, s2cr %#x) and context %u at %#x: sctlr %#x cbar %#x fsr %#x\n", rs.match,
           rs.old_smr, rs.old_s2cr, rs.context, SMMU_PA + (rs.pages + rs.context) * rs.page, rs.saved[0], rs.cbar,
           wifi_rd(smmu_ctx(SMMU_CB_FSR)));
    if (rs.saved[0] & 1) {
        printf("SWEET WIFI: context %u is switched on although no stream uses it: somebody's; left alone\n", rs.context);
        return false;
    }
    if (wifi_rd(smmu_ctx(SMMU_CB_FSR)) & SMMU_FSR_ALL) {
        printf("SWEET WIFI: context %u has pending faults; left alone\n", rs.context);
        return false;
    }
    rs.found = true;
    return true;
}

static void smmu_faults(const char *when)
{
    uint32_t status, global;
    if (!rs.installed)
        return;
    status = wifi_rd(smmu_ctx(SMMU_CB_FSR));
    global = wifi_rd(rs.base + SMMU_SGFSR);
    if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
        printf("SWEET WIFI: %s: no SMMU faults (context %u fsr %#x)\n", when, rs.context, status);
        return;
    }
    printf("SWEET WIFI: %s: SMMU FAULT: context %u fsr %#x far %#x:%#x fsynr0 %#x; global sgfsr %#x sgfsynr %#x/%#x\n", when,
           rs.context, status, wifi_rd(smmu_ctx(SMMU_CB_FAR + 4)), wifi_rd(smmu_ctx(SMMU_CB_FAR)),
           wifi_rd(smmu_ctx(SMMU_CB_FSYNR0)), global, wifi_rd(rs.base + SMMU_SGFSYNR0), wifi_rd(rs.base + SMMU_SGFSYNR1));
}

/* Stream 0x1c01 through our context (tables made beforehand); every global write read back. */
static bool smmu_install(void)
{
    uint64_t root;
    uint32_t sees = 1u << 31 | RADIO_STREAM, smr, s2cr, cbar, sctlr, ttbr_lo, ttbr_hi;
    if (!rs.base || !smmu_find() || !smmu_tables())
        return false;
    root = table_pa(0) | (uint64_t)rs.context << 48;
    printf("SWEET WIFI: about to give SMMU context %u our tables (root %#x:%#x) and send stream %#x to it by SMR%u\n",
           rs.context, (uint32_t)((root & PTE_ADDRESS) >> 32), (uint32_t)(root & PTE_ADDRESS), RADIO_STREAM, rs.match);
    wifi_wr(smmu_ctx(SMMU_CB_SCTLR), 0);
    wifi_wr(smmu_second(SMMU_CBA2R(rs.context)), 1);         /* 64-bit descriptors */
    wifi_wr(smmu_second(SMMU_CBAR(rs.context)), SMMU_CBAR_STAGE_1);
    wifi_wr(smmu_ctx(SMMU_CB_TCR2), SMMU_TCR2);
    wifi_wr(smmu_ctx(SMMU_CB_TCR), SMMU_TCR_39_BITS);
    wifi_wr(smmu_ctx(SMMU_CB_TTBR0), (uint32_t)root);
    wifi_wr(smmu_ctx(SMMU_CB_TTBR0 + 4), (uint32_t)(root >> 32));
    wifi_wr(smmu_ctx(SMMU_CB_TTBR1), 0);
    wifi_wr(smmu_ctx(SMMU_CB_TTBR1 + 4), 0);
    wifi_wr(smmu_ctx(SMMU_CB_MAIR0), SMMU_MAIR0);
    wifi_wr(smmu_ctx(SMMU_CB_MAIR1), 0);
    wifi_wr(smmu_ctx(SMMU_CB_FSR), SMMU_FSR_ALL);
    rs.installed = true;            /* from here on there is something to put back */
    if (!smmu_tlb_sync())
        return false;
    wifi_wr(smmu_ctx(SMMU_CB_SCTLR), SMMU_SCTLR_ON);
    wifi_wr(rs.base + SMMU_S2CR(rs.match), rs.context);
    wifi_wr(rs.base + SMMU_SMR(rs.match), sees);
    /* The global registers are the hypervisor's to grant: see what it made of them. */
    smr = wifi_rd(rs.base + SMMU_SMR(rs.match));
    s2cr = wifi_rd(rs.base + SMMU_S2CR(rs.match));
    cbar = wifi_rd(smmu_second(SMMU_CBAR(rs.context)));
    sctlr = wifi_rd(smmu_ctx(SMMU_CB_SCTLR));
    ttbr_lo = wifi_rd(smmu_ctx(SMMU_CB_TTBR0));
    ttbr_hi = wifi_rd(smmu_ctx(SMMU_CB_TTBR0 + 4));
    printf("SWEET WIFI: SMMU read back: smr %#x s2cr %#x cbar %#x sctlr %#x ttbr0 %#x:%#x tcr %#x tcr2 %#x\n", smr, s2cr,
           cbar, sctlr, ttbr_hi, ttbr_lo, wifi_rd(smmu_ctx(SMMU_CB_TCR)), wifi_rd(smmu_ctx(SMMU_CB_TCR2)));
    if (smr != sees || (s2cr & 0x300ff) != rs.context || (cbar & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
        !(sctlr & 1) || ttbr_lo != (uint32_t)root || ttbr_hi != (uint32_t)(root >> 32)) {
        printf("SWEET WIFI: THE SMMU DID NOT TAKE IT: stream %#x is not going through context %u\n", RADIO_STREAM,
               rs.context);
        return false;
    }
    printf("SWEET WIFI: THE RADIO'S STREAM %#x GOES THROUGH SMMU CONTEXT %u: RAM %#x:%#x..%#x:%#x 1:1, MSI frame %#x\n",
           RADIO_STREAM, rs.context, (uint32_t)(RADIO_RAM_START >> 32), (uint32_t)RADIO_RAM_START,
           (uint32_t)((RADIO_RAM_END - 1) >> 32), (uint32_t)(RADIO_RAM_END - 1), (uint32_t)RADIO_MSI_PAGE);
    smmu_faults("installed");
    return true;
}

/* As found; called with the endpoint in reset (PERST low) or never enabled. */
static void smmu_restore(void)
{
    bool restored;
    unsigned i;
    if (!rs.found)
        return;
    if (rs.installed)
        smmu_faults("before restore");
    wifi_wr(rs.base + SMMU_SMR(rs.match), rs.old_smr);
    wifi_wr(rs.base + SMMU_S2CR(rs.match), rs.old_s2cr);
    wifi_wr(smmu_ctx(SMMU_CB_SCTLR), 0);
    for (i = 1; i < SMMU_SAVED; i++)
        wifi_wr(smmu_ctx(smmu_saved_offsets[i]), rs.saved[i]);
    wifi_wr(smmu_second(SMMU_CBAR(rs.context)), rs.cbar);
    wifi_wr(smmu_second(SMMU_CBA2R(rs.context)), rs.cba2r);
    wifi_wr(smmu_ctx(SMMU_CB_SCTLR), rs.saved[0]);
    restored = wifi_rd(rs.base + SMMU_SMR(rs.match)) == rs.old_smr && wifi_rd(rs.base + SMMU_S2CR(rs.match)) == rs.old_s2cr &&
               wifi_rd(smmu_second(SMMU_CBAR(rs.context))) == rs.cbar && wifi_rd(smmu_second(SMMU_CBA2R(rs.context))) == rs.cba2r;
    for (i = 0; i < SMMU_SAVED; i++)
        restored &= wifi_rd(smmu_ctx(smmu_saved_offsets[i])) == rs.saved[i];
    printf("SWEET WIFI: SMMU SMR%u and context %u put back as found: %s\n", rs.match, rs.context,
           restored ? "yes" : "NO (read back differs)");
    rs.installed = false;
    rs.found = false;
}

/* The route that makes the radio's requests carry stream 0x1c01 (sweet_pcie_enable_dma, without bus mastering:
 * Windows' pci.sys and the radio's driver switch that on themselves). */
static bool wifi_route(void)
{
    /* Kona hashes the two BDF bytes (bus 1, devfn 0) using CRC-8/0x07.
     * SID table contains the low byte; PCIe0 supplies the wired 0x1c00 base. */
    static const uint8_t bdf[] = { 1, 0 };
    uint8_t hash = 0;
    uint32_t route, old, wanted = 0x01000100;
    unsigned i, b;
    for (i = 0; i < sizeof(bdf); i++) {
        hash ^= bdf[i];
        for (b = 0; b < 8; b++)
            hash = (uint8_t)((hash << 1) ^ ((hash & 0x80) ? 7 : 0));
    }
    route = PARF + 0x2000 + 4 * hash;
    old = rd(route);
    report("BDF-to-SID route address", route);
    report("BDF-to-SID previous route", old);
    if (old != 0 && old != wanted)
        return false;
    wr(route, wanted);
    if (rd(route) != wanted)
        return false;
    /* SM8250 defaults to BDF-to-SID bypass: explicitly enable translation. */
    report("BDF-to-SID configuration before", rd(PARF + 0x2c00));
    if (update(PARF + 0x2c00, 1, 0) || (rd(PARF + 0x2c00) & 1))
        return false;
    report("BDF-to-SID configuration after", rd(PARF + 0x2c00));
    return true;
}

/* ---------- the steps ---------- */
static void wifi_run(unsigned steps)
{
    bool ecam = false, smmu = false;
    printf("SWEET WIFI: steps 1..%u%s%s\n", steps, wifi_keep ? ", the link is kept afterwards" : "",
           wifi_deep ? ", deep ECAM read" : "");
    if (!wifi_map_regions())
        return;
    wifi_survey();
    if (steps < 2)
        return;
    printf("SWEET WIFI: step 2: rails, wlan-en, PCIe0 clocks, PHY, link, the QCA6390's IDs\n");
    if (sweet_pcie_enumerate() != 0) {
        printf("SWEET WIFI: step 2 FAILED: no link to the radio (the lines above say where)\n");
        return;
    }
    printf("SWEET WIFI: THE PCIe LINK IS UP AND THE QCA6390 ANSWERS (17cb:1101)\n");
    wifi_identity();
    smmu_survey();
    if (steps >= 3)
        ecam = wifi_ecam();
    if (steps >= 4) {
        printf("SWEET WIFI: step 4: an apps SMMU context for the radio's stream %#x with identity tables\n", RADIO_STREAM);
        smmu = smmu_install() && wifi_route();
        if (smmu)
            printf("SWEET WIFI: THE RADIO MAY USE PHYSICAL ADDRESSES: SMMU context and BDF-to-SID route are in place\n");
    }
    if (steps >= 3)
        printf("SWEET WIFI: summary: link up, ECAM %s%s\n", ecam ? "ready" : "NOT ready",
               steps >= 4 ? (smmu ? ", SMMU ready" : ", SMMU NOT ready") : "");
    if (wifi_keep) {
        printf("SWEET WIFI: Keep = 1: the link, ECAM%s stay as they are until the phone restarts\n",
               rs.installed ? " and the SMMU context" : "");
        return;
    }
    sweet_pcie_quiesce();
    printf("SWEET WIFI: PERST and wlan-en low again; the rails stay voted\n");
    smmu_restore();
    if (ecam_on) {
        /* the controller keeps its registers while its clocks run: leave nothing that blocks */
        wr(PARF + PARF_SYS_CTRL, rd(PARF + PARF_SYS_CTRL) & ~PARF_ECAM_BLOCKER_EN);
    }
}

static VOID ProbeThread(PVOID Context)
{
    unsigned steps = (unsigned)(ULONG_PTR)Context;
    wifi_run(steps);
    printf("SWEET WIFI: done: steps 1..%u\n", steps);
    if (!wifi_keep) {
        wifi_unmap_regions();
        if (rs.base) {
            MmUnmapIoSpace((PVOID)rs.base, SMMU_SIZE);
            rs.base = 0;
        }
        if (rs.tables) {
            MmFreeContiguousMemorySpecifyCache(rs.tables, 4096u * RADIO_TABLES, MmCached);
            rs.tables = NULL;
        }
    }
    if (rh_rsc)
        MmUnmapIoSpace((PVOID)rh_rsc, 0x100);
    if (rh_tcs)
        MmUnmapIoSpace((PVOID)rh_tcs, RH_TCS_SIZE);
    if (rh_db)
        MmUnmapIoSpace((PVOID)rh_db, RH_DB_SIZE);
    rh_rsc = rh_tcs = rh_db = 0;
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
    LogBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, LOG_SIZE, POOL_TAG);
    LogLen = 0;
    steps = ReadParam(L"Steps", 2);
    wifi_keep = ReadParam(L"Keep", 0);
    wifi_deep = ReadParam(L"Deep", 0);
    if (steps > 4)
        steps = 4;
    /* with the link kept, the SMMU tables and the route stay in use: the driver then cannot be unloaded */
    if (!wifi_keep)
        DriverObject->DriverUnload = Unload;
    if (steps == 0) {
        printf("SWEET WIFI: done: Steps = 0: nothing to do\n");
        return STATUS_SUCCESS;
    }
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, ProbeThread, (PVOID)(ULONG_PTR)steps);
    if (!NT_SUCCESS(st)) {
        printf("SWEET WIFI: done: cannot start the thread (%#x)\n", st);
        return STATUS_SUCCESS;
    }
    (void)ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&Thread, NULL);
    ZwClose(h);
    return STATUS_SUCCESS;
}
