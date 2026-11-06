#!/usr/bin/env python3
"""Make qcpep8250.sys skip the LMh cluster registers that hang apollo's boot.

qcpep's AddDevice writes 0x3A to 0x18350800 (LMh, cluster 0). On apollo that write never completes: the CPU
stalls, Windows' boot thread holds the PnP lock forever, nothing starts (see README, "boot hangs in qcpep's
AddDevice"). Three small functions touch the two LMh blocks (0x18350800 / 0x18358800); each becomes `ret`:

  +0x5520  reads  LMh +4 into qcpep's own state (callers ignore the result)
  +0x5b20  writes LMh +8
  +0x5b88  writes LMh +0  (the write that hangs)

Mapping the blocks (MmMapIoSpaceEx) does not touch the hardware and is left alone. LMh stays as the firmware
configured it (hardware throttling still runs; qcpep just does not change it).

Only qcpep8250.sys 1.0.2140.0 from WOA-Project/windows_silicon_qcom_kona is accepted (checked by SHA-256 and by
the instructions being replaced). The output has no signature: test-sign it (sign-install-qcpep.ps1) before use.

  python3 patch-qcpep-lmh.py qcpep8250.sys qcpep8250-lmhfix.sys
"""
import hashlib
import struct
import sys

ORIGINAL_SHA256 = "39fafcd60536c7d36dd95187dc9f51bfc4af661b614bce4a5e9ec0729fe24e01"
IMAGE_BASE = 0x140000000
RET = bytes.fromhex("c0035fd6")                        # ret
PATCHES = {                                            # RVA -> instruction expected there
    0x5520: bytes.fromhex("fd7bbda9"),                 # stp x29, x30, [sp, #-0x30]!
    0x5b20: bytes.fromhex("fd7bbea9"),                 # stp x29, x30, [sp, #-0x20]!
    0x5b88: bytes.fromhex("fd7bbda9"),                 # stp x29, x30, [sp, #-0x30]!
}


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    osz = struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(n):
        o = pe + 24 + osz + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, o + 8)
        yield va, max(vsize, rawsize), raw


def rva_to_offset(d, rva):
    for va, size, raw in sections(d):
        if va <= rva < va + size:
            return raw + rva - va
    raise ValueError("RVA %#x not in any section" % rva)


def pe_checksum(d, csum_off):
    total = 0
    data = d + b"\0" * (len(d) % 2)
    for i in range(0, len(data), 2):
        if csum_off <= i < csum_off + 4:
            continue
        total += struct.unpack_from("<H", data, i)[0]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(d)) & 0xFFFFFFFF


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1:]
    d = bytearray(open(src, "rb").read())
    digest = hashlib.sha256(d).hexdigest()
    if digest != ORIGINAL_SHA256:
        sys.exit("%s: SHA-256 %s is not qcpep8250.sys 1.0.2140.0 (%s); not patching" % (src, digest, ORIGINAL_SHA256))

    for rva, expected in PATCHES.items():
        off = rva_to_offset(d, rva)
        if d[off:off + 4] != expected:
            sys.exit("unexpected instruction at +%#x; not patching" % rva)
        d[off:off + 4] = RET
        print("+%#x (file %#x): ret" % (rva, off))

    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        sys.exit("not a PE32+ image")
    sec_entry = opt + 112 + 8 * 4                      # IMAGE_DIRECTORY_ENTRY_SECURITY
    sec_off, sec_size = struct.unpack_from("<II", d, sec_entry)
    if sec_off:
        if sec_off + sec_size != len(d):
            sys.exit("signature is not at the end of the file; not patching")
        del d[sec_off:]                                # old signature no longer matches
        struct.pack_into("<II", d, sec_entry, 0, 0)
        print("removed the original signature (%d bytes)" % sec_size)

    csum_off = opt + 64
    struct.pack_into("<I", d, csum_off, pe_checksum(bytes(d), csum_off))
    open(dst, "wb").write(d)
    print("wrote %s (unsigned; test-sign it before use)" % dst)


if __name__ == "__main__":
    main()
