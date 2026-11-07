/*
 * apollobt - QCA6390 Bluetooth for Windows on the Xiaomi Mi 10T / 10T Pro (apollo, SM8250)
 *
 * Port of ios-apollo's bsd/dev/sweet_bt.c, sweet_bt.h and sweet_rpmh.c (xnu 0136-0145; Bluetooth found devices,
 * paired and played A2DP on apollo there), which came from the Xiaomi Mi 10 port of
 * github.com/JopangJovan/hackios-xnu-kernel-umi (MIT). Windows on apollo runs without Qualcomm's PEP, so
 * Qualcomm's UART and Bluetooth drivers cannot start; this driver owns the hardware directly:
 *   - the chip's supplies, voted through the application processor's RPMh sequencer (cmd-db at 0x80860000):
 *     smpa6 950 mV, smpa5 1.9 V, smpc8 1.35 V, ldoa16 3.025 V;
 *   - BT_EN (gpio21) and SW_CTRL (gpio124, read only);
 *   - QUP0 serial engine 6 as a 4-wire UART at 0x998000, gpio16-19, its GCC clocks; FIFOs polled;
 *   - the chip: HCI reset and version from its ROM at 115200 baud, the UART moved to 921600 (Baud), the
 *     phone's own firmware (htbtfw20.tlv, htnv20.bin from its bluetooth partition, in System32\drivers),
 *     the address (Address, else the NVM's), then H4 traffic with in-band sleep (IBS) handled here.
 * Windows' Bluetooth stack talks to it through Microsoft's extensible transport interface: this driver is a
 * bus driver for \_SB.BTH0 (ACPI\APLO0002) whose one child (compatible ID MS_BTHX_BTHMINI) gets the inbox
 * BthMini, which sends IOCTL_BTHX_* (bthxddi.h, as Microsoft's serialhcibus sample).
 *
 * Progress goes to HKLM\SYSTEM\CurrentControlSet\Services\apollobt\Parameters (Status, Chip, Firmware, Address,
 * Traffic) and DbgPrintEx. Optional DWORDs there: Steps (default 5; 1 read clocks and pins, 2 + the UART's
 * registers, 3 + internal loopback, 4 + rails, BT_EN and the ROM's version, then off again; 5 everything and stay
 * up), Baud (default 921600), StartDelayMs (default 0); REG_BINARY Address (6 bytes, display order).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <bthxddi.h>

#define TAG 'tbpA'
#define REG_PARAMS L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\apollobt\\Parameters"
#define FW_PATCH   L"\\SystemRoot\\System32\\drivers\\htbtfw20.tlv"
#define FW_NVM     L"\\SystemRoot\\System32\\drivers\\htnv20.bin"

/* ---------- hardware ---------- */
#define GCC_PA          0x00100000ULL
#define GCC_SIZE        0x80000
#define TLMM_NORTH_PA   0x0f900000ULL   /* gpio16-21 and 124 */
#define TLMM_SIZE       0x80000
#define SE6_PA          0x00998000ULL
#define SE6_SIZE        0x4000

#define GCC_QUP_VOTE    0x52008u
#define GCC_QUP_M_AHB   0x17004u
#define GCC_QUP_S_AHB   0x17008u
#define GCC_SE6_CBCR    0x1772cu
#define GCC_SE6_CMD     0x17730u
#define GCC_SE6_CFG     0x17734u
#define GCC_SE6_M       0x17738u
#define GCC_SE6_N       0x1773cu
#define GCC_SE6_D       0x17740u
#define GCC_QUP_AHB     ((1u << 6) | (1u << 7))     /* shared with the touch driver's SE4: never voted off */
#define GCC_SE6_BIT     (1u << 16)
#define CBCR_OFF        0x80000000u

#define PIN_BT_EN       21u
#define PIN_SW_CTRL     124u
#define TLMM_CFG(p)     ((ULONG)(p) * 0x1000u)
#define TLMM_IO(p)      ((ULONG)(p) * 0x1000u + 4u)

#define GENI_STATUS     0x40u
#define GENI_TX_LEN     0x270u
#define GENI_M_CMD      0x600u
#define GENI_M_CANCEL   0x604u
#define GENI_M_IRQ      0x610u
#define GENI_M_IRQ_CLR  0x618u
#define GENI_S_CMD      0x630u
#define GENI_S_CANCEL   0x634u
#define GENI_S_IRQ      0x640u
#define GENI_S_IRQ_CLR  0x648u
#define GENI_TX_FIFO    0x700u
#define GENI_RX_FIFO    0x780u
#define GENI_TX_STATUS  0x800u
#define GENI_RX_STATUS  0x804u
#define GENI_TX_DEPTH   0xe24u
#define GENI_RX_DEPTH   0xe28u

/* RPMh: the apps RSC, DRV 2, its ACTIVE TCSes; cmd-db (sweet_rpmh.c) */
#define RH_RSC_PA       0x18220000ULL
#define RH_TCS_PA       0x18220d00ULL
#define RH_TCS_SIZE     0x3000u
#define RH_DB_PA        0x80860000ULL
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

/* ---------- H4 ---------- */
#define H4_MAX          1100u           /* largest packet: ACL 5 + 1021, event 3 + 255 */
#define RX_RAW          16384u          /* power of two */
#define RX_SLOTS        64u             /* packets waiting for a read request, per type */
#define EVENT_MAX       260u
#define NVM_MOST        16384u
#define PATCH_MOST      (512u * 1024u)
#define ACL_PAYLOAD     1021u
#define CTX_HDR         ((ULONG)FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data))

typedef struct {
    ULONG Baud;
    UCHAR Qca;
    USHORT M, N;
    UCHAR Div;
} BT_SPEED;

/* QCA set-baudrate index; SE6 root = GPLL0 even (300 MHz) x M / N; root = 16 x baud x div (sweet_bt.h) */
static const BT_SPEED Speeds[] = {
    { 115200, 0, 384, 15625, 4 },
    { 230400, 5, 384, 15625, 2 },
    { 460800, 7, 384, 15625, 1 },
    { 921600, 10, 768, 15625, 1 },
    { 1000000, 11, 4, 75, 1 },
    { 2000000, 13, 8, 75, 1 },
    { 3000000, 14, 4, 25, 1 },
    { 3200000, 17, 64, 375, 1 },
};

typedef struct {
    UCHAR Bytes[H4_MAX];
    ULONG Used, Wanted, Header;
    BOOLEAN Complete;
} H4;

typedef struct {
    ULONG Len;
    UCHAR Data[H4_MAX];                 /* without the H4 type byte */
} SLOT;

typedef struct {
    SLOT Slot[RX_SLOTS];
    ULONG Head, Tail;
} RING;

typedef struct {
    WDFDEVICE Device, Child;
    WDFQUEUE WriteQ, EventQ, AclQ;
    PKTHREAD Thread;
    KEVENT Stop, Kick;
    PUCHAR Gcc, Tlmm, Uart, Rsc, Tcs, Db;
    ULONG Steps, Baud, StartDelayMs, NcPt;
    BOOLEAN RhPoisoned, Up, Awake, AckPending, HaveAddress;
    UCHAR Address[6];
    const BT_SPEED *Speed;
    UCHAR Raw[RX_RAW];
    ULONG RawHead, RawTail;
    H4 Rx;
    RING Events, Acl;
    UCHAR Tx[H4_MAX + 1];
    ULONG Wakes, Sleeps, CmdsOut, AclOut, EventsIn, AclIn, Dropped;
    PUCHAR Patch, Nvm;
    ULONG PatchSize, NvmSize;
} BT_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BT_CTX, GetCtx)

typedef struct {
    BT_CTX *Bus;
} PDO_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PDO_CTX, GetPdoCtx)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT EvtIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP EvtIoCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtPdoIoctl;
static NTSTATUS CreateChild(BT_CTX *c);

