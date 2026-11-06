#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
	/* apollo (Mi 10T / 10T Pro, 8 GB): regions measured on the device from
	 * /proc/iomem + /proc/device-tree/reserved-memory (OrangeFox, 2026-11-06).
	 * Every Conv region lies inside Linux "System RAM"; every no-map
	 * carve-out is Reserv. RAM top kept at 0x200000000 for now (bank 3 above
	 * that, up to 0x280000000, is real but not exposed yet). */
// Below 0xA0000000 Windows only gets 0x98900000-0x9C000000, as in Mu-Silicium's working Kona
// maps (alioth, pipa). Android uses the "Low Free" slices as RAM, but they stay out of Windows' map:
// a pipa build that handed Windows more low memory hit the same INACCESSIBLE_BOOT_DEVICE
// (Mu-Silicium issue #2145) and was fixed by this layout.
	{"Hypervisor",       0x80000000, 0x00600000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, NS_DEVICE},
	{"AOP",              0x80600000, 0x00260000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
	{"AOP CMD DB",       0x80860000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
	{"XBL Log Buffer",   0x80880000, 0x00014000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"Low Free 1",       0x80894000, 0x0006C000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"SMEM",             0x80900000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
	{"Removed Mem",      0x80B00000, 0x05300000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, NS_DEVICE},
	{"Low Free 2",       0x85E00000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"PEP Region",       0x86000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN}, // DSDT PEP0.INTR maps it; must not be Windows RAM
	{"PIL Reserved 1",   0x86200000, 0x0061C000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
	{"Low Free 3",       0x8681C000, 0x000E4000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"PIL Reserved 2",   0x86900000, 0x0BE00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
	{"ADSP RPC",         0x92700000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
	{"Low Free 4",       0x92F00000, 0x05A00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"HLOS 4",           0x98900000, 0x03700000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
	{"Display Reserved", 0x9C000000, 0x02400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
	{"DBI Dump",         0x9E400000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
	{"Low Free 5",       0x9F300000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"SEC Heap",         0x9FF00000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
	{"CPU Vectors",      0x9FF8C000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
	{"MMU PageTables",   0x9FF8D000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
	{"UEFI Stack",       0x9FF90000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
	{"Low Free 6",       0x9FFD0000, 0x00027000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"Log Buffer",       0x9FFF7000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"Info Blk",         0x9FFFF000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"HLOS 7",           0xA0000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
	{"PSTORE",           0xB0000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"Display RamDump",  0xB0400000, 0x01000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
	{"HLOS 8",           0xB1400000, 0x0A700000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
	/* 0xBBB00000 - 0xC0000000: not RAM (gap between DDR banks) */
	{"DXE Heap",         0xC0000000, 0x0E000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
	{"UEFI FD",          0xCE000000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
	{"RAM Partition 1",  0xD0000000, 0x2FC00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
	{"TZ Reserved",      0xFFC00000, 0x00020000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
	{"RAM Partition 2",  0xFFC20000,0x1003E0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

	/* Other memory regions */
	{"IMEM Base",        0x14680000, 0x00040000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
	{"IMEM Cookie Base", 0x146BF000, 0x00001000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

	/* Register regions */
	{"IPC_ROUTER_TOP",   0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"SECURITY CONTROL", 0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"QUPV3_2_GSI",      0x00800000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"QUPV3_0_GSI",      0x00900000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"QUPV3_1_GSI",      0x00A00000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"PRNG_CFG_PRNG",    0x00790000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"CRYPTO0 CRYPTO",   0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"TCSR_TCSR_REGS",   0x01FC0000, 0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"GPU_GMU_CX_BLK",   0x02C7D000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"GPU_CC",           0x02C90000, 0x0000A000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"QUPV3_SSC_GSI",    0x05A00000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"PERIPH_SS",        0x08800000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"USB30_PRIM",       0x0A600000, 0x0011B000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"USB_RUMI",         0x0A720000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"USB30_SEC",        0x0A800000, 0x0011B000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"AOSS",             0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"TLMM_WEST",        0x0F100000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"TLMM_SOUTH",       0x0F500000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"TLMM_NORTH",       0x0F900000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"SMMU",             0x15000000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"APSS_HM",          0x17800000, 0x0d981000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
	{"Terminator", 0, 0, 0, 0, 0, 0, 0}
};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
