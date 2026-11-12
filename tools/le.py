"""LE (Linear Executable) loader for DOS/4GW programs built with Watcom C.

Parses the MZ stub, the LE header, the object table, the page map and the fixup
tables, and produces a flat memory image with every object placed at its
preferred base address and every internal fixup applied.

Usage as a library:
    img = LEImage.load(path)
    img.objects        -> list of LEObject (base, size, flags, data)
    img.fixups         -> list of Fixup (site address, target address, kind)
    img.entry          -> linear address of the program's entry point
    img.stack          -> initial ESP
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

OBJ_READ = 0x0001
OBJ_WRITE = 0x0002
OBJ_EXEC = 0x0004
OBJ_BIG = 0x2000


@dataclass
class LEObject:
    index: int  # 1-based
    vsize: int
    base: int
    flags: int
    page_index: int  # 1-based
    page_count: int
    data: bytearray = field(default_factory=bytearray)

    @property
    def is_code(self) -> bool:
        return bool(self.flags & OBJ_EXEC)

    @property
    def end(self) -> int:
        return self.base + self.vsize


@dataclass
class Fixup:
    site: int        # linear address of the patched bytes
    target: int      # linear address the fixup resolves to (offset part)
    src_type: int    # LE source type (low nibble)
    target_obj: int  # 1-based object index of the target
    size: int        # bytes patched (4 for 32-bit offsets, 2 for 16-bit / selectors)
    is_selector: bool = False
    is_relative: bool = False


class LEImage:
    def __init__(self) -> None:
        self.path = ""
        self.raw = b""
        self.le_off = 0
        self.objects: list[LEObject] = []
        self.fixups: list[Fixup] = []
        self.entry = 0
        self.stack = 0
        self.page_size = 0
        self.last_page_size = 0

    # ------------------------------------------------------------------
    @classmethod
    def load(cls, path: str, bias: int = 0) -> "LEImage":
        """bias: added to every object's preferred base (the image is relocated there)"""
        self = cls()
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        raw = self.raw
        if raw[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        le_off = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[le_off:le_off + 2] not in (b"LE", b"LX"):
            # DOS/4GW bound executables: the stub is followed by the LE image;
            # 0x3C may not point at it, so search.
            le_off = raw.find(b"LE\x00\x00")
            if le_off < 0:
                raise ValueError("LE header not found")
        self.le_off = le_off
        self.is_lx = raw[le_off:le_off + 2] == b"LX"
        h = raw[le_off:le_off + 0xC4]

        def u32(o: int) -> int:
            return struct.unpack_from("<I", h, o)[0]

        self.page_count = u32(0x14)
        eip_obj = u32(0x18)
        eip = u32(0x1C)
        esp_obj = u32(0x20)
        esp = u32(0x24)
        self.page_size = u32(0x28)
        self.last_page_size = u32(0x2C)
        fixup_section_size = u32(0x30)
        obj_tab = u32(0x40) + le_off
        obj_count = u32(0x44)
        page_map = u32(0x48) + le_off
        fixup_page_tab = u32(0x68) + le_off
        fixup_rec_tab = u32(0x6C) + le_off
        data_pages = u32(0x80)  # absolute file offset
        self.fixup_page_tab = fixup_page_tab
        self.fixup_rec_tab = fixup_rec_tab

        for i in range(obj_count):
            vsize, base, flags, pidx, pcnt, _ = struct.unpack_from("<6I", raw, obj_tab + i * 24)
            self.objects.append(LEObject(i + 1, vsize, base + bias, flags, pidx, pcnt))

        # page map: LE uses 4-byte entries (high 3 bytes page number, low byte flags); LX uses 8-byte
        # entries (data offset << shift, size, flags)
        self._page_numbers = []
        self._page_spans = []
        for i in range(self.page_count):
            if self.is_lx:
                off, size, fl = struct.unpack_from("<IHH", raw, page_map + i * 8)
                shift = self.last_page_size
                self._page_spans.append((data_pages + (off << shift), size, fl))
                self._page_numbers.append(i + 1)
            else:
                b = raw[page_map + i * 4:page_map + i * 4 + 4]
                num = (b[0] << 16) | (b[1] << 8) | b[2]
                self._page_numbers.append(num)

        for obj in self.objects:
            buf = bytearray(max(obj.vsize, obj.page_count * self.page_size))
            for p in range(obj.page_count):
                page = obj.page_index - 1 + p  # 0-based logical page
                if self.is_lx:
                    off, size, fl = self._page_spans[page]
                    if fl != 0:
                        raise NotImplementedError(f"LX page flags {fl}")
                else:
                    num = self._page_numbers[page]
                    if num == 0:
                        continue
                    off = data_pages + (num - 1) * self.page_size
                    size = self.last_page_size if page == self.page_count - 1 else self.page_size
                chunk = raw[off:off + size]
                buf[p * self.page_size:p * self.page_size + len(chunk)] = chunk
            obj.data = buf

        self.entry = self.objects[eip_obj - 1].base + eip if eip_obj else 0
        self.stack = self.objects[esp_obj - 1].base + esp if esp_obj else 0
        self.module_flags = u32(0x10)
        self._parse_entries(u32(0x5C) + le_off)
        self._parse_fixups()
        self._apply_fixups()
        return self

    # ------------------------------------------------------------------
    def _obj_of_page(self, page: int) -> LEObject:
        for obj in self.objects:
            if obj.page_index - 1 <= page < obj.page_index - 1 + obj.page_count:
                return obj
        raise ValueError(page)

    def _parse_fixups(self) -> None:
        raw = self.raw
        for page in range(self.page_count):
            start, end = struct.unpack_from("<II", raw, self.fixup_page_tab + page * 4)
            obj = self._obj_of_page(page)
            page_base = obj.base + (page - (obj.page_index - 1)) * self.page_size
            p = self.fixup_rec_tab + start
            q = self.fixup_rec_tab + end
            while p < q:
                src = raw[p]
                flags = raw[p + 1]
                p += 2
                if src & 0x20:
                    count = raw[p]
                    p += 1
                    offsets = None
                else:
                    count = 1
                    srcoff = struct.unpack_from("<h", raw, p)[0]
                    p += 2
                    offsets = [srcoff]
                kind = flags & 3
                if kind != 0:
                    raise NotImplementedError(f"fixup target kind {kind} in {self.path}")
                if flags & 0x40:
                    tobj = struct.unpack_from("<H", raw, p)[0]
                    p += 2
                else:
                    tobj = raw[p]
                    p += 1
                stype = src & 0x0F
                toff = 0
                if stype != 2:  # selector fixups carry no offset
                    if flags & 0x10:
                        toff = struct.unpack_from("<I", raw, p)[0]
                        p += 4
                    else:
                        toff = struct.unpack_from("<H", raw, p)[0]
                        p += 2
                if offsets is None:
                    offsets = list(struct.unpack_from(f"<{count}h", raw, p))
                    p += 2 * count
                target = self.objects[tobj - 1].base + toff
                for so in offsets:
                    site = page_base + so
                    size = {0: 1, 2: 2, 3: 4, 5: 2, 6: 6, 7: 4, 8: 4}.get(stype, 4)
                    self.fixups.append(Fixup(site, target, stype, tobj, size,
                                             is_selector=(stype == 2),
                                             is_relative=(stype == 8)))

    def _parse_entries(self, off: int) -> None:
        """the entry table (exports): ordinal -> linear address; resident names give their names"""
        raw = self.raw
        self.exports: dict[int, int] = {}
        ordinal = 1
        while True:
            cnt = raw[off]
            if cnt == 0:
                break
            typ = raw[off + 1]
            off += 2
            if typ == 0:
                ordinal += cnt
                continue
            obj = struct.unpack_from("<H", raw, off)[0]
            off += 2
            for _ in range(cnt):
                if typ == 3:  # 32-bit entry: flags, offset
                    eoff = struct.unpack_from("<I", raw, off + 1)[0]
                    self.exports[ordinal] = self.objects[obj - 1].base + eoff
                    off += 5
                elif typ == 1:
                    eoff = struct.unpack_from("<H", raw, off + 1)[0]
                    self.exports[ordinal] = self.objects[obj - 1].base + eoff
                    off += 3
                else:
                    raise NotImplementedError(f"entry type {typ}")
                ordinal += 1

    def _apply_fixups(self) -> None:
        for fx in self.fixups:
            obj = self.object_at(fx.site)
            if obj is None:
                continue
            off = fx.site - obj.base
            if fx.src_type == 7:  # 32-bit offset
                if 0 <= off and off + 4 <= len(obj.data):
                    struct.pack_into("<I", obj.data, off, fx.target & 0xFFFFFFFF)
                elif off < 0:  # fixup straddling into the previous page
                    pass
            elif fx.src_type == 8:  # 32-bit self-relative
                struct.pack_into("<I", obj.data, off, (fx.target - (fx.site + 4)) & 0xFFFFFFFF)
            elif fx.src_type == 5:  # 16-bit offset
                struct.pack_into("<H", obj.data, off, fx.target & 0xFFFF)
            elif fx.src_type == 2:  # 16-bit selector: the flat selector, filled at run time
                pass
            else:
                raise NotImplementedError(f"fixup source type {fx.src_type:#x}")

    # ------------------------------------------------------------------
    def object_at(self, addr: int) -> LEObject | None:
        for obj in self.objects:
            if obj.base <= addr < obj.base + len(obj.data):
                return obj
        return None

    def read(self, addr: int, n: int) -> bytes:
        obj = self.object_at(addr)
        if obj is None:
            return b"\x00" * n
        off = addr - obj.base
        return bytes(obj.data[off:off + n])

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def summary(self) -> str:
        lines = [f"{self.path}: entry {self.entry:#x} esp {self.stack:#x} pages {self.page_count} "
                 f"pagesize {self.page_size:#x} fixups {len(self.fixups)}"]
        for o in self.objects:
            kinds = "".join(c for c, b in (("R", OBJ_READ), ("W", OBJ_WRITE), ("X", OBJ_EXEC), ("B", OBJ_BIG))
                            if o.flags & b)
            lines.append(f"  obj{o.index}: base {o.base:#010x} vsize {o.vsize:#x} flags {o.flags:#x} {kinds} "
                         f"pages {o.page_index}+{o.page_count}")
        from collections import Counter
        c = Counter(f.src_type for f in self.fixups)
        lines.append(f"  fixup source types: {dict(c)}")
        return "\n".join(lines)


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        print(LEImage.load(p).summary())
