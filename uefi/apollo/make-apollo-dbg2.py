#!/usr/bin/env python3
"""Check and install Silicium-ACPI's Kona DBG2 table for apollo (kernel debugging over USB).

    python3 make-apollo-dbg2.py Silicium-ACPI/Silicon/Qualcomm/Kona/DBG2.aml Dbg2.aml

The table is used unchanged (Silicium's pipa ships it as is); only its header checksum, stored as 0 in
the source, is fixed. Windows picks the debug port with BCD `busparams` = index into this table. Qualcomm's
layout keeps index 0 for the debug UART, and Microsoft's KDNET-EEM-USB guide uses busparams=1 as the default
USB port:
    busparams=1 / 3 : \\_SB.URS0 (primary USB-C controller, 0x0A600000), two clock-fixup variants
    busparams=2 / 4 : \\_SB.URS1 (second controller)
USB entries are port type 0x8003 (KDNET over USB EEM), subtype 0x5143, served by kd_8003_5143.dll.
The script refuses a table whose layout differs, so the busparams values above stay true.
"""
import struct, sys

HDR = 36
EXPECT = [('\\_SB.UARD', 0x8000), ('\\_SB.URS0', 0x8003), ('\\_SB.URS1', 0x8003),
          ('\\_SB.URS0', 0x8003), ('\\_SB.URS1', 0x8003)]


def entries(t):
    off, count = struct.unpack_from('<II', t, HDR)
    for _ in range(count):
        length = struct.unpack_from('<H', t, off + 1)[0]
        yield t[off:off + length]
        off += length


def describe(e):
    n_len, n_off = struct.unpack_from('<HH', e, 4)
    ptype, psub = struct.unpack_from('<HH', e, 12)
    return e[n_off:n_off + n_len].rstrip(b'\0').decode(), ptype, psub


def main(src, dst):
    t = bytearray(open(src, 'rb').read())
    if t[:4] != b'DBG2' or struct.unpack_from('<I', t, 4)[0] != len(t):
        sys.exit(f'{src} is not a complete DBG2 table')
    got = [describe(e) for e in entries(t)]
    if [(n, p) for n, p, _ in got] != EXPECT or any(s != 0x5143 for n, p, s in got if p == 0x8003):
        sys.exit(f'unexpected DBG2 layout, busparams mapping would be wrong: {got}')
    t[9] = 0
    t[9] = (-sum(t)) & 0xFF
    open(dst, 'wb').write(t)
    print(f'{dst}: {len(t)} bytes, checksum fixed')
    for i, (n, p, s) in enumerate(got):
        print(f'  busparams={i}: {n}  type {p:#06x} subtype {s:#06x}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