/* ---------- logging and parameters ---------- */
static VOID Note(PCWSTR Name, PCSTR Fmt, ...)
{
    CHAR a[200];
    WCHAR w[200];
    va_list args;
    ULONG i;
    UNICODE_STRING key, val;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;

    va_start(args, Fmt);
    (void)RtlStringCbVPrintfA(a, sizeof(a), Fmt, args);
    va_end(args);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "apollobt: %s\n", a);
    for (i = 0; a[i] && i < 199; i++)
        w[i] = (WCHAR)(UCHAR)a[i];
    w[i] = 0;
    RtlInitUnicodeString(&key, REG_PARAMS);
    InitializeObjectAttributes(&oa, &key, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&val, Name);
        (void)ZwSetValueKey(h, &val, 0, REG_SZ, w, (i + 1) * sizeof(WCHAR));
        ZwClose(h);
    }
}

static ULONG ReadParam(PCWSTR Name, ULONG Default, PUCHAR Bin, ULONG BinLen)
{
    UNICODE_STRING key, val;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 16];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len, v = Default;

    RtlInitUnicodeString(&key, REG_PARAMS);
    InitializeObjectAttributes(&oa, &key, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) {
        RtlInitUnicodeString(&val, Name);
        if (NT_SUCCESS(ZwQueryValueKey(h, &val, KeyValuePartialInformation, info, sizeof(buf), &len))) {
            if (Bin && info->Type == REG_BINARY && info->DataLength == BinLen) {
                RtlCopyMemory(Bin, info->Data, BinLen);
                v = 1;
            } else if (!Bin && info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
                v = *(ULONG *)info->Data;
            }
        }
        ZwClose(h);
    }
    return v;
}

static BOOLEAN LoadFile(PCWSTR Path, ULONG Most, PUCHAR *Out, ULONG *Size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION si;
    HANDLE h;
    NTSTATUS st;

    RtlInitUnicodeString(&path, Path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                      FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st))
        return FALSE;
    st = ZwQueryInformationFile(h, &io, &si, sizeof(si), FileStandardInformation);
    if (NT_SUCCESS(st) && (si.EndOfFile.QuadPart < 4 || si.EndOfFile.QuadPart > Most))
        st = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(st)) {
        *Size = (ULONG)si.EndOfFile.QuadPart;
        *Out = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, *Size, TAG);
        st = *Out ? ZwReadFile(h, NULL, NULL, NULL, &io, *Out, *Size, NULL, NULL) : STATUS_NO_MEMORY;
    }
    ZwClose(h);
    return NT_SUCCESS(st);
}

/* ---------- MMIO and time ---------- */
static __forceinline ULONG Rd(PUCHAR Base, ULONG Off)
{
    ULONG v = READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
    __dmb(_ARM64_BARRIER_OSHLD);
    return v;
}

static __forceinline VOID Wr(PUCHAR Base, ULONG Off, ULONG Val)
{
    __dmb(_ARM64_BARRIER_OSHST);
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
    __dsb(_ARM64_BARRIER_OSH);
}

static VOID Us(ULONG N)
{
    KeStallExecutionProcessor(N);
}

