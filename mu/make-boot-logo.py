#!/usr/bin/env python3
"""Make Mu-Silicium's boot logo from the phone's own Xiaomi `logo` partition.

    python3 make-boot-logo.py logo.img BootLogo.bmp            first image (the normal boot splash)
    python3 make-boot-logo.py logo.img BootLogo.bmp --index 1  another image in the partition
    python3 make-boot-logo.py logo.img --list                  list the images found
    add --full to keep the whole screen instead of cropping to the logo

logo.img is a dump of the phone's `logo` partition (see mu/README.md). It holds full-screen BMP splashes
(1080x2400, black background, Mi logo in the middle, small text at the bottom). The logo is cropped out (the
block of non-black rows nearest the middle of the screen) so UEFI and Windows show it centred, the way they
show Silicium's own logo, and written as an uncompressed 24-bit BMP. Xiaomi's image is not committed anywhere:
install-apollo.sh LOGO=<file> copies it into the local Mu-Silicium checkout only.
"""
import struct, sys

THRESH = 24      # a pixel is "content" if any channel is above this
MARGIN = 24      # pixels of black kept around the cropped logo
BLANK_GAP = 40   # rows of black that separate two blocks (logo vs. bottom text)


def find_bmps(d):
    out = []
    pos = d.find(b'BM')
    while pos >= 0:
        if pos + 54 <= len(d):
            fsize, _, off = struct.unpack_from('<IIL', d, pos + 2)
            hsz, w, h, planes, bpp, comp = struct.unpack_from('<IiiHHI', d, pos + 14)
            if (hsz in (40, 108, 124) and planes == 1 and bpp in (24, 32) and comp in (0, 3)
                    and 16 <= w <= 4096 and 16 <= abs(h) <= 4096 and pos + off + (w * bpp // 8) * abs(h) <= len(d)):
                out.append((pos, w, h, bpp, off))
                pos = d.find(b'BM', pos + off + (w * bpp // 8) * abs(h))
                continue
        pos = d.find(b'BM', pos + 1)
    return out


def decode(d, bmp):
    pos, w, h, bpp, off = bmp
    bpr = w * bpp // 8
    stride = (bpr + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src = y if h < 0 else abs(h) - 1 - y          # top-down output
        r = d[pos + off + src * stride: pos + off + src * stride + bpr]
        if bpp == 32:
            r = b''.join(r[i:i + 3] for i in range(0, len(r), 4))
        rows.append(bytes(r))                          # BGR, 3 bytes a pixel
    return w, rows


def crop(w, rows):
    lit = [any(c > THRESH for c in r) for r in rows]
    blocks, start, gap = [], None, 0
    for y, on in enumerate(lit + [False] * (BLANK_GAP + 1)):
        if on:
            if start is None:
                start = y
            gap, end = 0, y
        elif start is not None:
            gap += 1
            if gap > BLANK_GAP:
                blocks.append((start, end))
                start, gap = None, 0
    if not blocks:
        return w, rows
    mid = len(rows) / 2
    y0, y1 = min(blocks, key=lambda b: 0 if b[0] <= mid <= b[1] else min(abs(b[0] - mid), abs(b[1] - mid)))
    xs = [x for r in rows[y0:y1 + 1] for x in range(w) if max(r[3 * x:3 * x + 3]) > THRESH]
    x0, x1 = max(min(xs) - MARGIN, 0), min(max(xs) + MARGIN, w - 1)
    y0, y1 = max(y0 - MARGIN, 0), min(y1 + MARGIN, len(rows) - 1)
    return x1 - x0 + 1, [r[3 * x0:3 * (x1 + 1)] for r in rows[y0:y1 + 1]]


def write_bmp(path, w, rows):
    stride = (3 * w + 3) & ~3
    pad = b'\0' * (stride - 3 * w)
    data = b''.join(r + pad for r in reversed(rows))   # bottom-up
    hdr = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    dib = struct.pack('<IiiHHIIiiII', 40, w, len(rows), 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + dib + data)


def main():
    a = sys.argv[1:]
    full = '--full' in a and not a.remove('--full')
    lst = '--list' in a and not a.remove('--list')
    idx = 0
    if '--index' in a:
        k = a.index('--index'); idx = int(a[k + 1]); del a[k:k + 2]
    if not a or (not lst and len(a) != 2):
        sys.exit(__doc__)
    d = open(a[0], 'rb').read()
    bmps = find_bmps(d)
    if not bmps:
        sys.exit(f'no 24/32-bit BMP images found in {a[0]} (is it the logo partition?)')
    if lst:
        for i, (pos, w, h, bpp, _) in enumerate(bmps):
            print(f'{i}: offset {pos:#x}, {w}x{abs(h)}, {bpp}-bit')
        return
    if idx >= len(bmps):
        sys.exit(f'only {len(bmps)} images (0..{len(bmps) - 1})')
    w, rows = decode(d, bmps[idx])
    if not full:
        w, rows = crop(w, rows)
    write_bmp(a[1], w, rows)
    print(f'image {idx} of {len(bmps)} -> {a[1]}: {w}x{len(rows)}{" (full screen)" if full else " (cropped to the logo)"}')


if __name__ == '__main__':
    main()
