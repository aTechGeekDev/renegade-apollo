#!/usr/bin/env python3
"""Build apollo's DBG2 table (kernel debugging over USB) from Silicium-ACPI's Kona DBG2.

    python3 make-apollo-dbg2.py Silicium-ACPI/Silicon/Qualcomm/Kona/DBG2.aml Dbg2.aml [--second]

Keeps only the entry for the primary USB controller (\\_SB.URS0, 0x0A600000): port type 0x8003
(KDNET over USB), subtype 0x5143 ("QC"), which Windows serves with kd_8003_5143.dll. The Kona table
also lists a debug UART (\\_SB.UARD, removed from apollo's DSDT) and the second USB controller.
It lists URS0 twice; the copies differ only in Qualcomm's OEM clock-fixup data. The first is used,
--second picks the other if the first does not bring the debug link up.
Every offset inside a DBG2 device entry is relative to that entry, so it is copied unchanged.
"""
import struct, sys

HDR = 36


def entries(t):
    off, count = struct.unpack_from('<II', t, HDR)
    for _ in range(count):
        length = struct.unpack_from('<H', t, off + 1)[0]
        yield t[off:off + length]
        off += length


def namepath(e):
    n_len, n_off = struct.unpack_from('<HH', e, 4)
    return e[n_off:n_off + n_len].rstrip(b'\0').decode()


def main(src, dst, which=0):
    t = open(src, 'rb').read()
    if t[:4] != b'DBG2':
        sys.exit(f'{src} is not a DBG2 table')
    keep = [e for e in entries(t) if namepath(e) == '\\_SB.URS0']
    if len(keep) != 2:
        sys.exit(f'expected two \\_SB.URS0 entries, found {len(keep)} (wrong source table?)')
    e = keep[which]
    ptype, psub = struct.unpack_from('<HH', e, 12)
    if (ptype, psub) != (0x8003, 0x5143):
        sys.exit(f'URS0 entry is type {ptype:#x}/{psub:#x}, expected 0x8003/0x5143')
    body = struct.pack('<II', HDR + 8, 1) + e
    hdr = bytearray(t[:HDR])
    struct.pack_into('<I', hdr, 4, HDR + len(body))   # length
    hdr[9] = 0                                         # checksum, fixed below
    out = bytearray(hdr + body)
    out[9] = (-sum(out)) & 0xFF
    open(dst, 'wb').write(out)
    print(f'{dst}: {len(out)} bytes, 1 entry: \\_SB.URS0 (copy {which + 1}) type {ptype:#06x} subtype {psub:#06x}')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--second']
    if len(args) != 2:
        sys.exit(__doc__)
    main(args[0], args[1], 1 if '--second' in sys.argv else 0)
