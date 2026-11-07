/*
 * apollotouch - Novatek NT36675 SPI touchscreen for Windows on the Xiaomi Mi 10T / 10T Pro (apollo, SM8250)
 *
 * Port of ios-apollo's bsd/dev/sweet_touch.c (xnu patches 0082-0094, 0119; touch verified working on apollo
 * there since boot 10, 2026-10-24) to a KMDF driver that reports through the Virtual HID Framework (vhf.sys).
 * Windows on apollo runs without Qualcomm's PEP, so Qualcomm's SPI driver cannot start; like ios-apollo this
 * driver owns the hardware directly:
 *   - QUP0 SE4 (GENI, SPI protocol) at 0x990000, FIFO mode, polled: /soc/qupv3_se4_spi spi@990000 in apollo's
 *     live device tree;
 *   - its GCC clocks (vote register 0x52008, RCG 0x174d0), the SPI pins gpio8-11 (function qup4, north tile);
 *   - the chip's reset (gpio38, active low) and IRQ (gpio39, rising edge, latched and polled) in TLMM;
 *   - the NT36675 SPI protocol (Xiaomi apollo-q-oss nt36672c driver): bootloader reset, trim ID, host firmware
 *     download with HW CRC (the chip has no flash), then 65-byte reports from the event buffer at 0x22d00.
 * The firmware is novatek_nt36675_j3s_fw01.bin from Xiaomi's kernel source (MiCode Xiaomi_Kernel_OpenSource,
 * apollo-q-oss, firmware/*.ihex), installed to System32\drivers by install-apollotouch.ps1; it is not
 * redistributed here.
 *
 * Progress is written to HKLM\SYSTEM\CurrentControlSet\Services\apollotouch\Parameters (Status, TrimId, Firmware,
 * Reports) so the result can be read without a debugger; the same lines go to DbgPrintEx (IHVDRIVER).
 * Optional DWORD values there: PollMs (default 4), SpiDiv (default 4), StartDelayMs (default 0), TopRows (default 0:
 * set it to the UEFI's FB_SHIFT when Windows draws only below that many panel rows, so touch maps onto that area).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <hidclass.h>
#include <vhf.h>

#define TOUCH_SE_PA           0x00990000ULL
#define TOUCH_SE_SIZE         0x4000
#define TOUCH_RST_GPIO        38
#define TOUCH_IRQ_GPIO        39
#define TOUCH_GCC_PA          0x00100000ULL
#define TOUCH_GCC_SIZE        0x60000
#define TOUCH_GCC_VOTE        0x52008u
#define TOUCH_GCC_VOTE_MASK   ((0xfu << 6) | (1u << 14))
#define TOUCH_GCC_RCG         0x174d0u
#define TLMM_WEST             0x0f100000ULL
#define TLMM_SOUTH            0x0f500000ULL
#define TLMM_NORTH            0x0f900000ULL
#define TLMM_TILE_SIZE        0x40000
#define TOUCH_SPI_GPIO0       8
#define TOUCH_SPI_NGPIO       4
#define TOUCH_SPI_CFG         ((1u << 2) | (2u << 6))
#define TOUCH_MAX_FINGERS     10
#define TOUCH_ABS_X           1080
#define TOUCH_ABS_Y           2400

#define NVT_SPI_HDR           2
#define NVT_SPI_READ(a)       ((UCHAR)((a) & 0x7fu))
#define NVT_SPI_WRITE(a)      ((UCHAR)(((a) & 0x7fu) | 0x80u))
#define NVT_SWRST_N8_ADDR     0x3f0feu
#define NVT_SPI_RD_FAST_ADDR  0x3f310u
#define NVT_TRIM_ADDR         0x1f64eu
#define NVT_TRIM_LEN          6
#define NVT_EVENT_BUF_ADDR    0x22d00u
#define NVT_POINT_DATA_LEN    65
#define NVT_EVENT_MAP_HOST_CMD 0x50u
#define NVT_EVENT_MAP_RESET_COMPLETE 0x60u
#define NVT_RESET_STATE_INIT  0xa0u
#define NVT_RESET_STATE_MAX   0xafu
#define NVT_BOOT_RDY_ADDR     0x3f10du
#define NVT_BLD_CRC_EN_ADDR   0x3f30eu
#define NVT_ILM_DES_ADDR      0x3f128u
#define NVT_ILM_LENGTH_ADDR   0x3f118u
#define NVT_G_ILM_CHECKSUM_ADDR 0x3f100u
#define NVT_DLM_DES_ADDR      0x3f12cu
#define NVT_DLM_LENGTH_ADDR   0x3f130u
#define NVT_G_DLM_CHECKSUM_ADDR 0x3f104u
#define NVT_BLD_ILM_DLM_CRC_ADDR 0x3f133u
#define NVT_R_ILM_CHECKSUM_ADDR 0x3f120u
#define NVT_R_DLM_CHECKSUM_ADDR 0x3f124u
#define NVT_FW_CHUNK          4096u
#define NVT_MAX_PARTITIONS    32u
#define NVT_XFER_MAX          (NVT_SPI_HDR + NVT_POINT_DATA_LEN)
#define NVT_FW_MAX            (512u * 1024u)

#define SE_GENI_FORCE_DEFAULT 0x020
#define SE_GENI_OUTPUT_CTRL   0x024
#define SE_GENI_CGC_CTRL      0x028
#define SE_GENI_STATUS        0x040
#define SE_GENI_M_CLK_CFG     0x048
#define SE_GENI_FW_REVISION   0x068
#define SE_GENI_CLK_SEL       0x07c
#define SE_GENI_BYTE_GRAN     0x254
#define SE_GENI_DMA_MODE_EN   0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0        0x600
#define SE_GENI_M_CMD_CTRL    0x604
#define SE_GENI_M_IRQ_STATUS  0x610
#define SE_GENI_M_IRQ_EN      0x614
#define SE_GENI_M_IRQ_CLEAR   0x618
#define SE_GENI_S_IRQ_EN      0x644
#define SE_GENI_S_IRQ_CLEAR   0x648
#define SE_GENI_TX_FIFO       0x700
#define SE_GENI_RX_FIFO       0x780
#define SE_GENI_TX_FIFO_STATUS 0x800
#define SE_GENI_RX_FIFO_STATUS 0x804
#define SE_GENI_TX_WATERMARK  0x80c
#define SE_GENI_RX_WATERMARK  0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN       0xe18
#define SE_IRQ_EN             0xe1c
#define SE_HW_PARAM_1         0xe28
#define SE_DMA_GENERAL_CFG    0xe30
#define SE_SPI_CPHA           0x224
#define SE_SPI_LOOPBACK       0x22c
#define SE_SPI_CPOL           0x230
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c
#define SE_SPI_DEMUX_SEL      0x250
#define SE_SPI_TRANS_CFG      0x25c
#define SE_SPI_WORD_LEN       0x268
#define SE_SPI_TX_TRANS_LEN   0x26c
#define SE_SPI_RX_TRANS_LEN   0x270
#define SE_FIFO_WORDS(v)      ((v) & 0x1ffffffu)
#define SE_M_DONE             (1u << 0)
#define SE_M_ERRORS           0x3eu
#define SE_M_EVENTS           (SE_M_DONE | SE_M_ERRORS | (3u << 26) | (1u << 30))
#define SE_PACK0              0x0007f8feu
#define SE_PACK1              0x000ffefeu
#define GENI_SPI_FULL_DUPLEX  0x3u
#define GENI_OPCODE_SHIFT     27
#define TOUCH_TX_FIFO_SLACK   8
#define GENI_PROTO_SPI        1u

#define TLMM_GPIO_CFG(n)      ((ULONG)(n) * 0x1000u)
#define TLMM_GPIO_IN_OUT(n)   (TLMM_GPIO_CFG(n) + 4u)
#define TLMM_GPIO_INTR_CFG(n) (TLMM_GPIO_CFG(n) + 8u)
#define TLMM_GPIO_INTR_STATUS(n) (TLMM_GPIO_CFG(n) + 0xcu)
#define TLMM_INTR_RISING_LATCHED ((3u << 5) | (1u << 4) | (1u << 2) | (1u << 1))

#define FW_PATH L"\\SystemRoot\\System32\\drivers\\novatek_nt36675_j3s_fw01.bin"
#define TAG 'tcpA'

/* ---------- HID: a 10-finger touchscreen (parallel mode), report 1; feature 2 = contact count maximum ---------- */
#define RID_TOUCH 1
#define RID_MAXCOUNT 2
#define FINGER_COLLECTION \
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,              /* Finger, logical */ \
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, /* Tip switch */ \
    0x09, 0x47, 0x81, 0x02,                          /* Confidence */ \
    0x75, 0x06, 0x81, 0x03,                          /* pad */ \
    0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x81, 0x02,  /* Contact identifier */ \
    0x05, 0x01, 0x65, 0x11, 0x55, 0x0E, 0x75, 0x10, \
    0x09, 0x30, 0x26, 0x37, 0x04, 0x46, 0xB2, 0x02, 0x81, 0x02,   /* X 0..1079, 69.0 mm */ \
    0x09, 0x31, 0x26, 0x5F, 0x09, 0x46, 0xFE, 0x05, 0x81, 0x02,   /* Y 0..2399, 153.4 mm */ \
    0x65, 0x00, 0x55, 0x00, 0x45, 0x00, 0xC0

