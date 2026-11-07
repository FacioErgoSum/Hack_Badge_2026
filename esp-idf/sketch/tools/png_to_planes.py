"""Turn a palette PNG into the panel's two 1-bpp planes, as a C header.

    python3 tools/png_to_planes.py ../../data/gallery/EtchASketch.png src/etch_frame.h

Colours are sorted the way lib/large_png.py does it on the MicroPython side:
red if R >= 128 and G, B < 128, black if all three < 128, white otherwise.
Rows the image doesn't reach are filled with FILL_BELOW (red: the gallery image
is 670 rows on a 672-row panel and sits inside a red frame).

The planes go out run-length encoded, (count, byte) pairs over the bw plane and
then the red plane, so a mostly flat picture costs a few kB of flash instead
of 160. Standard library only.
"""

import struct
import sys
import zlib

W, H = 960, 672
STRIDE = W // 8
FILL_BELOW = "red"


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, palette = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, n, 3)]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    if ctype != 3 or interlace or depth not in (1, 2, 4, 8):
        raise SystemExit(f"{path}: want a non-interlaced palette PNG (type {ctype}, depth {depth})")

    raw = zlib.decompress(idat)
    row_bytes = (w * depth + 7) // 8
    bpp = 1
    rows, prev, o = [], bytearray(row_bytes), 0
    for _ in range(h):
        ft, line = raw[o], bytearray(raw[o + 1:o + 1 + row_bytes])
        o += 1 + row_bytes
        for i in range(row_bytes):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append([(line[(x * depth) >> 3] >> (8 - depth - ((x * depth) & 7))) & ((1 << depth) - 1)
                     for x in range(w)])
        prev = line
    return w, h, palette, rows


def kind(rgb):
    r, g, b = rgb
    if r >= 128 and g < 128 and b < 128:
        return "red"
    if r < 128 and g < 128 and b < 128:
        return "black"
    return "white"


def rle(buf):
    out, i = bytearray(), 0
    while i < len(buf):
        v, n = buf[i], 1
        while i + n < len(buf) and buf[i + n] == v and n < 255:
            n += 1
        out += bytes((n, v))
        i += n
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, palette, rows = read_png(src)
    kinds = [kind(c) for c in palette]
    bw, red = bytearray(STRIDE * H), bytearray(STRIDE * H)
    for y in range(H):
        for x in range(W):
            k = kinds[rows[y][x]] if (y < h and x < w) else FILL_BELOW
            bit, i = 0x80 >> (x & 7), y * STRIDE + (x >> 3)
            if k == "black":
                bw[i] |= bit
            elif k == "red":
                red[i] |= bit
    enc = rle(bw + red)
    with open(dst, "w") as f:
        f.write(f"// Generated from {src.split('/')[-1]} by tools/png_to_planes.py - do not edit.\n")
        f.write("// RLE (count, byte) pairs: the bw plane, then the red plane.\n#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static const uint8_t k_etch_frame_rle[{len(enc)}] = {{\n")
        for i in range(0, len(enc), 24):
            f.write("  " + ", ".join(f"0x{b:02X}" for b in enc[i:i + 24]) + ",\n")
        f.write("};\n")
    print(f"{src}: {w}x{h}, {len(palette)} colours -> {len(enc)} bytes RLE")


if __name__ == "__main__":
    main()