/* Sleeps; TRUE if the driver is stopping. */
static BOOLEAN WaitMs(BT_CTX *c, ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * (LONGLONG)Ms;
    return KeWaitForSingleObject(&c->Stop, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

static ULONGLONG NowMs(VOID)
{
    return KeQueryInterruptTime() / 10000ULL;
}

static PUCHAR Map(ULONGLONG Pa, SIZE_T Size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (LONGLONG)Pa;
    return (PUCHAR)MmMapIoSpaceEx(p, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

static USHORT Le16(const UCHAR *p)
{
    return (USHORT)(p[0] | p[1] << 8);
}

static ULONG Le32(const UCHAR *p)
{
    return Le16(p) | (ULONG)Le16(p + 2) << 16;
}

/* ---------- RPMh: ACTIVE votes for the chip's rails (sweet_rpmh.c) ---------- */
static BOOLEAN RhSync(BT_CTX *c, ULONG Off, ULONG Val)
{
    ULONG i;
    Wr(c->Tcs, Off, Val);
    for (i = 0; i < 1000; i++) {
        if (Rd(c->Tcs, Off) == Val)
            return TRUE;
        Us(1);
    }
    return FALSE;
}

static USHORT Db16(BT_CTX *c, ULONG Off)
{
    return (USHORT)(Rd(c->Db, Off & ~3u) >> (8u * (Off & 3u)));
}

static NTSTATUS RhMap(BT_CTX *c)
{
    ULONG id, solver, config, major, count;

    if (!c->Rsc) c->Rsc = Map(RH_RSC_PA, 0x100);
    if (!c->Tcs) c->Tcs = Map(RH_TCS_PA, RH_TCS_SIZE);
    if (!c->Db) c->Db = Map(RH_DB_PA, RH_DB_SIZE);
    if (!c->Rsc || !c->Tcs || !c->Db)
        return STATUS_INSUFFICIENT_RESOURCES;
    id = Rd(c->Rsc, 0);
    solver = Rd(c->Rsc, 4);
    config = Rd(c->Rsc, 0x0c);
    major = (id >> 16) & 0xffu;
    count = (config >> (6u * RH_DRV)) & 0x3fu;
    c->NcPt = (config >> 27) & 0x1fu;
    if ((major != 1 && major != 2) || config == 0xffffffffu || solver == 0xffffffffu || count < RH_NORMAL_N ||
        count > RH_TCS_SIZE / RH_TCS_STRIDE || c->NcPt < 3 || c->NcPt > 16) {
        Note(L"Status", "RPMh: unexpected RSC (id %#lx config %#lx); stopped", id, config);
        return STATUS_NOT_SUPPORTED;
    }
    if (Rd(c->Db, 4) != RH_DB_MAGIC || Rd(c->Db, 0) != 1 || (Rd(c->Db, 140) & 1u)) {
        Note(L"Status", "RPMh: no cmd-db at 0x80860000 (magic %#lx); stopped", Rd(c->Db, 4));
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN DbRange(ULONG Off, ULONG Size)
{
    return Off <= RH_DB_SIZE && Size <= RH_DB_SIZE - Off;
}

static NTSTATUS RhLookup(BT_CTX *c, PCSTR Res, ULONG *Address)
{
    ULONG key[2] = { 0, 0 }, i, j, matches = 0;

    for (i = 0; i < 8 && Res[i]; i++)
        key[i / 4] |= (ULONG)(UCHAR)Res[i] << (8u * (i % 4));
    for (i = 0; i < 8; i++) {
        ULONG h = 8u + 16u * i, slave = Db16(c, h), entries, aux, count;
        if (!slave)
            break;
        entries = RH_DB_HEADER + Db16(c, h + 2);
        aux = RH_DB_HEADER + Db16(c, h + 4);
        count = Db16(c, h + 6);
        if (slave < 3 || slave > 5 || (entries & 3u) || !DbRange(entries, count * RH_DB_ENTRY) || !DbRange(aux, 0))
            return STATUS_INVALID_PARAMETER;
        for (j = 0; j < count; j++) {
            ULONG e = entries + RH_DB_ENTRY * j, addr;
            if (Rd(c->Db, e) != key[0] || Rd(c->Db, e + 4) != key[1])
                continue;
            addr = Rd(c->Db, e + 16);
            if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15u))
                return STATUS_INVALID_PARAMETER;
            if (matches && *Address != addr)
                return STATUS_INVALID_PARAMETER;
            *Address = addr;
            matches++;
        }
    }
    return matches ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static BOOLEAN RhFree(BT_CTX *c, ULONG T)
{
    ULONG b = T * RH_TCS_STRIDE, st = Rd(c->Tcs, b + RH_STATUS);
    return st != 0 && st != 0xffffffffu && (Rd(c->Tcs, b + RH_CONTROL) & ~RH_AMC_EN) == 0 &&
           Rd(c->Tcs, b + RH_ENABLE) == 0 && Rd(c->Tcs, b + RH_WAIT) == 0 &&
           !(Rd(c->Tcs, RH_IRQ_STATUS) & (1u << T));
}

/* A regulator's voltage and enable as one ACTIVE request, waited for. */
static NTSTATUS RhSend(BT_CTX *c, ULONG Address, ULONG Mv)
{
    const ULONG offsets[2] = { 0, 4 }, values[2] = { Mv, 1 };
    LONG chosen = -1;
    ULONG t, b, bit, irqEn, inherited, cmd, us, irq = 0, idle = 0, status[2] = { 0, 0 };
    BOOLEAN done = FALSE, progress = FALSE;

    for (t = 0; t < RH_ACTIVE_N; t++) {
        ULONG en = Rd(c->Tcs, t * RH_TCS_STRIDE + RH_ENABLE), k;
        if (en & ~((1u << c->NcPt) - 1u))
            return STATUS_DEVICE_BUSY;
        for (k = 0; k < c->NcPt; k++)
            if ((en & (1u << k)) && (Rd(c->Tcs, t * RH_TCS_STRIDE + RH_ADDR + k * RH_CMD_STRIDE) & ~15u) == Address)
                return STATUS_DEVICE_BUSY;
        if (chosen < 0 && RhFree(c, t))
            chosen = (LONG)t;
    }
    if (chosen < 0)
        return STATUS_DEVICE_BUSY;
    t = (ULONG)chosen;
    b = t * RH_TCS_STRIDE;
    bit = 1u << t;
    irqEn = Rd(c->Tcs, RH_IRQ_EN);
    inherited = Rd(c->Tcs, b + RH_CONTROL);
    if (irqEn == 0xffffffffu || !RhSync(c, RH_IRQ_EN, irqEn & ~bit)) {
        c->RhPoisoned = TRUE;
        return STATUS_IO_DEVICE_ERROR;
    }
    c->RhPoisoned = TRUE;           /* until proven complete */
    for (cmd = 0; cmd < 2; cmd++) {
        ULONG o = b + cmd * RH_CMD_STRIDE;
        if (!RhSync(c, o + RH_MSGID, RH_WRITE_ACK) || !RhSync(c, o + RH_ADDR, Address + offsets[cmd]) ||
            !RhSync(c, o + RH_DATA, values[cmd]))
            return STATUS_IO_DEVICE_ERROR;
    }
    if (!RhSync(c, b + RH_WAIT, 3) || !RhSync(c, b + RH_ENABLE, 3) || !RhSync(c, b + RH_CONTROL, 0) ||
        !RhSync(c, b + RH_CONTROL, RH_AMC_EN))
        return STATUS_IO_DEVICE_ERROR;
    Wr(c->Tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
    for (us = 0; us < 100000; us++) {
        BOOLEAN complete = TRUE;
        irq = Rd(c->Tcs, RH_IRQ_STATUS);
        idle = Rd(c->Tcs, b + RH_STATUS);
        for (cmd = 0; cmd < 2; cmd++) {
            status[cmd] = Rd(c->Tcs, b + RH_CMD_STATUS + cmd * RH_CMD_STRIDE);
            if (status[cmd] == 0xffffffffu || (status[cmd] & (RH_ISSUED | RH_COMPLETE)) != (RH_ISSUED | RH_COMPLETE))
                complete = FALSE;
        }
        if (idle == 0 || !complete)
            progress = TRUE;
        if (idle && idle != 0xffffffffu && irq != 0xffffffffu && ((irq & bit) || progress) && complete) {
            done = TRUE;
            break;
        }
        Us(1);
    }
    if (!done)
        return STATUS_IO_TIMEOUT;
    if (!RhSync(c, b + RH_CONTROL, RH_AMC_EN) || !RhSync(c, b + RH_CONTROL, inherited) ||
        !RhSync(c, b + RH_ENABLE, 0) || !RhSync(c, b + RH_WAIT, 0))
        return STATUS_IO_DEVICE_ERROR;
    Wr(c->Tcs, RH_IRQ_CLEAR, bit);
    if ((Rd(c->Tcs, RH_IRQ_STATUS) & bit) || !RhSync(c, RH_IRQ_EN, (Rd(c->Tcs, RH_IRQ_EN) & ~bit) | (irqEn & bit)))
        return STATUS_IO_DEVICE_ERROR;
    c->RhPoisoned = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS BtPower(BT_CTX *c)
{
    static const struct { PCSTR Res; ULONG Mv; } rails[] = {
        { "smpa6", 950 },   /* PM8150 S6: AON and digital */
        { "smpa5", 1900 },  /* PM8150 S5: RFA1 */
        { "smpc8", 1350 },  /* PM8150A S8: RFA2 */
        { "ldoa16", 3025 }, /* PM8150 L16: ASD */
    };
    ULONG addr[4], i, tries;
    NTSTATUS st = RhMap(c);

    for (i = 0; NT_SUCCESS(st) && i < 4; i++) {
        st = RhLookup(c, rails[i].Res, &addr[i]);
        if (!NT_SUCCESS(st))
            Note(L"Status", "RPMh: %s not in cmd-db (%#lx); stopped", rails[i].Res, st);
    }
    for (i = 0; NT_SUCCESS(st) && i < 4; i++) {
        if (c->RhPoisoned)
            return STATUS_IO_DEVICE_ERROR;
        for (tries = 0; tries < 20; tries++) {
            st = RhSend(c, addr[i], rails[i].Mv);
            if (st != STATUS_DEVICE_BUSY)
                break;
            Us(5000);
        }
        if (!NT_SUCCESS(st))
            Note(L"Status", "RPMh: %s %lu mV vote failed (%#lx); stopped", rails[i].Res, rails[i].Mv, st);
    }
    return st;
}

/* ---------- GENI UART on SE6 ---------- */
static VOID UartSetup(BT_CTX *c)
{
    PUCHAR u = c->Uart;
    ULONG div = c->Speed->Div;
    Wr(u, 0x48, (div << 4) | 1);
    Wr(u, 0x4c, (div << 4) | 1);
    Wr(u, 0x258, 0);
    Wr(u, 0x22c, 0);
    Wr(u, 0x25c, 2);
    Wr(u, 0x280, 0);
    Wr(u, 0x260, 0xf);
    Wr(u, 0x264, 0);
    Wr(u, 0x284, 0x4380e);
    Wr(u, 0x288, 0xc3e0e);
    Wr(u, 0x268, 8);
    Wr(u, 0x28c, 8);
    Wr(u, 0x26c, 0);
    Wr(u, 0x294, 160);
    Wr(u, 0x2a4, 0);
    Wr(u, 0x2a8, 0);
    Wr(u, 0x80c, 1);
    Wr(u, 0x810, 0);
    Wr(u, 0x814, 6);
    Wr(u, GENI_M_IRQ_CLR, 0xffffffffu);
    Wr(u, GENI_S_IRQ_CLR, 0xffffffffu);
}

static NTSTATUS UartQuiesce(BT_CTX *c)
{
    PUCHAR u = c->Uart;
    ULONG attempt, t, depth, words;

    for (attempt = 0; attempt < 2; attempt++) {
        ULONG active = Rd(u, GENI_STATUS);
        if (!(active & 0x1001))
            break;
        if (active & 1)
            Wr(u, GENI_M_CANCEL, attempt ? 2 : 4);
        if (active & 0x1000)
            Wr(u, GENI_S_CANCEL, attempt ? 2 : 4);
        for (t = 0; t < 10000 && (Rd(u, GENI_STATUS) & 0x1001); t++)
            Us(1);
    }
    if (Rd(u, GENI_STATUS) & 0x1001)
        return STATUS_IO_TIMEOUT;
    depth = (Rd(u, GENI_RX_DEPTH) >> 16) & 0x3f;
    words = Rd(u, GENI_RX_STATUS) & 0x1ffffff;
    if (!depth || words > depth)
        return STATUS_IO_DEVICE_ERROR;
    while (words--)
        (void)Rd(u, GENI_RX_FIFO);
    Wr(u, GENI_M_IRQ_CLR, 0xffffffffu);
    Wr(u, GENI_S_IRQ_CLR, 0xffffffffu);
    return STATUS_SUCCESS;
}

/* The receive FIFO into c->Raw. Returns how many bytes came. */
static LONG RxCapture(BT_CTX *c)
{
    PUCHAR u = c->Uart;
    ULONG irq = Rd(u, GENI_S_IRQ), budget, got = 0;

    if (irq)
        Wr(u, GENI_S_IRQ_CLR, irq);
    for (budget = 0; budget < 64; budget++) {
        ULONG status = Rd(u, GENI_RX_STATUS), words = status & 0x1ffffff, word, last, bytes, b;
        if (!words)
            break;
        if (words > ((Rd(u, GENI_RX_DEPTH) >> 16) & 0x3f))
            return -1;
        last = (status >> 28) & 7;
        bytes = (words == 1 && (status >> 31) && last) ? last : 4;
        if (bytes > 4)
            return -1;
        if (RX_RAW - (c->RawHead - c->RawTail) < bytes)
            return -2;
        word = Rd(u, GENI_RX_FIFO);
        for (b = 0; b < bytes; b++)
            c->Raw[c->RawHead++ & (RX_RAW - 1)] = (UCHAR)(word >> (8 * b));
        got += bytes;
    }
    return (LONG)got;
}

/* One more byte of H4: 0 not yet, 1 complete, 2 an IBS byte, -1 out of step (sweet_bt.h). */
static int H4Push(H4 *p, UCHAR Byte)
{
    if (p->Complete) {
        p->Used = p->Wanted = p->Header = 0;
        p->Complete = FALSE;
    }
    if (p->Used == 0) {
        if (Byte >= 0xfc && Byte <= 0xfe)
            return 2;
        switch (Byte) {
        case 1: case 3: p->Header = 4; break;
        case 2: case 5: p->Header = 5; break;
        case 4: p->Header = 3; break;
        default: return -1;
        }
        p->Wanted = p->Header;
    }
    if (p->Used >= H4_MAX)
        return -1;
    p->Bytes[p->Used++] = Byte;
    if (p->Used == p->Header) {
        ULONG len;
        switch (p->Bytes[0]) {
        case 1: case 3: len = p->Bytes[3]; break;
        case 4: len = p->Bytes[2]; break;
        default:
            len = p->Bytes[3] | (ULONG)p->Bytes[4] << 8;
            if (p->Bytes[0] == 5)
                len &= 0x3fff;
        }
        p->Wanted = p->Header + len;
        if (p->Wanted > H4_MAX)
            return -1;
    }
    if (p->Used == p->Wanted) {
        p->Complete = TRUE;
        return 1;
    }
    return 0;
}

/* The next whole packet (only events if EventsOnly): 1, 0 none yet, <0 error. */
static int RxFrame(BT_CTX *c, UCHAR *Frame, ULONG Cap, ULONG *Len, BOOLEAN EventsOnly)
{
    if (RxCapture(c) < 0)
        return -1;
    while (c->RawHead != c->RawTail) {
        UCHAR byte = c->Raw[c->RawTail++ & (RX_RAW - 1)];
        int r = H4Push(&c->Rx, byte);
        if (r < 0) {
            RtlZeroMemory(&c->Rx, sizeof(c->Rx));
            c->Dropped++;
            continue;                   /* resynchronise on the next packet type */
        }
        if (r == 2) {
            if (byte == 0xfd) {
                c->AckPending = TRUE;
                c->Wakes++;
            } else if (byte == 0xfe) {
                c->Sleeps++;
                c->Awake = FALSE;
            } else {
                c->Awake = TRUE;
            }
            continue;
        }
        if (r != 1 || (EventsOnly && c->Rx.Bytes[0] != 4))
            continue;
        if (c->Rx.Used > Cap)
            return -1;
        *Len = c->Rx.Used;
        RtlCopyMemory(Frame, c->Rx.Bytes, *Len);
        return 1;
    }
    return 0;
}

static NTSTATUS UartTx(BT_CTX *c, const UCHAR *Pkt, ULONG Len)
{
    PUCHAR u = c->Uart;
    ULONG sent = 0, depth = (Rd(u, GENI_TX_DEPTH) >> 16) & 0x3f, elapsed;

    if (!depth || depth > 64 || !Len || Len > H4_MAX + 1)
        return STATUS_INVALID_PARAMETER;
    Wr(u, GENI_M_IRQ_CLR, 0xffffffffu);
    Wr(u, GENI_TX_LEN, Len);
    Wr(u, GENI_M_CMD, 1u << 27);
    for (elapsed = 0; elapsed < Len * 180 + 10000; elapsed++) {
        ULONG used, irq, room;
        if (RxCapture(c) < 0)
            return STATUS_IO_DEVICE_ERROR;
        used = Rd(u, GENI_TX_STATUS) & 0x0fffffff;
        if (used > depth)
            return STATUS_IO_DEVICE_ERROR;
        for (room = depth - used; room && sent < Len; room--)
            Wr(u, GENI_TX_FIFO, Pkt[sent++]);       /* one byte a FIFO word (TX packing 0xf, as umi) */
        irq = Rd(u, GENI_M_IRQ);
        if (sent == Len && (irq & 1)) {
            Wr(u, GENI_M_IRQ_CLR, irq);
            return STATUS_SUCCESS;
        }
        Us(1);
    }
    Wr(u, GENI_M_CANCEL, 4);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS ServiceIbs(BT_CTX *c)
{
    static const UCHAR ack = 0xfc;
    NTSTATUS st;
    if (!c->AckPending)
        return STATUS_SUCCESS;
    st = UartTx(c, &ack, 1);
    if (NT_SUCCESS(st))
        c->AckPending = FALSE;
    return st;
}

static int CcStatus(const UCHAR *e, ULONG n, USHORT Op)
{
    if (n < 7 || e[0] != 4 || e[1] != 0x0e || (ULONG)e[2] + 3 != n || e[4] != (UCHAR)Op || e[5] != (UCHAR)(Op >> 8))
        return -1;
    return e[6];
}

static BOOLEAN TlvAck(const UCHAR *e, ULONG n)
{
    return (n == 8 && CcStatus(e, n, 0xfc00) == 0 && e[7] == 0x1e) ||
           (n == 6 && e[0] == 4 && e[1] == 0xff && e[2] == 3 && e[3] == 0 && e[4] == 4 && e[5] == 0);
}

/* An HCI command during bring-up; with Wait, its Command Complete/Status (1 s). */
static NTSTATUS Hci(BT_CTX *c, USHORT Op, const UCHAR *P, ULONG PLen, BOOLEAN Wait, UCHAR *Ev, ULONG *EvLen)
{
    UCHAR pkt[260];
    NTSTATUS st;
    ULONG t;

    if (PLen > 255)
        return STATUS_INVALID_PARAMETER;
    pkt[0] = 1;
    pkt[1] = (UCHAR)Op;
    pkt[2] = (UCHAR)(Op >> 8);
    pkt[3] = (UCHAR)PLen;
    if (PLen)
        RtlCopyMemory(pkt + 4, P, PLen);
    *EvLen = 0;
    st = UartTx(c, pkt, PLen + 4);
    if (!NT_SUCCESS(st) || !Wait)
        return st;
    for (t = 0; t < 100000; t++) {
        int r = RxFrame(c, Ev, EVENT_MAX, EvLen, TRUE);
        if (r < 0)
            return STATUS_IO_DEVICE_ERROR;
        st = ServiceIbs(c);
        if (!NT_SUCCESS(st))
            return st;
        if (r == 1) {
            int s = CcStatus(Ev, *EvLen, Op);
            if (s >= 0)
                return s ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
            if (Op == 0xfc00 && TlvAck(Ev, *EvLen))
                return STATUS_SUCCESS;
            if (Op == 0xfc14 && *EvLen >= 3 && Ev[1] == 0xff)
                return STATUS_SUCCESS;
            if (*EvLen == 7 && Ev[1] == 0x0f && Ev[5] == (UCHAR)Op && Ev[6] == (UCHAR)(Op >> 8))
                return Ev[3] ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
        }
        *EvLen = 0;
        Us(10);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS Download(BT_CTX *c, const UCHAR *Tlv, ULONG Size, BOOLEAN SkipEvents)
{
    ULONG off = 0, seg = 0;
    if (!Tlv || Size < 4 || Size > PATCH_MOST || (Tlv[1] | (ULONG)Tlv[2] << 8 | (ULONG)Tlv[3] << 16) != Size - 4)
        return STATUS_INVALID_PARAMETER;
    while (off < Size) {
        ULONG part = Size - off > 243 ? 243 : Size - off, len = 0;
        UCHAR p[245], ev[EVENT_MAX];
        BOOLEAN ack;
        NTSTATUS st;
        p[0] = 0x1e;
        p[1] = (UCHAR)part;
        RtlCopyMemory(p + 2, Tlv + off, part);
        off += part;
        seg++;
        ack = (BOOLEAN)(!SkipEvents || off == Size);
        st = Hci(c, 0xfc00, p, part + 2, ack, ev, &len);
        if (NT_SUCCESS(st) && ack && !TlvAck(ev, len))
            st = STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(st)) {
            Note(L"Firmware", "record type %u: segment %lu of %lu refused (%#lx)", Tlv[0], seg, (Size + 242) / 243, st);
            return st;
        }
    }
    return STATUS_SUCCESS;
}

/* The NVM's HCI transport record (tag 17) set to the UART speed (sweet_bt.h). */
static BOOLEAN NvmPrepare(PUCHAR d, ULONG n, UCHAR Qca)
{
    ULONG off, hci = 0;
    if (n < 4 || d[0] != 2 || ((ULONG)d[1] | (ULONG)d[2] << 8 | (ULONG)d[3] << 16) != n - 4)
        return FALSE;
    for (off = 4; off < n;) {
        ULONG id, len;
        if (n - off < 12)
            return FALSE;
        id = d[off] | (ULONG)d[off + 1] << 8;
        len = d[off + 2] | (ULONG)d[off + 3] << 8;
        if (len > n - off - 12)
            return FALSE;
        if (id == 17) {
            if (len < 3 || hci)
                return FALSE;
            hci = off + 12;
        }
        off += 12 + len;
    }
    if (!hci)
        return FALSE;
    d[hci + 1] = Qca;
    return TRUE;
}

/* SE6's root clock for Speed: GPLL0 even x M/N, dual-edge (sweet_bt.c bt_uart_clock). */
static BOOLEAN UartClock(BT_CTX *c, const BT_SPEED *s)
{
    ULONG i;
    Wr(c->Gcc, GCC_SE6_M, s->M);
    Wr(c->Gcc, GCC_SE6_N, ~((ULONG)s->N - s->M) & 0xffff);
    Wr(c->Gcc, GCC_SE6_D, ~(ULONG)s->N & 0xffff);
    Wr(c->Gcc, GCC_SE6_CFG, 0x2601);
    Wr(c->Gcc, GCC_SE6_CMD, 1);
    for (i = 0; i < 10000; i++) {
        if (!(Rd(c->Gcc, GCC_SE6_CMD) & 1))
            return TRUE;
        Us(1);
    }
    return FALSE;
}

static NTSTATUS SetSpeed(BT_CTX *c, const BT_SPEED *s)
{
    UCHAR ev[EVENT_MAX];
    ULONG len = 0;
    NTSTATUS st = Hci(c, 0xfc48, &s->Qca, 1, FALSE, ev, &len);
    if (!NT_SUCCESS(st))
        return st;
    if (WaitMs(c, 300))
        return STATUS_CANCELLED;
    st = UartQuiesce(c);
    if (!NT_SUCCESS(st))
        return st;
    if (!UartClock(c, s))
        return STATUS_IO_TIMEOUT;
    c->Speed = s;
    Wr(c->Uart, 0x48, ((ULONG)s->Div << 4) | 1);
    Wr(c->Uart, 0x4c, ((ULONG)s->Div << 4) | 1);
    RtlZeroMemory(&c->Rx, sizeof(c->Rx));
    c->RawHead = c->RawTail = 0;
    Wr(c->Uart, GENI_S_CMD, (1u << 27) | 0x80);
    st = Hci(c, 0x1001, NULL, 0, TRUE, ev, &len);
    if (NT_SUCCESS(st) && len != 15)
        st = STATUS_UNSUCCESSFUL;
    return st;
}

static VOID Gpio(BT_CTX *c, ULONG Pin, ULONG Cfg, BOOLEAN High)
{
    Wr(c->Tlmm, TLMM_IO(Pin), High ? 2 : 0);
    Wr(c->Tlmm, TLMM_CFG(Pin), Cfg);
}

static BOOLEAN ClocksRunning(BT_CTX *c)
{
    return !((Rd(c->Gcc, GCC_QUP_M_AHB) | Rd(c->Gcc, GCC_QUP_S_AHB) | Rd(c->Gcc, GCC_SE6_CBCR)) & CBCR_OFF);
}

/* Power-up and the ROM's answer at 115200 (ios-apollo step 4). */
static NTSTATUS ChipOn(BT_CTX *c)
{
    UCHAR ev[EVENT_MAX];
    ULONG len = 0, pin;
    NTSTATUS st;

    c->Speed = &Speeds[0];
    (void)UartQuiesce(c);
    if (!UartClock(c, c->Speed))
        return STATUS_IO_TIMEOUT;
    Gpio(c, PIN_BT_EN, 0x241, FALSE);           /* GPIO, 4 mA, pull-down, output, low */
    Gpio(c, PIN_SW_CTRL, 0x040, FALSE);         /* input, no pull */
    if (WaitMs(c, 56))
        return STATUS_CANCELLED;
    Gpio(c, PIN_BT_EN, 0x241, TRUE);
    if (WaitMs(c, 56))
        return STATUS_CANCELLED;
    for (pin = 16; pin <= 19; pin++)            /* qup function 1; RTS pulled down, TX pulled up */
        Wr(c->Tlmm, TLMM_CFG(pin), pin == 17 ? 5u : pin == 18 ? 7u : 4u);
    UartSetup(c);
    Wr(c->Uart, GENI_S_CMD, (1u << 27) | 0x80);
    RtlZeroMemory(&c->Rx, sizeof(c->Rx));
    c->RawHead = c->RawTail = 0;
    c->Awake = c->AckPending = FALSE;
    st = Hci(c, 0x0c03, NULL, 0, TRUE, ev, &len);
    if (NT_SUCCESS(st))
        st = Hci(c, 0x1001, NULL, 0, TRUE, ev, &len);
    if (NT_SUCCESS(st) && len != 15)
        st = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(st))
        Note(L"Chip", "answers from its ROM: HCI %#x, manufacturer %u, subversion %#x; SW_CTRL %lu",
             ev[7], Le16(ev + 11), Le16(ev + 13), Rd(c->Tlmm, TLMM_IO(PIN_SW_CTRL)) & 1);
    else
        Note(L"Chip", "no answer from the ROM (%#lx; SW_CTRL %lu)", st, Rd(c->Tlmm, TLMM_IO(PIN_SW_CTRL)) & 1);
    return st;
}

static NTSTATUS Firmware(BT_CTX *c)
{
    UCHAR ev[EVENT_MAX];
    ULONG len = 0, product, soc, mode, i, nonzero = 0;
    USHORT rom;
    static const UCHAR versionReq = 0x19, logging[2] = { 0x14, 0 };
    PUCHAR nvm;
    NTSTATUS st;

    st = Hci(c, 0xfc00, &versionReq, 1, TRUE, ev, &len);
    if (!NT_SUCCESS(st))
        return st;
    if (len != 21 || ev[7] != 0x19) {
        Note(L"Firmware", "the chip's version answer is %lu bytes, not 21", len);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    product = Le32(ev + 9);
    rom = Le16(ev + 15);
    soc = Le32(ev + 17);
    if (product != 0x10 || rom != 0x200 || soc != 0x400a0200) {
        Note(L"Firmware", "not a QCA6390 (product %#lx ROM %#x SoC %#lx)", product, rom, soc);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (!c->Patch || !c->Nvm || c->PatchSize < 28) {
        Note(L"Firmware", "htbtfw20.tlv / htnv20.bin not in System32\\drivers (install-apollobt.ps1 -Firmware)");
        return STATUS_NOT_FOUND;
    }
    if (Le16(c->Patch + 16) != product || Le16(c->Patch + 18) != rom) {
        Note(L"Firmware", "the patch is for product %#x ROM %#x", Le16(c->Patch + 16), Le16(c->Patch + 18));
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    mode = c->Patch[14];
    if (mode != 0 && mode != 3)
        return STATUS_INVALID_IMAGE_FORMAT;
    nvm = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, c->NvmSize, TAG);
    if (!nvm)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(nvm, c->Nvm, c->NvmSize);
    if (!NvmPrepare(nvm, c->NvmSize, c->Speed->Qca)) {
        ExFreePoolWithTag(nvm, TAG);
        Note(L"Firmware", "htnv20.bin has no HCI transport record (wrong file?)");
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    st = Download(c, c->Patch, c->PatchSize, (BOOLEAN)(mode == 3));
    if (NT_SUCCESS(st) && WaitMs(c, 10))
        st = STATUS_CANCELLED;
    if (NT_SUCCESS(st))
        st = Download(c, nvm, c->NvmSize, FALSE);
    ExFreePoolWithTag(nvm, TAG);
    if (NT_SUCCESS(st))
        st = Hci(c, 0xfc17, logging, sizeof(logging), TRUE, ev, &len);
    if (NT_SUCCESS(st))
        st = Hci(c, 0x0c03, NULL, 0, TRUE, ev, &len);
    if (NT_SUCCESS(st))
        st = Hci(c, 0x1001, NULL, 0, TRUE, ev, &len);
    if (!NT_SUCCESS(st))
        return st;
    if (len != 15 || Le16(ev + 11) != 29 || Le16(ev + 13) == 0x0d2b) {
        Note(L"Firmware", "after the download the chip does not run it (manufacturer %u, subversion %#x)",
             len == 15 ? Le16(ev + 11) : 0, len == 15 ? Le16(ev + 13) : 0);
        return STATUS_UNSUCCESSFUL;
    }
    Note(L"Firmware", "running: HCI %#x, LMP %#x, subversion %#x, UART %lu baud", ev[7], ev[10], Le16(ev + 13),
         c->Speed->Baud);
    if (c->HaveAddress) {
        st = Hci(c, 0xfc14, c->Address, 6, TRUE, ev, &len);    /* display order: QCA takes it big-endian */
        if (!NT_SUCCESS(st))
            Note(L"Address", "setting the Address parameter failed (%#lx); the NVM's is kept", st);
    }
    st = Hci(c, 0x1009, NULL, 0, TRUE, ev, &len);
    if (!NT_SUCCESS(st) || len != 13)
        return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    for (i = 0; i < 6; i++)
        nonzero |= ev[12 - i];
    Note(L"Address", "%02X:%02X:%02X:%02X:%02X:%02X%s", ev[12], ev[11], ev[10], ev[9], ev[8], ev[7],
         !nonzero || (ev[12] & 1) ? " (not a usable address: set the Address parameter)" : "");
    return STATUS_SUCCESS;
}

/* ---------- traffic between BthMini and the chip ---------- */
static BOOLEAN RingPut(RING *r, const UCHAR *Data, ULONG Len)
{
    SLOT *s;
    if (r->Head - r->Tail >= RX_SLOTS || Len > H4_MAX)
        return FALSE;
    s = &r->Slot[r->Head % RX_SLOTS];
    s->Len = Len;
    RtlCopyMemory(s->Data, Data, Len);
    r->Head++;
    return TRUE;
}

/* Waiting packets of one type into waiting read requests of that type. */
static VOID Deliver(BT_CTX *c, RING *r, WDFQUEUE Q, UCHAR Type)
{
    while (r->Head != r->Tail) {
        WDFREQUEST req;
        WDFMEMORY mem;
        PBTHX_HCI_READ_WRITE_CONTEXT ctx;
        size_t size;
        SLOT *s = &r->Slot[r->Tail % RX_SLOTS];
        ULONG need = CTX_HDR + s->Len;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Q, &req)))
            return;
        if (!NT_SUCCESS(WdfRequestRetrieveOutputMemory(req, &mem))) {
            WdfRequestComplete(req, STATUS_INVALID_PARAMETER);
            continue;
        }
        ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(mem, &size);
        if (size < need) {
            WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
            r->Tail++;
            c->Dropped++;
            continue;
        }
        ctx->Type = Type;
        ctx->DataLen = s->Len;
        RtlCopyMemory(ctx->Data, s->Data, s->Len);
        r->Tail++;
        WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, need);
    }
}

/* One write request from BthMini: wakes the chip if it sleeps, sends type + data. */
static NTSTATUS SendOne(BT_CTX *c, WDFREQUEST Req)
{
    WDFMEMORY mem;
    PBTHX_HCI_READ_WRITE_CONTEXT ctx;
    size_t size;
    NTSTATUS st;
    ULONGLONG until;

    st = WdfRequestRetrieveInputMemory(Req, &mem);
    if (!NT_SUCCESS(st))
        return st;
    ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(mem, &size);
    if (size < CTX_HDR || ctx->DataLen > H4_MAX || size < (size_t)CTX_HDR + ctx->DataLen ||
        (ctx->Type != HciPacketCommand && ctx->Type != HciPacketAclData))
        return STATUS_INVALID_PARAMETER;
    if (!c->Awake) {
        static const UCHAR wake = 0xfd;
        st = UartTx(c, &wake, 1);
        if (!NT_SUCCESS(st))
            return st;
        for (until = NowMs() + 200; !c->Awake && NowMs() < until;) {
            UCHAR f[H4_MAX];
            ULONG l = 0;
            int r = RxFrame(c, f, sizeof(f), &l, FALSE);
            if (r < 0)
                return STATUS_IO_DEVICE_ERROR;
            if (r == 1) {
                if (f[0] == 4) {
                    c->EventsIn++;
                    if (!RingPut(&c->Events, f + 1, l - 1)) c->Dropped++;
                } else if (f[0] == 2) {
                    c->AclIn++;
                    if (!RingPut(&c->Acl, f + 1, l - 1)) c->Dropped++;
                }
            }
            (void)ServiceIbs(c);
            Us(20);
        }
        c->Awake = TRUE;    /* sent anyway after 200 ms; the chip wakes on traffic too */
    }
    c->Tx[0] = ctx->Type;
    RtlCopyMemory(c->Tx + 1, ctx->Data, ctx->DataLen);
    st = UartTx(c, c->Tx, ctx->DataLen + 1);
    if (NT_SUCCESS(st)) {
        if (ctx->Type == HciPacketCommand) c->CmdsOut++; else c->AclOut++;
        WdfRequestSetInformation(Req, ctx->DataLen);
    }
    return st;
}

static VOID Pump(BT_CTX *c)
{
    ULONGLONG lastNote = NowMs();
    ULONG idle = 0;

    Note(L"Status", "running: Windows' Bluetooth stack can use the radio");
    for (;;) {
        BOOLEAN busy = FALSE;
        WDFREQUEST req;
        UCHAR f[H4_MAX];
        ULONG l = 0;
        int r;

        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(c->WriteQ, &req))) {
            NTSTATUS st = SendOne(c, req);
            WdfRequestComplete(req, st);
            busy = TRUE;
            if (!NT_SUCCESS(st))
                Note(L"Traffic", "a write failed (%#lx)", st);
        }
        while ((r = RxFrame(c, f, sizeof(f), &l, FALSE)) == 1) {
            busy = TRUE;
            if (f[0] == 4) {
                c->EventsIn++;
                if (!RingPut(&c->Events, f + 1, l - 1)) c->Dropped++;
            } else if (f[0] == 2) {
                c->AclIn++;
                if (!RingPut(&c->Acl, f + 1, l - 1)) c->Dropped++;
            }
        }
        if (r < 0) {
            c->Dropped++;
            (void)UartQuiesce(c);
            Wr(c->Uart, GENI_S_CMD, (1u << 27) | 0x80);
        }
        (void)ServiceIbs(c);
        Deliver(c, &c->Events, c->EventQ, HciPacketEvent);
        Deliver(c, &c->Acl, c->AclQ, HciPacketAclData);
        if (NowMs() - lastNote > 30000) {
            lastNote = NowMs();
            Note(L"Traffic", "out: %lu commands, %lu ACL; in: %lu events, %lu ACL; %lu dropped; chip wakes %lu sleeps %lu",
                 c->CmdsOut, c->AclOut, c->EventsIn, c->AclIn, c->Dropped, c->Wakes, c->Sleeps);
        }
        /* while data flows, poll again at once (the 128-byte RX FIFO fills in ~1.4 ms at 921600 baud) */
        if (busy) {
            idle = 0;
            if (KeReadStateEvent(&c->Stop))
                return;
            Us(50);
            continue;
        }
        if (++idle < 200) {
            Us(100);
            if (KeReadStateEvent(&c->Stop))
                return;
            continue;
        }
        {
            PVOID objs[2] = { &c->Stop, &c->Kick };
            LARGE_INTEGER t;
            t.QuadPart = -10000LL;          /* 1 ms */
            if (KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &t, NULL) == STATUS_WAIT_0)
                return;
        }
    }
}

/* ---------- worker ---------- */
static VOID Worker(PVOID Context)
{
    BT_CTX *c = (BT_CTX *)Context;
    ULONG vote = 0, i, rootSave[5] = { 0 }, hwcg = 0;
    BOOLEAN added = FALSE;
    NTSTATUS st;
    const BT_SPEED *want = NULL;

    ExSetTimerResolution(10000, TRUE);
    if (c->StartDelayMs && WaitMs(c, c->StartDelayMs))
        goto out;
    Note(L"Status", "starting (steps 1..%lu)", c->Steps);
    for (i = 0; i < RTL_NUMBER_OF(Speeds); i++)
        if (Speeds[i].Baud == c->Baud)
            want = &Speeds[i];
    if (!want) {
        Note(L"Status", "Baud %lu is not a speed this knows; 115200", c->Baud);
        want = &Speeds[0];
    }
    c->Gcc = Map(GCC_PA, GCC_SIZE);
    c->Tlmm = Map(TLMM_NORTH_PA, TLMM_SIZE);
    c->Uart = Map(SE6_PA, SE6_SIZE);
    if (!c->Gcc || !c->Tlmm || !c->Uart) {
        Note(L"Status", "cannot map GCC/TLMM/SE6; stopped");
        goto out;
    }
    Note(L"Chip", "step 1: GCC vote %#lx SE6 %#lx/%#lx; gpio16-19 %#lx %#lx %#lx %#lx; BT_EN %#lx/%#lx SW_CTRL %#lx/%#lx",
         Rd(c->Gcc, GCC_QUP_VOTE), Rd(c->Gcc, GCC_SE6_CBCR), Rd(c->Gcc, GCC_SE6_CFG),
         Rd(c->Tlmm, TLMM_CFG(16)), Rd(c->Tlmm, TLMM_CFG(17)), Rd(c->Tlmm, TLMM_CFG(18)), Rd(c->Tlmm, TLMM_CFG(19)),
         Rd(c->Tlmm, TLMM_CFG(PIN_BT_EN)), Rd(c->Tlmm, TLMM_IO(PIN_BT_EN)),
         Rd(c->Tlmm, TLMM_CFG(PIN_SW_CTRL)), Rd(c->Tlmm, TLMM_IO(PIN_SW_CTRL)));
    if (c->Steps < 2)
        goto out;

    /* the UART's clocks; a GENI register read without them can stall the interconnect */
    vote = Rd(c->Gcc, GCC_QUP_VOTE);
    hwcg = Rd(c->Gcc, GCC_QUP_S_AHB) & 2u;
    added = (BOOLEAN)!(vote & GCC_SE6_BIT);
    Wr(c->Gcc, GCC_QUP_S_AHB, Rd(c->Gcc, GCC_QUP_S_AHB) & ~2u);
    Wr(c->Gcc, GCC_QUP_VOTE, vote | GCC_QUP_AHB | GCC_SE6_BIT);
    for (i = 0; i < 10000 && !ClocksRunning(c); i++)
        Us(1);
    if (!ClocksRunning(c)) {
        Note(L"Status", "SE6's clocks do not run (%#lx %#lx %#lx); stopped", Rd(c->Gcc, GCC_QUP_M_AHB),
             Rd(c->Gcc, GCC_QUP_S_AHB), Rd(c->Gcc, GCC_SE6_CBCR));
        goto putback;
    }
    if (((Rd(c->Uart, 0x68) >> 8) & 0xffu) != 2u)     /* GENI_FW_REVISION: protocol 2 = UART */
        Note(L"Chip", "step 2: SE6 runs protocol %lu, not UART (2)", (Rd(c->Uart, 0x68) >> 8) & 0xffu);
    Note(L"Chip", "step 2: SE6 status %#lx fw %#lx clk %#lx tx depth %#lx", Rd(c->Uart, GENI_STATUS), Rd(c->Uart, 0x68),
         Rd(c->Uart, 0x48), Rd(c->Uart, GENI_TX_DEPTH));
    if (c->Steps < 3)
        goto putback;
    if (c->Steps == 3) {
        static const UCHAR pattern[8] = { 0x01, 0x03, 0x0c, 0x00, 0x55, 0xaa, 0x7e, 0xff };
        UCHAR got[8];
        ULONG n = 0, t;
        c->Speed = &Speeds[0];
        (void)UartQuiesce(c);
        UartSetup(c);
        Wr(c->Uart, 0x22c, 1);          /* loopback */
        Wr(c->Uart, GENI_S_CMD, (1u << 27) | 0x80);
        c->RawHead = c->RawTail = 0;
        (void)UartTx(c, pattern, sizeof(pattern));
        for (t = 0; t < 20000 && (c->RawHead - c->RawTail) < sizeof(pattern); t++) {
            (void)RxCapture(c);
            Us(1);
        }
        while (n < sizeof(got) && c->RawTail != c->RawHead)
            got[n++] = c->Raw[c->RawTail++ & (RX_RAW - 1)];
        Note(L"Chip", "step 3: loopback %s (%lu of 8 bytes)",
             n == 8 && RtlCompareMemory(got, pattern, 8) == 8 ? "WORKS" : "FAILED", n);
        (void)UartQuiesce(c);
        Wr(c->Uart, 0x22c, 0);
        goto putback;
    }
    for (i = 0; i < 5; i++)
        rootSave[i] = Rd(c->Gcc, GCC_SE6_CMD + 4 * i);

    st = BtPower(c);
    if (!NT_SUCCESS(st))
        goto off;
    Note(L"Status", "rails voted; powering the chip");
    for (i = 0; i < 2; i++) {
        st = ChipOn(c);
        if (!NT_SUCCESS(st) || c->Steps < 5)
            break;
        if (want != c->Speed) {
            st = SetSpeed(c, want);
            if (!NT_SUCCESS(st)) {
                Note(L"Chip", "%lu baud did not work (%#lx): started over at 115200", want->Baud, st);
                want = &Speeds[0];
                continue;
            }
        }
        st = Firmware(c);
        if (!NT_SUCCESS(st) && c->Speed != &Speeds[0]) {
            Note(L"Chip", "firmware at %lu baud failed (%#lx): started over at 115200", c->Speed->Baud, st);
            want = &Speeds[0];
            continue;
        }
        break;
    }
    if (!NT_SUCCESS(st) || c->Steps < 5)
        goto off;

    /* up: the child for BthMini (created only now, so Windows never sees a radio that is not there), then
     * traffic until the driver stops */
    c->Up = TRUE;
    c->Awake = TRUE;
    if (!c->Child) {
        st = CreateChild(c);
        if (!NT_SUCCESS(st)) {
            Note(L"Status", "the Bluetooth child device could not be created (%#lx)", st);
            c->Up = FALSE;
            goto off;
        }
    }
    KeSetEvent(&c->Kick, IO_NO_INCREMENT, FALSE);
    Pump(c);
    c->Up = FALSE;
off:
    if (c->Steps >= 5 && !NT_SUCCESS(st))
        Note(L"Status", "Bluetooth did not start (%#lx): see Chip and Firmware", st);
    else if (c->Steps < 5)
        Note(L"Status", "steps 1..%lu done", c->Steps);
    Gpio(c, PIN_BT_EN, 0x241, FALSE);
    (void)UartQuiesce(c);
    if (c->Steps >= 4) {
        for (i = 0; i < 5; i++)
            Wr(c->Gcc, GCC_SE6_CMD + 4 * i, i == 0 ? (rootSave[0] | 1) : rootSave[i]);
    }
putback:
    if (added)
        Wr(c->Gcc, GCC_QUP_VOTE, Rd(c->Gcc, GCC_QUP_VOTE) & ~GCC_SE6_BIT);
    Wr(c->Gcc, GCC_QUP_S_AHB, (Rd(c->Gcc, GCC_QUP_S_AHB) & ~2u) | hwcg);
    if (c->Steps < 2)
        Note(L"Status", "step 1 done");
out:
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------- the child (BthMini's PDO) ---------- */
static VOID EvtPdoIoctl(WDFQUEUE Queue, WDFREQUEST Req, size_t OutLen, size_t InLen, ULONG Code)
{
    BT_CTX *c = GetPdoCtx(WdfIoQueueGetDevice(Queue))->Bus;
    WDFMEMORY mem;
    PVOID buf;
    size_t size;
    NTSTATUS st = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(InLen);
    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
        if (OutLen >= sizeof(BTHX_VERSION) && NT_SUCCESS(WdfRequestRetrieveOutputMemory(Req, &mem))) {
            buf = WdfMemoryGetBuffer(mem, &size);
            ((PBTHX_VERSION)buf)->Version = BTHX_DDI_VERSION_1;
            WdfRequestCompleteWithInformation(Req, STATUS_SUCCESS, sizeof(BTHX_VERSION));
            return;
        }
        break;
    case IOCTL_BTHX_SET_VERSION:
        st = STATUS_SUCCESS;
        break;
    case IOCTL_BTHX_QUERY_CAPABILITIES:
        if (OutLen >= sizeof(BTHX_CAPABILITIES) && NT_SUCCESS(WdfRequestRetrieveOutputMemory(Req, &mem))) {
            PBTHX_CAPABILITIES caps = (PBTHX_CAPABILITIES)WdfMemoryGetBuffer(mem, &size);
            RtlZeroMemory(caps, sizeof(*caps));
            caps->MaxAclTransferInSize = 4 + ACL_PAYLOAD;
            caps->ScoSupport = ScoSupportNone;
            caps->MaxScoChannels = 0;
            caps->IsDeviceIdleCapable = FALSE;
            caps->IsDeviceWakeCapable = FALSE;
            WdfRequestCompleteWithInformation(Req, STATUS_SUCCESS, sizeof(BTHX_CAPABILITIES));
            return;
        }
        break;
    case IOCTL_BTHX_WRITE_HCI:
        if (!c->Up) { st = STATUS_DEVICE_NOT_READY; break; }
        st = WdfRequestForwardToIoQueue(Req, c->WriteQ);
        if (NT_SUCCESS(st)) { KeSetEvent(&c->Kick, IO_NO_INCREMENT, FALSE); return; }
        break;
    case IOCTL_BTHX_READ_HCI:
        if (NT_SUCCESS(WdfRequestRetrieveInputMemory(Req, &mem))) {
            buf = WdfMemoryGetBuffer(mem, &size);
            if (size >= 1) {
                UCHAR type = *(PUCHAR)buf;
                WDFQUEUE q = type == HciPacketEvent ? c->EventQ : type == HciPacketAclData ? c->AclQ : NULL;
                if (q) {
                    st = WdfRequestForwardToIoQueue(Req, q);
                    if (NT_SUCCESS(st)) { KeSetEvent(&c->Kick, IO_NO_INCREMENT, FALSE); return; }
                }
            }
        }
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    WdfRequestComplete(Req, st);
}

static NTSTATUS CreateChild(BT_CTX *c)
{
    PWDFDEVICE_INIT init;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qc;
    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDFDEVICE child;
    NTSTATUS st;
    DECLARE_CONST_UNICODE_STRING(hwid, L"apollobt\\UART_H4");
    DECLARE_CONST_UNICODE_STRING(compat, L"MS_BTHX_BTHMINI");
    DECLARE_CONST_UNICODE_STRING(inst, L"00");
    DECLARE_CONST_UNICODE_STRING(container, L"{00000000-0000-0000-ffff-ffffffffffff}");
    DECLARE_CONST_UNICODE_STRING(desc, L"apollo Bluetooth (QCA6390 over UART)");
    DECLARE_CONST_UNICODE_STRING(loc, L"apollo SE6 UART");

    init = WdfPdoInitAllocate(c->Device);
    if (!init)
        return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BUS_EXTENDER);
    st = WdfPdoInitAssignDeviceID(init, &hwid);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddHardwareID(init, &hwid);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddCompatibleID(init, &compat);
    if (NT_SUCCESS(st)) st = WdfPdoInitAssignInstanceID(init, &inst);
    if (NT_SUCCESS(st)) st = WdfPdoInitAssignContainerID(init, &container);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddDeviceText(init, &desc, &loc, 0x409);
    if (!NT_SUCCESS(st)) {
        WdfDeviceInitFree(init);
        return st;
    }
    WdfPdoInitSetDefaultLocale(init, 0x409);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PDO_CTX);
    st = WdfDeviceCreate(&init, &attr, &child);
    if (!NT_SUCCESS(st)) {
        WdfDeviceInitFree(init);
        return st;
    }
    GetPdoCtx(child)->Bus = c;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.Removable = WdfFalse;
    caps.SurpriseRemovalOK = WdfTrue;
    caps.UniqueID = WdfTrue;
    WdfDeviceSetPnpCapabilities(child, &caps);
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoDeviceControl = EvtPdoIoctl;
    qc.EvtIoInternalDeviceControl = EvtPdoIoctl;
    st = WdfIoQueueCreate(child, &qc, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(st))
        return st;
    st = WdfFdoAddStaticChild(c->Device, child);
    if (NT_SUCCESS(st))
        c->Child = child;
    return st;
}

/* ---------- the bus device ---------- */
static NTSTATUS EvtIoInit(WDFDEVICE Device)
{
    BT_CTX *c = GetCtx(Device);
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;

    c->Steps = ReadParam(L"Steps", 5, NULL, 0);
    if (c->Steps == 0 || c->Steps > 5)
        c->Steps = 5;
    c->Baud = ReadParam(L"Baud", 921600, NULL, 0);
    c->StartDelayMs = ReadParam(L"StartDelayMs", 0, NULL, 0);
    if (c->StartDelayMs > 120000)
        c->StartDelayMs = 0;
    c->HaveAddress = (BOOLEAN)(ReadParam(L"Address", 0, c->Address, 6) == 1 && !(c->Address[0] & 1) &&
                               (c->Address[0] | c->Address[1] | c->Address[2] | c->Address[3] | c->Address[4] | c->Address[5]));
    if (c->Steps >= 5) {
        BOOLEAN p = LoadFile(FW_PATCH, PATCH_MOST, &c->Patch, &c->PatchSize);
        BOOLEAN n = LoadFile(FW_NVM, NVM_MOST, &c->Nvm, &c->NvmSize);
        if (!p || !n)
            Note(L"Firmware", "missing in System32\\drivers:%s%s", p ? "" : " htbtfw20.tlv", n ? "" : " htnv20.bin");
    }
    KeInitializeEvent(&c->Stop, NotificationEvent, FALSE);
    KeInitializeEvent(&c->Kick, SynchronizationEvent, FALSE);
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
    BT_CTX *c = GetCtx(Device);
    if (c->Thread) {
        KeSetEvent(&c->Stop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(c->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(c->Thread);
        c->Thread = NULL;
    }
    if (c->Gcc) MmUnmapIoSpace(c->Gcc, GCC_SIZE);
    if (c->Tlmm) MmUnmapIoSpace(c->Tlmm, TLMM_SIZE);
    if (c->Uart) MmUnmapIoSpace(c->Uart, SE6_SIZE);
    if (c->Rsc) MmUnmapIoSpace(c->Rsc, 0x100);
    if (c->Tcs) MmUnmapIoSpace(c->Tcs, RH_TCS_SIZE);
    if (c->Db) MmUnmapIoSpace(c->Db, RH_DB_SIZE);
    c->Gcc = c->Tlmm = c->Uart = c->Rsc = c->Tcs = c->Db = NULL;
    if (c->Patch) ExFreePoolWithTag(c->Patch, TAG);
    if (c->Nvm) ExFreePoolWithTag(c->Nvm, TAG);
    c->Patch = c->Nvm = NULL;
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qc;
    WDFDEVICE dev;
    BT_CTX *c;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceSelfManagedIoInit = EvtIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = EvtIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BT_CTX);
    st = WdfDeviceCreate(&Init, &attr, &dev);
    if (!NT_SUCCESS(st))
        return st;
    c = GetCtx(dev);
    c->Device = dev;
    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
    st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &c->WriteQ);
    if (NT_SUCCESS(st)) st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &c->EventQ);
    if (NT_SUCCESS(st)) st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &c->AclQ);
    return st;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