static const UCHAR ReportDescriptor[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,              /* Digitizer: touch screen, application */
    0x85, RID_TOUCH,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    0x05, 0x0D, 0x09, 0x54, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, /* Contact count */
    0x85, RID_MAXCOUNT, 0x09, 0x55, 0x25, 0x0A, 0xB1, 0x02,                              /* Contact count max */
    0xC0
};

#pragma pack(push, 1)
typedef struct {
    UCHAR Flags;        /* bit 0 tip, bit 1 confidence */
    UCHAR Id;
    USHORT X, Y;
} HID_FINGER;
typedef struct {
    UCHAR ReportId;
    HID_FINGER Finger[TOUCH_MAX_FINGERS];
    UCHAR Count;
} HID_TOUCH_REPORT;
#pragma pack(pop)

typedef struct {
    ULONG Sram, Size, Bin, Crc;
} NVT_PART;

typedef struct {
    WDFDEVICE Device;
    VHFHANDLE Vhf;
    PKTHREAD Thread;
    KEVENT Stop;
    PUCHAR Se, Gcc, Tlmm, North;
    PUCHAR TileVa[3];
    ULONG Errors, Active, PollMs, SpiDiv, StartDelayMs, Reports, TopRows;
    LONG X[TOUCH_MAX_FINGERS], Y[TOUCH_MAX_FINGERS];
    PUCHAR Fw;
    ULONG FwSize;
    NVT_PART Parts[NVT_MAX_PARTITIONS];
    ULONG NParts;
    UCHAR Chunk[1 + NVT_FW_CHUNK];
} TOUCH_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TOUCH_CTX, GetCtx)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT EvtIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP EvtIoCleanup;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtCleanup;
static EVT_VHF_ASYNC_OPERATION EvtGetFeature;

