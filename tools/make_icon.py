"""Writes res/xpcode.ico: 32x32 and 16x16, 16 colours, no dependencies."""
import struct, sys

PAL = [(0, 0, 0), (0xCC, 0x7A, 0x00), (0x9E, 0x5A, 0x00), (0xFF, 0xFF, 0xFF), (0xE8, 0xB0, 0x5A)]  # BGR

def image(n):
    px = [[0] * n for _ in range(n)]
    mask = [[1] * n for _ in range(n)]
    r = n // 6
    for y in range(n):
        for x in range(n):
            dx = max(r - x, x - (n - 1 - r), 0)
            dy = max(r - y, y - (n - 1 - r), 0)
            if dx * dx + dy * dy <= r * r:
                mask[y][x] = 0
                px[y][x] = 2 if y >= n - 2 or x >= n - 2 else 1
    s = n / 32.0
    def dot(x, y, c=3):
        for yy in range(int(y * s), max(int(y * s) + 1, int((y + 2.6) * s))):
            for xx in range(int(x * s), max(int(x * s) + 1, int((x + 2.6) * s))):
                if 0 <= xx < n and 0 <= yy < n:
                    px[yy][xx] = c
    for i in range(7):            # the ">" chevron
        dot(7 + i, 8 + i); dot(7 + i, 20 - i)
    for x in range(16, 26):       # the "_" cursor
        dot(x, 21)
    return px, mask

def bmp(n):
    px, mask = image(n)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 4, 0, 0, 0, 0, 16, 0)
    pal = b''.join(struct.pack('<BBBB', *c, 0) for c in PAL + [(0, 0, 0)] * (16 - len(PAL)))
    rows = b''
    for y in reversed(range(n)):
        row = bytearray()
        for x in range(0, n, 2):
            row.append(px[y][x] << 4 | px[y][x + 1])
        while len(row) % 4: row.append(0)
        rows += bytes(row)
    m = b''
    for y in reversed(range(n)):
        row = bytearray((n + 31) // 32 * 4)
        for x in range(n):
            if mask[y][x]: row[x // 8] |= 0x80 >> (x % 8)
        m += bytes(row)
    return hdr + pal + rows + m

imgs = [bmp(32), bmp(16)]
out = struct.pack('<HHH', 0, 1, len(imgs))
off = 6 + 16 * len(imgs)
for n, d in zip((32, 16), imgs):
    out += struct.pack('<BBBBHHII', n, n, 16, 0, 1, 4, len(d), off)
    off += len(d)
out += b''.join(imgs)
open(sys.argv[1] if len(sys.argv) > 1 else 'res/xpcode.ico', 'wb').write(out)
