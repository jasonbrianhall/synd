"""Pack the original game files into one file the program carries inside it (the private build's built-in
game; src/rt/vfs.c reads it).

    python tools/pack_game.py GAME_DIR OUT_FILE

GAME_DIR is the game folder (SYND.EXE, DATA, SYNDICAT, DATADISK, SYND).  The format, little-endian:
"SYNPAK01", u32 count, `count` entries (u16 path length, the path in upper case with '\\' between
components, u32 offset, u32 size, u16 DOS time, u16 DOS date), then the files' data, each at a multiple
of 16.  The output is only rewritten when it changes, so builds stay incremental.
"""
import os
import struct
import sys
import time


def dos_time_date(mtime):
    t = time.localtime(mtime)
    year = max(t.tm_year, 1980)
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2),
            ((year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = sys.argv[1], sys.argv[2]
    files = []
    for d, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(d, n)
            rel = os.path.relpath(full, root).replace('/', '\\').upper()
            files.append((rel, full))
    files.sort()
    header = bytearray(b'SYNPAK01' + struct.pack('<I', len(files)))
    size = len(header) + sum(2 + len(r.encode('ascii')) + 12 for r, _ in files)
    offset = (size + 15) & ~15
    data = bytearray()
    for rel, full in files:
        with open(full, 'rb') as f:
            body = f.read()
        tm, dt = dos_time_date(os.path.getmtime(full))
        p = rel.encode('ascii')
        header += struct.pack('<H', len(p)) + p + struct.pack('<IIHH', offset + len(data), len(body), tm, dt)
        data += body
        data += bytes((-len(data)) % 16)
    header += bytes(offset - len(header))
    blob = bytes(header + data)
    if os.path.exists(out):
        with open(out, 'rb') as f:
            if f.read() == blob:
                print(f'{out}: unchanged ({len(files)} files)')
                return
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(blob)
    print(f'{out}: {len(files)} files, {len(blob) // 1024} KB')


if __name__ == '__main__':
    main()