/* ---------- logging: DbgPrintEx + a value under Parameters ---------- */
static VOID Note(PCWSTR Name, PCSTR Fmt, ...)
{
    CHAR a[160];
    WCHAR w[160];
    va_list args;
    ULONG i;
    UNICODE_STRING key, val;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;

    va_start(args, Fmt);
    (void)RtlStringCbVPrintfA(a, sizeof(a), Fmt, args);
    va_end(args);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "apollotouch: %s\n", a);
    for (i = 0; a[i] && i < 159; i++)
        w[i] = (WCHAR)(UCHAR)a[i];
    w[i] = 0;
    RtlInitUnicodeString(&key, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\apollotouch\\Parameters");
    InitializeObjectAttributes(&oa, &key, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&val, Name);
        (void)ZwSetValueKey(h, &val, 0, REG_SZ, w, (i + 1) * sizeof(WCHAR));
        ZwClose(h);
    }
}

static ULONG ReadParam(PCWSTR Name, ULONG Default)
{
    UNICODE_STRING key, val;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len, v = Default;

    RtlInitUnicodeString(&key, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\apollotouch\\Parameters");
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

/* ---------- MMIO and timing ---------- */
static __forceinline ULONG Rd(PUCHAR Base, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static __forceinline VOID Wr(PUCHAR Base, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
    __dsb(_ARM64_BARRIER_SY);
}

/* Sleeps, returning TRUE if the driver is stopping. */
static BOOLEAN WaitMs(TOUCH_CTX *c, ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * (LONGLONG)Ms;
    return KeWaitForSingleObject(&c->Stop, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

static BOOLEAN WaitClear(PUCHAR Base, ULONG Off, ULONG Mask)
{
    ULONG i;
    for (i = 0; i < 5000; i++) {
        if (!(Rd(Base, Off) & Mask))
            return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static PUCHAR Map(ULONGLONG Pa, SIZE_T Size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (LONGLONG)Pa;
    return (PUCHAR)MmMapIoSpaceEx(p, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

/* ---------- TLMM ---------- */
static VOID GpioOut(TOUCH_CTX *c, ULONG Pin, BOOLEAN High)
{
    ULONG io = Rd(c->Tlmm, TLMM_GPIO_IN_OUT(Pin));
    ULONG cfg;
    Wr(c->Tlmm, TLMM_GPIO_IN_OUT(Pin), High ? (io | 2u) : (io & ~2u));
    cfg = Rd(c->Tlmm, TLMM_GPIO_CFG(Pin)) & ~0x3ffu;
    Wr(c->Tlmm, TLMM_GPIO_CFG(Pin), cfg | (3u << 6) | 0x200u);
}

/* ---------- GCC: QUP0 SE4 clocks (returns the GENI CLK_SEL level, or -1) ---------- */
static LONG Clocks(TOUCH_CTX *c)
{
    static const ULONG branches[] = { 0x17004, 0x17008, 0x23000, 0x23008, 0x174cc };
    LONG level = -1;
    ULONG dfs, cfg, l, i;

    Wr(c->Gcc, TOUCH_GCC_VOTE, Rd(c->Gcc, TOUCH_GCC_VOTE) | TOUCH_GCC_VOTE_MASK);
    dfs = Rd(c->Gcc, TOUCH_GCC_RCG + 0x14u);
    cfg = Rd(c->Gcc, TOUCH_GCC_RCG + 4u);
    if (dfs & 1u) {
        for (l = 0; l < 8; l++) {
            ULONG lv = Rd(c->Gcc, TOUCH_GCC_RCG + 0x1cu + 4u * l);
            if (level < 0 && (lv & 0x371fu) == 1u)
                level = (LONG)l;
        }
        if (level < 0) {
            Note(L"Status", "SE4 clock: no XO/1 DFS level (dfs %#lx); stopped", dfs);
            return -1;
        }
    } else {
        cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
        Wr(c->Gcc, TOUCH_GCC_RCG + 4u, cfg);
        Wr(c->Gcc, TOUCH_GCC_RCG, Rd(c->Gcc, TOUCH_GCC_RCG) | 1u);
        if (!WaitClear(c->Gcc, TOUCH_GCC_RCG, 1u)) {
            Note(L"Status", "SE4 clock: RCG update timeout; stopped");
            return -1;
        }
        level = 0;
    }
    for (i = 0; i < RTL_NUMBER_OF(branches); i++) {
        ULONG cbcr = Rd(c->Gcc, branches[i]);
        if (cbcr & 2u)
            continue;               /* hardware clock gating: CLK_OFF reads 1 while idle */
        if (!WaitClear(c->Gcc, branches[i], 1u << 31)) {
            Note(L"Status", "SE4 clock: GCC branch %#lx still off (%#lx); stopped", branches[i], Rd(c->Gcc, branches[i]));
            return -1;
        }
    }
    return level;
}

/* ---------- GENI SPI ---------- */
static BOOLEAN SpiInit(TOUCH_CTX *c, ULONG ClkSel)
{
    PUCHAR se = c->Se;
    ULONG hw1 = Rd(se, SE_HW_PARAM_1);
    if (Rd(se, SE_GENI_STATUS) & 1u) {
        Note(L"Status", "SE4 busy (status %#lx); not touching it", Rd(se, SE_GENI_STATUS));
        return FALSE;
    }
    Wr(se, SE_GSI_EVENT_EN, 0);
    Wr(se, SE_IRQ_EN, 0);
    Wr(se, SE_GENI_S_IRQ_EN, 0);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    Wr(se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
    Wr(se, SE_GENI_DMA_MODE_EN, 0);
    Wr(se, SE_GENI_CGC_CTRL, Rd(se, SE_GENI_CGC_CTRL) | 0x7fu);
    Wr(se, SE_DMA_GENERAL_CFG, Rd(se, SE_DMA_GENERAL_CFG) | 0x0fu);
    Wr(se, SE_GENI_OUTPUT_CTRL, 0x7f);
    Wr(se, SE_GENI_FORCE_DEFAULT, 1);
    Wr(se, SE_GENI_CLK_SEL, ClkSel);
    Wr(se, SE_GENI_M_CLK_CFG, ((c->SpiDiv & 0xffu) << 4) | 1u);
    Wr(se, SE_SPI_LOOPBACK, 0);
    Wr(se, SE_SPI_CPHA, 0);
    Wr(se, SE_SPI_CPOL, 0);
    Wr(se, SE_SPI_DEMUX_OUTPUT_INV, 0);
    Wr(se, SE_SPI_DEMUX_SEL, 0);
    Wr(se, SE_SPI_TRANS_CFG, 0);
    Wr(se, SE_SPI_WORD_LEN, 8 - 4);
    Wr(se, SE_GENI_BYTE_GRAN, 0);
    Wr(se, SE_GENI_TX_PACKING_CFG0, SE_PACK0);
    Wr(se, SE_GENI_TX_PACKING_CFG1, SE_PACK1);
    Wr(se, SE_GENI_RX_PACKING_CFG0, SE_PACK0);
    Wr(se, SE_GENI_RX_PACKING_CFG1, SE_PACK1);
    Wr(se, SE_GENI_TX_WATERMARK, 0);
    Wr(se, SE_GENI_RX_WATERMARK, 0);
    Wr(se, SE_GENI_RX_RFR_WATERMARK, ((hw1 >> 16) & 0x3fu) - 1u);
    Wr(se, SE_GENI_M_IRQ_EN, SE_M_EVENTS);
    return TRUE;
}

/* Full duplex, len bytes; Rx may be NULL. 0 on success. */
static int SpiXfer(TOUCH_CTX *c, const UCHAR *Tx, UCHAR *Rx, SIZE_T Len)
{
    PUCHAR se = c->Se;
    SIZE_T txi = 0, rxi = 0;
    BOOLEAN done = FALSE;
    ULONG i, idle;

    if (Len == 0)
        return -1;
    if (Rd(se, SE_GENI_STATUS) & 1u) {
        Wr(se, SE_GENI_M_CMD_CTRL, 2);
        (void)WaitClear(se, SE_GENI_STATUS, 1u);
    }
    for (i = 0; i < 64 && SE_FIFO_WORDS(Rd(se, SE_GENI_RX_FIFO_STATUS)); i++)
        (void)Rd(se, SE_GENI_RX_FIFO);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    Wr(se, SE_SPI_TX_TRANS_LEN, (ULONG)Len);
    Wr(se, SE_SPI_RX_TRANS_LEN, (ULONG)Len);
    Wr(se, SE_GENI_M_CMD0, GENI_SPI_FULL_DUPLEX << GENI_OPCODE_SHIFT);

    for (idle = 0; idle < 10000; idle++) {
        SIZE_T before = txi + rxi;
        ULONG n, irq;
        while (txi < Len && SE_FIFO_WORDS(Rd(se, SE_GENI_TX_FIFO_STATUS)) < TOUCH_TX_FIFO_SLACK) {
            ULONG word = 0, b;
            for (b = 0; b < 4 && txi < Len; b++, txi++)
                word |= (ULONG)Tx[txi] << (8 * b);
            Wr(se, SE_GENI_TX_FIFO, word);
        }
        for (n = SE_FIFO_WORDS(Rd(se, SE_GENI_RX_FIFO_STATUS)); n && rxi < Len; n--) {
            ULONG word = Rd(se, SE_GENI_RX_FIFO), b;
            for (b = 0; b < 4 && rxi < Len; b++, rxi++)
                if (Rx)
                    Rx[rxi] = (UCHAR)(word >> (8 * b));
        }
        irq = Rd(se, SE_GENI_M_IRQ_STATUS);
        if (irq & SE_M_ERRORS) {
            Wr(se, SE_GENI_M_IRQ_CLEAR, irq);
            c->Errors++;
            return -1;
        }
        if (irq & SE_M_DONE) {
            Wr(se, SE_GENI_M_IRQ_CLEAR, irq);
            done = TRUE;
        }
        if (done && rxi >= Len)
            break;
        if (txi + rxi != before)
            idle = 0;
        else
            KeStallExecutionProcessor(1);
    }
    if (!done || rxi < Len) {
        Wr(se, SE_GENI_M_CMD_CTRL, 2);
        c->Errors++;
        return -1;
    }
    c->Errors = 0;
    return 0;
}

/* ---------- NT36xxx framing ---------- */
static int NvtWrite(TOUCH_CTX *c, const UCHAR *Buf, SIZE_T Len)
{
    UCHAR rx[8];
    if (Len > sizeof(rx))
        return -1;
    return SpiXfer(c, Buf, rx, Len);
}

static int NvtSetPage(TOUCH_CTX *c, ULONG Addr)
{
    UCHAR b[3] = { 0xff, (UCHAR)(Addr >> 15), (UCHAR)(Addr >> 7) };
    return NvtWrite(c, b, sizeof(b));
}

static int NvtWriteAddr(TOUCH_CTX *c, ULONG Addr, UCHAR Val)
{
    UCHAR b[2];
    b[0] = NVT_SPI_WRITE(Addr);
    b[1] = Val;
    if (NvtSetPage(c, Addr))
        return -1;
    return NvtWrite(c, b, sizeof(b));
}

static int NvtRead(TOUCH_CTX *c, ULONG Addr, UCHAR *Out, SIZE_T Len)
{
    UCHAR tx[NVT_XFER_MAX] = { 0 };
    UCHAR rx[NVT_XFER_MAX];
    SIZE_T i;
    if (Len > NVT_XFER_MAX - NVT_SPI_HDR || NvtSetPage(c, Addr))
        return -1;
    tx[0] = NVT_SPI_READ(Addr);
    if (SpiXfer(c, tx, rx, NVT_SPI_HDR + Len))
        return -1;
    for (i = 0; i < Len; i++)
        Out[i] = rx[NVT_SPI_HDR + i];
    return 0;
}

static ULONG Word(const UCHAR *p)
{
    return p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static int NvtWriteLe(TOUCH_CTX *c, ULONG Addr, ULONG Val, ULONG N)
{
    UCHAR b[5];
    b[0] = NVT_SPI_WRITE(Addr);
    b[1] = (UCHAR)Val;
    b[2] = (UCHAR)(Val >> 8);
    b[3] = (UCHAR)(Val >> 16);
    b[4] = (UCHAR)(Val >> 24);
    return NvtWrite(c, b, 1 + N);
}

/* ---------- firmware ---------- */
static BOOLEAN LoadFirmware(TOUCH_CTX *c)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION si;
    HANDLE h;
    NTSTATUS st;

    RtlInitUnicodeString(&path, FW_PATH);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                      FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) {
        Note(L"Firmware", "cannot open System32\\drivers\\novatek_nt36675_j3s_fw01.bin (%#lx)", st);
        return FALSE;
    }
    st = ZwQueryInformationFile(h, &io, &si, sizeof(si), FileStandardInformation);
    if (NT_SUCCESS(st) && (si.EndOfFile.QuadPart < 0x8000 || si.EndOfFile.QuadPart > NVT_FW_MAX))
        st = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(st)) {
        c->FwSize = (ULONG)si.EndOfFile.QuadPart;
        c->Fw = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, c->FwSize, TAG);
        st = c->Fw ? ZwReadFile(h, NULL, NULL, NULL, &io, c->Fw, c->FwSize, NULL, NULL) : STATUS_NO_MEMORY;
    }
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        Note(L"Firmware", "cannot read the firmware file (%#lx)", st);
        return FALSE;
    }
    return TRUE;
}

static SIZE_T FwNeedSize(const UCHAR *Fw, SIZE_T Size)
{
    SIZE_T i;
    for (i = Size / 4096; i > 0; i--) {
        const UCHAR *e = Fw + i * 4096 - 3;
        if ((e[0] == 'N' && e[1] == 'V' && e[2] == 'T') || (e[0] == 'M' && e[1] == 'O' && e[2] == 'D'))
            return i * 4096;
    }
    return 0;
}

static BOOLEAN ParseHeader(TOUCH_CTX *c)
{
    const UCHAR *fw = c->Fw;
    ULONG end = Word(fw), info = 0, ovly = (fw[0x28] & 0x10) ? (fw[0x28] & 0x0fu) : 0, pos, i;

    for (pos = 0x30; pos < end; pos += 0x10)
        info++;
    c->NParts = 2 + ovly + info;
    if (c->NParts > NVT_MAX_PARTITIONS)
        return FALSE;
    for (i = 0; i < c->NParts; i++) {
        NVT_PART *p = &c->Parts[i];
        if (i < 2) {
            p->Bin = Word(fw + 12 * i);
            p->Sram = Word(fw + 4 + 12 * i);
            p->Size = Word(fw + 8 + 12 * i);
            p->Crc = Word(fw + 0x18 + 4 * i);
        } else {
            pos = i < 2 + info ? 0x30 + 0x10 * (i - 2) : c->Parts[1].Bin + 0x10 * (i - 2 - info);
            if (pos + 16 > c->FwSize)
                return FALSE;
            p->Sram = Word(fw + pos);
            p->Size = Word(fw + pos + 4);
            p->Bin = Word(fw + pos + 8);
            p->Crc = Word(fw + pos + 12);
        }
        if ((ULONGLONG)p->Bin + p->Size > c->FwSize)
            return FALSE;
    }
    return TRUE;
}

static int WriteSram(TOUCH_CTX *c, ULONG Sram, ULONG Size, ULONG Bin)
{
    while (Size) {
        ULONG len = Size < NVT_FW_CHUNK ? Size : NVT_FW_CHUNK;
        if (NvtSetPage(c, Sram))
            return -1;
        c->Chunk[0] = NVT_SPI_WRITE(Sram);
        RtlCopyMemory(c->Chunk + 1, c->Fw + Bin, len);
        if (SpiXfer(c, c->Chunk, NULL, 1 + len))
            return -1;
        Sram += len;
        Bin += len;
        Size -= len;
    }
    return 0;
}

static VOID SetCrcBank(TOUCH_CTX *c, ULONG Des, ULONG Sram, ULONG LenAddr, ULONG Size, ULONG GcrcAddr, ULONG Crc)
{
    (void)NvtSetPage(c, Des);
    (void)NvtWriteLe(c, Des, Sram & 0xffffffu, 3);
    (void)NvtWriteLe(c, LenAddr, Size & 0x1ffffu, 3);
    (void)NvtWriteLe(c, GcrcAddr, Crc, 4);
}

static BOOLEAN Download(TOUCH_CTX *c)
{
    SIZE_T need;
    UCHAR ver, bar;
    ULONG attempt, i, t;

    if (!LoadFirmware(c))
        return FALSE;
    need = FwNeedSize(c->Fw, c->FwSize);
    if (!need) {
        Note(L"Firmware", "file has no NVT/MOD end flag (wrong file?)");
        return FALSE;
    }
    ver = c->Fw[need - 4096];
    bar = c->Fw[need - 4096 + 1];
    if ((UCHAR)(ver + bar) != 0xff || !ParseHeader(c)) {
        Note(L"Firmware", "bad header (version %#x bar %#x)", ver, bar);
        return FALSE;
    }
    for (attempt = 1; attempt <= 3; attempt++) {
        BOOLEAN ok = TRUE;
        UCHAR en = 0, state[5] = { 0 };
        (void)NvtWriteAddr(c, NVT_SWRST_N8_ADDR, 0x69);
        if (WaitMs(c, 5))
            return FALSE;
        (void)NvtWriteAddr(c, NVT_SPI_RD_FAST_ADDR, 0x00);
        for (i = 0; i < c->NParts && ok; i++) {
            ULONG w = c->Parts[i].Size + 1;
            if (c->Parts[i].Bin + w > c->FwSize)
                w = c->FwSize - c->Parts[i].Bin;
            if (c->Parts[i].Size)
                ok = (BOOLEAN)!WriteSram(c, c->Parts[i].Sram, w, c->Parts[i].Bin);
        }
        if (!ok)
            continue;
        SetCrcBank(c, NVT_ILM_DES_ADDR, c->Parts[0].Sram, NVT_ILM_LENGTH_ADDR, c->Parts[0].Size,
                   NVT_G_ILM_CHECKSUM_ADDR, c->Parts[0].Crc);
        SetCrcBank(c, NVT_DLM_DES_ADDR, c->Parts[1].Sram, NVT_DLM_LENGTH_ADDR, c->Parts[1].Size,
                   NVT_G_DLM_CHECKSUM_ADDR, c->Parts[1].Crc);
        (void)NvtRead(c, NVT_BLD_CRC_EN_ADDR, &en, 1);
        (void)NvtWriteAddr(c, NVT_BLD_CRC_EN_ADDR, en | 0x80u);
        (void)NvtWriteAddr(c, NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_RESET_COMPLETE, 0x00);
        (void)NvtWriteAddr(c, NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_HOST_CMD, 0xae);
        (void)NvtWriteAddr(c, NVT_BOOT_RDY_ADDR, 1);
        if (WaitMs(c, 5))
            return FALSE;
        for (t = 0; t <= 20; t++) {
            (void)NvtRead(c, NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_RESET_COMPLETE, state, sizeof(state));
            if (state[0] >= NVT_RESET_STATE_INIT && state[0] <= NVT_RESET_STATE_MAX) {
                Note(L"Firmware", "running (version %#x, %lu partitions, reset state %#x, attempt %lu)",
                     ver, c->NParts, state[0], attempt);
                return TRUE;
            }
            if (WaitMs(c, 10))
                return FALSE;
        }
        Note(L"Firmware", "not running after attempt %lu (state %02x %02x)", attempt, state[0], state[1]);
    }
    return FALSE;
}

/* ---------- reports ---------- */
static VOID Submit(TOUCH_CTX *c, ULONG Now)
{
    HID_TOUCH_REPORT r;
    HID_XFER_PACKET p;
    ULONG id, n = 0;

    RtlZeroMemory(&r, sizeof(r));
    r.ReportId = RID_TOUCH;
    /* every finger that is down now, and every finger that was down and lifted (tip 0, last position) */
    for (id = 0; id < TOUCH_MAX_FINGERS && n < TOUCH_MAX_FINGERS; id++) {
        BOOLEAN down = (BOOLEAN)((Now >> id) & 1u), was = (BOOLEAN)((c->Active >> id) & 1u);
        if (!down && !was)
            continue;
        r.Finger[n].Flags = (UCHAR)((down ? 1 : 0) | 2);
        r.Finger[n].Id = (UCHAR)id;
        r.Finger[n].X = (USHORT)c->X[id];
        r.Finger[n].Y = (USHORT)c->Y[id];
        n++;
    }
    if (n == 0)
        return;
    r.Count = (UCHAR)n;
    p.reportBuffer = (PUCHAR)&r;
    p.reportBufferLen = sizeof(r);
    p.reportId = RID_TOUCH;
    (void)VhfReadReportSubmit(c->Vhf, &p);
}

/* NT36xxx event buffer, 6 bytes a finger: [0] id << 3 | status (1 down, 2 move), [1] x 11:4, [2] y 11:4,
 * [3] x 3:0 << 4 | y 3:0 (Xiaomi nvt_ts_work_func, as ios-apollo's touch_parse_report). */
static VOID Parse(TOUCH_CTX *c, const UCHAR *R)
{
    BOOLEAN any = FALSE, changed = FALSE;
    ULONG i, now = 0;

    for (i = 0; i < NVT_POINT_DATA_LEN && !any; i++)
        any = (BOOLEAN)(R[i] != 0 && R[i] != 0xff);
    if (!any && !c->Active)
        return;
    for (i = 0; i < TOUCH_MAX_FINGERS && 6u * i + 3u < NVT_POINT_DATA_LEN; i++) {
        const UCHAR *p = R + 6u * i;
        ULONG id = p[0] >> 3, status = p[0] & 7u;
        LONG x, y;
        if (id == 0 || id > TOUCH_MAX_FINGERS || (status != 1 && status != 2))
            continue;
        id -= 1;
        x = (p[1] << 4) | (p[3] >> 4);
        y = (p[2] << 4) | (p[3] & 0x0f);
        if (x >= TOUCH_ABS_X) x = TOUCH_ABS_X - 1;
        if (y >= TOUCH_ABS_Y) y = TOUCH_ABS_Y - 1;
        if (c->TopRows) {           /* framebuffer starts TopRows lower: map panel rows TopRows..2399 onto 0..2399 */
            y = y < (LONG)c->TopRows ? 0 : (y - (LONG)c->TopRows) * TOUCH_ABS_Y / (TOUCH_ABS_Y - (LONG)c->TopRows);
            if (y >= TOUCH_ABS_Y) y = TOUCH_ABS_Y - 1;
        }
        now |= 1u << id;
        if (!(c->Active & (1u << id)) || c->X[id] != x || c->Y[id] != y) {
            c->X[id] = x;
            c->Y[id] = y;
            changed = TRUE;
        }
    }
    if (changed || now != c->Active) {
        Submit(c, now);
        c->Reports++;
        if (c->Reports == 1 || (c->Reports % 500) == 0)
            Note(L"Reports", "%lu touch reports sent", c->Reports);
    }
    c->Active = now;
}

/* ---------- worker ---------- */
static VOID Worker(PVOID Context)
{
    TOUCH_CTX *c = (TOUCH_CTX *)Context;
    static const ULONGLONG tiles[3] = { TLMM_WEST, TLMM_SOUTH, TLMM_NORTH };
    UCHAR id[NVT_TRIM_LEN] = { 0 }, report[NVT_POINT_DATA_LEN];
    ULONG t, i, fw, idle = 0, edges = 0, icfg;
    LONG clkSel;

    ExSetTimerResolution(10000, TRUE);          /* 1 ms ticks for the poll loop */
    if (c->StartDelayMs && WaitMs(c, c->StartDelayMs))
        goto out;
    Note(L"Status", "starting");

    c->Se = Map(TOUCH_SE_PA, TOUCH_SE_SIZE);
    c->Gcc = Map(TOUCH_GCC_PA, TOUCH_GCC_SIZE);
    for (t = 0; t < 3; t++)
        c->TileVa[t] = Map(tiles[t], TLMM_TILE_SIZE);
    if (!c->Se || !c->Gcc || !c->TileVa[2]) {
        Note(L"Status", "cannot map the SPI/GCC/TLMM registers; stopped");
        goto out;
    }
    c->North = c->TileVa[2];
    for (t = 0; t < 3 && !c->Tlmm; t++) {
        PUCHAR va = c->TileVa[t];
        if (va && (Rd(va, TLMM_GPIO_CFG(TOUCH_RST_GPIO)) | Rd(va, TLMM_GPIO_IN_OUT(TOUCH_RST_GPIO)) |
                   Rd(va, TLMM_GPIO_CFG(TOUCH_IRQ_GPIO)) | Rd(va, TLMM_GPIO_IN_OUT(TOUCH_IRQ_GPIO))))
            c->Tlmm = va;
    }
    if (!c->Tlmm) {
        Note(L"Status", "gpio38/39 not found in any TLMM tile; stopped");
        goto out;
    }
    fw = Rd(c->Se, SE_GENI_FW_REVISION);
    if (((fw >> 8) & 0xffu) != GENI_PROTO_SPI) {
        Note(L"Status", "SE4 at 0x990000 runs protocol %lu, not SPI (1); stopped", (fw >> 8) & 0xffu);
        goto out;
    }

    GpioOut(c, TOUCH_RST_GPIO, FALSE);          /* reset pulse, active low */
    if (WaitMs(c, 10))
        goto out;
    GpioOut(c, TOUCH_RST_GPIO, TRUE);
    if (WaitMs(c, 20))
        goto out;

    clkSel = Clocks(c);
    if (clkSel < 0)
        goto out;
    for (i = 0; i < TOUCH_SPI_NGPIO; i++) {     /* gpio8-11 -> qup4 */
        ULONG pin = TOUCH_SPI_GPIO0 + i;
        Wr(c->North, TLMM_GPIO_CFG(pin), (Rd(c->North, TLMM_GPIO_CFG(pin)) & ~0x3ffu) | TOUCH_SPI_CFG);
    }
    if (!SpiInit(c, (ULONG)clkSel))
        goto out;

    (void)NvtWriteAddr(c, NVT_SWRST_N8_ADDR, 0x69);
    if (WaitMs(c, 5))
        goto out;
    (void)NvtWriteAddr(c, NVT_SPI_RD_FAST_ADDR, 0x00);
    (void)NvtRead(c, NVT_TRIM_ADDR, id, sizeof(id));
    Note(L"TrimId", "%02x %02x %02x %02x %02x %02x%s", id[0], id[1], id[2], id[3], id[4], id[5],
         (id[3] == 0x72 && id[4] == 0x66 && id[5] == 0x03) ? " = NT36675" : " (not NT36675's 72 66 03)");

    if (!Download(c)) {
        Note(L"Status", "touch firmware not running; stopped (see Firmware)");
        goto out;
    }

    icfg = Rd(c->Tlmm, TLMM_GPIO_INTR_CFG(TOUCH_IRQ_GPIO));
    Wr(c->Tlmm, TLMM_GPIO_INTR_CFG(TOUCH_IRQ_GPIO), (icfg & ~0xffu) | TLMM_INTR_RISING_LATCHED);
    Wr(c->Tlmm, TLMM_GPIO_INTR_STATUS(TOUCH_IRQ_GPIO), 0);
    Note(L"Status", "running: reports polled every %lu ms", c->PollMs);

    for (;;) {
        BOOLEAN edge = (BOOLEAN)((Rd(c->Tlmm, TLMM_GPIO_INTR_STATUS(TOUCH_IRQ_GPIO)) & 1u) != 0);
        BOOLEAN level = (BOOLEAN)((Rd(c->Tlmm, TLMM_GPIO_IN_OUT(TOUCH_IRQ_GPIO)) & 1u) != 0);
        if (edge) {
            Wr(c->Tlmm, TLMM_GPIO_INTR_STATUS(TOUCH_IRQ_GPIO), 0);
            edges++;
        }
        if (edge || level || edges == 0 || idle >= 12) {
            if (NvtRead(c, NVT_EVENT_BUF_ADDR, report, sizeof(report)) == 0) {
                Parse(c, report);
                idle = 0;
            } else {
                idle++;
            }
        } else {
            idle++;
        }
        if (WaitMs(c, c->Errors > 10 ? 1000 : c->PollMs))
            break;
    }
out:
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------- VHF feature report: contact count maximum ---------- */
static VOID EvtGetFeature(PVOID VhfClientContext, VHFOPERATIONHANDLE Op, PVOID Unused, PHID_XFER_PACKET Packet)
{
    UNREFERENCED_PARAMETER(VhfClientContext);
    UNREFERENCED_PARAMETER(Unused);
    if (Packet->reportId == RID_MAXCOUNT && Packet->reportBufferLen >= 2) {
        Packet->reportBuffer[0] = RID_MAXCOUNT;
        Packet->reportBuffer[1] = TOUCH_MAX_FINGERS;
        (void)VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else {
        (void)VhfAsyncOperationComplete(Op, STATUS_INVALID_PARAMETER);
    }
}

/* ---------- WDF ---------- */
static NTSTATUS EvtIoInit(WDFDEVICE Device)
{
    TOUCH_CTX *c = GetCtx(Device);
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;

    c->PollMs = ReadParam(L"PollMs", 4);
    if (c->PollMs == 0 || c->PollMs > 100)
        c->PollMs = 4;
    c->SpiDiv = ReadParam(L"SpiDiv", 4);
    if (c->SpiDiv == 0 || c->SpiDiv > 255)
        c->SpiDiv = 4;
    c->StartDelayMs = ReadParam(L"StartDelayMs", 0);
    c->TopRows = ReadParam(L"TopRows", 0);
    if (c->TopRows >= TOUCH_ABS_Y)
        c->TopRows = 0;
    KeInitializeEvent(&c->Stop, NotificationEvent, FALSE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, Worker, c);
    if (!NT_SUCCESS(st))
        return st;
    st = ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&c->Thread, NULL);
    ZwClose(h);
    return st;
}

static VOID EvtIoCleanup(WDFDEVICE Device)
{
    TOUCH_CTX *c = GetCtx(Device);
    ULONG t;
    if (c->Thread) {
        KeSetEvent(&c->Stop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(c->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(c->Thread);
        c->Thread = NULL;
    }
    if (c->Se) MmUnmapIoSpace(c->Se, TOUCH_SE_SIZE);
    if (c->Gcc) MmUnmapIoSpace(c->Gcc, TOUCH_GCC_SIZE);
    for (t = 0; t < 3; t++)
        if (c->TileVa[t]) MmUnmapIoSpace(c->TileVa[t], TLMM_TILE_SIZE);
    c->Se = c->Gcc = c->Tlmm = c->North = NULL;
    RtlZeroMemory(c->TileVa, sizeof(c->TileVa));
    if (c->Fw) {
        ExFreePoolWithTag(c->Fw, TAG);
        c->Fw = NULL;
    }
}

static VOID EvtCleanup(WDFOBJECT Object)
{
    TOUCH_CTX *c = GetCtx((WDFDEVICE)Object);
    if (c->Vhf) {
        VhfDelete(c->Vhf, TRUE);
        c->Vhf = NULL;
    }
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    VHF_CONFIG cfg;
    TOUCH_CTX *c;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceSelfManagedIoInit = EvtIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = EvtIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, TOUCH_CTX);
    attr.EvtCleanupCallback = EvtCleanup;
    st = WdfDeviceCreate(&Init, &attr, &dev);
    if (!NT_SUCCESS(st))
        return st;
    c = GetCtx(dev);
    c->Device = dev;

    VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(dev), sizeof(ReportDescriptor), (PUCHAR)ReportDescriptor);
    cfg.VhfClientContext = c;
    cfg.EvtVhfAsyncOperationGetFeature = EvtGetFeature;
    cfg.VendorID = 0x2717;      /* Xiaomi */
    cfg.ProductID = 0x3675;
    cfg.VersionNumber = 1;
    st = VhfCreate(&cfg, &c->Vhf);
    if (!NT_SUCCESS(st))
        return st;
    return VhfStart(c->Vhf);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
