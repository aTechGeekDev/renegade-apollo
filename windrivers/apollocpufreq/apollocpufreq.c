/*
 * apollocpufreq - raise the SM8250 CPU clusters to their top frequency on Windows (Xiaomi apollo).
 *
 * Windows runs on apollo without Qualcomm's PEP (it cannot run on SM8250), so nothing asks the
 * CPU frequency hardware for a level and the cores stay where the firmware left them. SM8250 does
 * CPU DVFS in hardware ("EPSS"): software writes the index of a row of the per-domain frequency
 * table to PERF_STATE and the hardware (with AOP for voltage) switches. Hardware thermal limiting
 * (LMh) still applies on top of the request.
 *
 * Register layout from Linux drivers/cpufreq/qcom-cpufreq-hw.c (EPSS) and sm8250.dtsi
 * (cpufreq@18591000):
 *   domain 0 (CPU0-3) 0x18591000, domain 1 (CPU4-6) 0x18592000, domain 2 (CPU7) 0x18593000
 *   +0x000 ENABLE (bit 0)   +0x100 FREQ_LUT (4-byte rows, up to 40)   +0x320 PERF_STATE
 *   LUT row: bits 7:0 L_VAL, bits 31:30 clock source; source != 0: f = L_VAL * 19.2 MHz,
 *   source 0: the 300 MHz alternate clock. The table ends at the first row repeating the previous one.
 *
 * Optional cap: HKLM\SYSTEM\CurrentControlSet\Services\apollocpufreq\Parameters\MaxMHz (REG_DWORD);
 * the highest row at or below it is chosen. 0 or absent: highest row.
 * Plain legacy kernel driver: does its work in DriverEntry and stays unloadable.
 */
#include <ntddk.h>

#define DOMAINS        3
#define MAP_SIZE       0x1000
#define REG_ENABLE     0x000
#define REG_FREQ_LUT   0x100
#define REG_PERF_STATE 0x320
#define LUT_ROWS       40
#define XO_KHZ         19200
#define ALT_KHZ        300000

static const ULONGLONG DomainPhys[DOMAINS] = { 0x18591000ULL, 0x18592000ULL, 0x18593000ULL };

#define LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "apollocpufreq: " fmt "\n", __VA_ARGS__)

static ULONG ReadMaxMHz(PUNICODE_STRING RegistryPath)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING sub, val;
    HANDLE svc = NULL, par = NULL;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len = 0, mhz = 0;

    InitializeObjectAttributes(&oa, RegistryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&svc, KEY_READ, &oa)))
        return 0;
    RtlInitUnicodeString(&sub, L"Parameters");
    InitializeObjectAttributes(&oa, &sub, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, svc, NULL);
    if (NT_SUCCESS(ZwOpenKey(&par, KEY_READ, &oa))) {
        RtlInitUnicodeString(&val, L"MaxMHz");
        if (NT_SUCCESS(ZwQueryValueKey(par, &val, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
            info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            mhz = *(ULONG *)info->Data;
        ZwClose(par);
    }
    ZwClose(svc);
    return mhz;
}

static VOID SetDomain(ULONG d, ULONG capKhz)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR base;
    ULONG i, rows = 0, best = 0, bestKhz = 0, prev = 0, before;

    pa.QuadPart = (LONGLONG)DomainPhys[d];
    base = (PUCHAR)MmMapIoSpaceEx(pa, MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!base) {
        LOG("domain %lu: cannot map %llx", d, DomainPhys[d]);
        return;
    }
    if (!(READ_REGISTER_ULONG((PULONG)(base + REG_ENABLE)) & 1)) {
        LOG("domain %lu: hardware not enabled, left alone", d);
        goto out;
    }
    for (i = 0; i < LUT_ROWS; i++) {
        ULONG row = READ_REGISTER_ULONG((PULONG)(base + REG_FREQ_LUT + i * 4));
        ULONG khz = (row >> 30) ? (row & 0xFF) * XO_KHZ : ALT_KHZ;
        if (i > 0 && khz == prev)
            break;
        prev = khz;
        rows++;
        if (khz > bestKhz && (capKhz == 0 || khz <= capKhz)) {
            bestKhz = khz;
            best = i;
        }
    }
    if (bestKhz == 0) {
        LOG("domain %lu: no usable table row, left alone", d);
        goto out;
    }
    before = READ_REGISTER_ULONG((PULONG)(base + REG_PERF_STATE));
    WRITE_REGISTER_ULONG((PULONG)(base + REG_PERF_STATE), best);
    LOG("domain %lu: %lu rows, perf state %lu -> %lu (%lu MHz)", d, rows, before, best, bestKhz / 1000);
out:
    MmUnmapIoSpace(base, MAP_SIZE);
}

static VOID Unload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG d, capMhz = ReadMaxMHz(RegistryPath);

    DriverObject->DriverUnload = Unload;
    LOG("start, cap %lu MHz (0 = none)", capMhz);
    for (d = 0; d < DOMAINS; d++)
        SetDomain(d, capMhz * 1000);
    return STATUS_SUCCESS;
}
