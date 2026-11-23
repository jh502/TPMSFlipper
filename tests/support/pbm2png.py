# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn the host renderer's 128 x 64 PBM screens into PNGs that look like the
Flipper's display: black pixels on orange, each pixel drawn as a square
(512 x 256 at the default scale of 4).

    python3 tests/support/pbm2png.py [--scale N] out_dir screen.pbm ...
    python3 tests/support/pbm2png.py [--scale N] --sheet out.png columns screen.pbm ...
"""
import struct, sys, zlib
from pathlib import Path

ORANGE, BLACK = (255, 130, 0), (20, 12, 0)


def png(path, rows, w, h):
    raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def sheet(path, cols, scale, names):
    """All the screens in one picture, cols across, for looking them over."""
    cell_w, cell_h, pad = 128 * scale, 64 * scale, 3 * scale
    n = len(names)
    lines = (n + cols - 1) // cols
    W, H = cols * (cell_w + pad) + pad, lines * (cell_h + pad) + pad
    grid = [[(40, 40, 40)] * W for _ in range(H)]
    for k, name in enumerate(names):
        tok = Path(name).read_text().split()
        bits = ''.join(tok[3:])
        x0, y0 = pad + (k % cols) * (cell_w + pad), pad + (k // cols) * (cell_h + pad)
        for y in range(64):
            row = []
            for x in range(128):
                row += [BLACK if bits[y * 128 + x] == '1' else ORANGE] * scale
            for dy in range(scale):
                grid[y0 + y * scale + dy][x0:x0 + cell_w] = row
    png(path, grid, W, H)


def main(argv):
    scale = 4
    if argv[:1] == ['--scale']:
        scale, argv = int(argv[1]), argv[2:]
    if argv[:1] == ['--sheet']:
        return sheet(argv[1], int(argv[2]), scale, argv[3:])
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    for name in argv[1:]:
        tok = Path(name).read_text().split()
        assert tok[0] == 'P1', name
        w, h = int(tok[1]), int(tok[2])
        bits = ''.join(tok[3:])
        assert len(bits) == w * h, name
        rows = []
        for y in range(h):
            line = []
            for x in range(w):
                line += [BLACK if bits[y * w + x] == '1' else ORANGE] * scale
            rows += [line] * scale
        png(out / (Path(name).stem + '.png'), rows, w * scale, h * scale)


if __name__ == '__main__':
    main(sys.argv[1:])
