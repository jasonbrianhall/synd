"""Static recompiler: 32-bit x86 (Watcom / DOS4GW LE program) -> portable C.

Every guest function becomes one C function operating on the emulated CPU
state `R` (rt/cpu.h) and the guest memory `M`.  Guest calls are C calls
(the return address is still pushed on the guest stack so that stack layouts
stay exact), guest returns are C returns, branches are gotos, jump tables
are switches and indirect calls go through a sorted address table.

Flags are computed only where a later instruction can read them (backward
liveness per function); a compare followed directly by the conditional jump
that consumes it becomes one C comparison.

    python tools/recomp.py <exe> <prefix> <outdir> [--sha1 HEX]
"""
from __future__ import annotations

import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import capstone  # noqa: E402
from capstone import x86  # noqa: E402

from discover import COND_JUMPS, Program  # noqa: E402
from le import LEImage  # noqa: E402

# ---------------------------------------------------------------------------
# flags
FC, FZ, FS, FO, FP, FA = 1, 2, 4, 8, 16, 32
FALL = FC | FZ | FS | FO | FP | FA
FSZP = FZ | FS | FP

COND_USE = {
    "o": FO, "no": FO, "b": FC, "ae": FC, "e": FZ, "ne": FZ, "be": FC | FZ, "a": FC | FZ,
    "s": FS, "ns": FS, "p": FP, "np": FP, "l": FS | FO, "ge": FS | FO, "le": FZ | FS | FO, "g": FZ | FS | FO,
}
COND_ALIAS = {"c": "b", "nc": "ae", "z": "e", "nz": "ne", "nae": "b", "nb": "ae", "na": "be", "nbe": "a",
              "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
SREGS = {"es", "cs", "ss", "ds", "fs", "gs"}

MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
SIGN = {1: "0x80u", 2: "0x8000u", 4: "0x80000000u"}


def cond_of(mnem: str) -> str:
    c = mnem[1:] if mnem.startswith("j") else mnem[3:]  # jcc / setcc
    return COND_ALIAS.get(c, c)


class RecompError(Exception):
    pass


# ---------------------------------------------------------------------------
# DOS/4GW loads programs above the first megabyte; so do we (keeps DOS memory and the VGA window free)
IMAGE_BIAS = 0x003F0000


class Func:
    def __init__(self, entry: int):
        self.entry = entry
        self.addrs: list[int] = []  # instruction addresses in the function, sorted
        self.labels: set[int] = set()
        self.flags_out = False
        self.ds_dyn = False  # loads DS: DS-relative accesses add R.ds_base


class Recompiler:
    def __init__(self, path: str, prefix: str, reloc: bool = False):
        self.path = path
        self.prefix = prefix
        # programs run relocated by IMAGE_BIAS (src/rt/rt.h RT_IMAGE_BIAS); drivers keep their link address
        self.img = LEImage.load(path, 0 if reloc else IMAGE_BIAS)
        # reloc: an image that its host program loads and relocates at run time (an AIL/32 driver).  Every
        # address that comes from a fixup is emitted as "<prefix>_delta + link-time address".
        self.reloc = reloc
        self.RB = f"{prefix}_delta"
        roots = self.ail_driver_roots() if reloc else None
        self.prog = Program(self.img, roots)
        self.prog.run()
        while reloc:
            # pointers stored in the driver's data (callback tables) that lead to plausible code
            more = sorted({f.target for f in self.prog.uncovered_fixups if f.target not in self.prog.func_entries
                           and self.prog.plausible_code(f.target)} - set(roots))
            if not more:
                break
            roots += more
            self.prog = Program(self.img, roots)
            self.prog.run()
        if reloc:
            self.gap_entries = []
        else:
            self.add_gap_functions()
        self.funcs: dict[int, Func] = {}
        self.unhandled: dict[str, int] = {}
        self.cur_func = None

    def ail_driver_roots(self) -> list[int]:
        """AIL/32 driver: the first dword points to a table of (function number, address) ended by -1"""
        base = self.img.objects[0].base
        t = int.from_bytes(self.img.read(base, 4), "little")
        roots = []
        while True:
            fn = int.from_bytes(self.img.read(t, 4), "little")
            if fn == 0xFFFFFFFF:
                break
            roots.append(int.from_bytes(self.img.read(t + 4, 4), "little"))
            t += 8
        return roots

    def fixed(self, site: int) -> bool:
        return self.reloc and site in self.prog.fixup_at

    def rel(self, a: int) -> str:
        """a link-time address as an expression of the run-time address"""
        return f"{self.RB} + 0x{a:x}u" if self.reloc else f"0x{a:x}u"

    # ---- speculative functions in unreached gaps (padding skipped) --------------
    def add_gap_functions(self) -> None:
        p = self.prog
        covered = bytearray(0x1000000)
        for i in p.insns.values():
            for k in range(i.size):
                covered[i.addr + k] = 1
        for a in p.table_slots:
            for k in range(4):
                covered[a + k] = 1
        extra = []
        for o in p.code_objs:
            a = o.base
            end = o.base + o.vsize
            while a < end:
                if covered[a]:
                    a += 1
                    continue
                b = a
                while b < end and not covered[b]:
                    b += 1
                # skip padding (lea eax,[eax+0] / nop / int3 / zero bytes)
                s = a
                while s < b:
                    d = self.img.read(s, 6)
                    if d[:6] == b"\x8d\x80\x00\x00\x00\x00" or d[:6] == b"\x8d\x92\x00\x00\x00\x00":
                        s += 6
                    elif d[:3] in (b"\x8d\x40\x00", b"\x8d\x52\x00", b"\x8d\x49\x00", b"\x8d\x5b\x00") or \
                            d[:3] == b"\x8d\x76\x00" or d[:3] == b"\x8d\x7f\x00":
                        s += 3
                    elif d[:4] == b"\x8d\x54\x22\x00" or d[:4] == b"\x8d\x44\x20\x00":
                        s += 4
                    elif d[:2] in (b"\x8b\xc0", b"\x8b\xd2", b"\x8b\xdb", b"\x8b\xc9", b"\x8b\xff", b"\x8b\xf6"):
                        s += 2
                    elif d[0] in (0x90, 0xCC, 0x00):
                        s += 1
                    else:
                        break
                if b - s >= 8 and self.try_gap(s, b):
                    extra.append(s)
                a = b
        if extra:
            for e in extra:
                p.func_entries.add(e)
            self.gap_entries = extra
            self.reflow(extra)
        else:
            self.gap_entries = []

    def try_gap(self, s: int, e: int) -> bool:
        """decode linearly from s to e; accept when it decodes cleanly and ends in ret/jmp"""
        p = self.prog
        a = s
        last = None
        while a < e:
            data = self.img.read(a, 16)
            try:
                ci = next(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(data, a, 1))
            except StopIteration:
                return False
            if ci.mnemonic in ("arpl", "bound", "into", "aaa", "aas", "daa", "das", "salc", "hlt", "lcall", "ljmp",
                               "insb", "insd", "outsb", "outsd", "les", "lds", "fbld") or ci.mnemonic.startswith("f"):
                return False
            last = ci.mnemonic
            a += ci.size
            if last in ("ret", "jmp") and a >= e - 16:
                return True
        return last in ("ret", "jmp")

    def reflow(self, roots: list[int]) -> None:
        # re-run the discovery walk from the new roots (Program.run's loop, reused)
        p = self.prog
        old_entry = self.img.entry
        saved = list(self.img.fixups)
        # emulate run() from specific roots
        work = list(roots)
        seen = set(p.insns)
        while work:
            a = work.pop()
            while a not in seen:
                if not p.in_code(a):
                    break
                seen.add(a)
                ins = p.decode(a)
                if ins is None:
                    break
                m = ins.mnem
                nxt = a + ins.size
                if m == "call":
                    op = ins.ci.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        ins.call_target = t
                        p.func_entries.add(t)
                        work.append(t)
                    else:
                        ins.indirect = True
                    ins.succ = [nxt]
                    a = nxt
                    continue
                if m in ("ret", "retf", "iretd", "iret"):
                    ins.is_ret = True
                    break
                if m == "jmp":
                    op = ins.ci.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        ins.succ = [t]
                        a = t
                        continue
                    ins.indirect = True
                    tab = p.jump_table(ins)
                    if tab:
                        ins.table = tab
                        ins.succ = list(dict.fromkeys(tab))
                        p.table_targets.update(tab)
                        work.extend(tab)
                    break
                if m in COND_JUMPS:
                    t = ins.ci.operands[0].imm & 0xFFFFFFFF
                    ins.succ = [nxt, t]
                    work.append(t)
                    a = nxt
                    continue
                ins.succ = [nxt]
                a = nxt
        del old_entry, saved

    # ---- function partition -----------------------------------------------------
    def build_funcs(self) -> None:
        p = self.prog
        entries = sorted(e for e in p.func_entries if e in p.insns)
        self.entry_set = set(entries)
        for e in entries:
            f = Func(e)
            seen = set()
            work = [e]
            while work:
                a = work.pop()
                if a in seen:
                    continue
                if a not in p.insns:
                    raise RecompError(f"{e:#x}: flow to undecoded {a:#x}")
                seen.add(a)
                ins = p.insns[a]
                if ins.is_ret:
                    continue
                if ins.mnem == "jmp" and not ins.indirect:
                    t = ins.succ[0]
                    if t in self.entry_set and t != e:
                        continue  # tail call
                    f.labels.add(t)
                    work.append(t)
                    continue
                if ins.mnem == "jmp" and ins.indirect:
                    if ins.table:
                        for t in ins.succ:
                            f.labels.add(t)
                            work.append(t)
                    continue
                if ins.mnem in COND_JUMPS:
                    nxt, t = ins.succ
                    work.append(nxt)
                    if not (t in self.entry_set and t != e):
                        f.labels.add(t)
                        work.append(t)
                    continue
                if ins.mnem == "hlt":
                    continue
                for s in ins.succ:
                    work.append(s)
            f.addrs = sorted(seen)
            for a in f.addrs:
                ci = p.insns[a].ci
                if ci.mnemonic == "lds" or (ci.mnemonic in ("mov", "pop") and ci.operands and
                                            ci.operands[0].type == x86.X86_OP_REG and
                                            ci.reg_name(ci.operands[0].reg) == "ds"):
                    f.ds_dyn = True
            # labels also for fall-through into non-consecutive addresses
            self.funcs[e] = f

    # ---- flag liveness ----------------------------------------------------------
    def insn_flags(self, ins) -> tuple[int, int, bool]:
        """(use, def, kills): `kills` False when the definition is conditional (count may be 0)"""
        m = ins.mnem
        base = m.split()[-1] if m.startswith(("rep", "repne", "repe")) else m
        rep = m != base
        if base in ("add", "sub", "cmp", "neg", "and", "or", "xor", "test", "imul", "mul", "div", "idiv", "bsr",
                    "bsf"):
            return 0, FALL, True
        if base in ("adc", "sbb"):
            return FC, FALL, True
        if base in ("inc", "dec"):
            return 0, FALL & ~FC, True
        if base in ("shl", "sal", "shr", "sar"):
            c = self.shift_count_const(ins)
            if c is None:
                return 0, FALL, False
            if c & 31 == 0:
                return 0, 0, True
            return 0, FALL, True
        if base in ("shld", "shrd"):
            return 0, FALL, False
        if base in ("rol", "ror"):
            c = self.shift_count_const(ins)
            if c is None:
                return 0, FC | FO, False
            if c & 31 == 0:
                return 0, 0, True
            return 0, FC | FO, True
        if base in ("rcl", "rcr"):
            c = self.shift_count_const(ins)
            if c is not None and c & 31 == 0:
                return 0, 0, True
            return FC, FC | FO, c is not None
        if base in ("scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
            if rep:
                return FALL, FALL, False
            return 0, FALL, True
        if base in ("clc", "stc"):
            return 0, FC, True
        if base == "cmc":
            return FC, FC, True
        if base == "sahf":
            return 0, FC | FZ | FS | FP | FA, True
        if base == "lahf":
            return FC | FZ | FS | FP | FA, 0, True
        if base in ("pushfd", "pushf"):
            return FALL, 0, True
        if base in ("popfd", "popf"):
            return 0, FALL, True
        if base.startswith("set"):
            return COND_USE[cond_of(base)], 0, True
        if base.startswith("j") and base in COND_JUMPS and base not in ("jcxz", "jecxz"):
            return COND_USE[cond_of(base)], 0, True
        if base in ("loope", "loopne"):
            return FZ, 0, True
        if base == "int":
            return 0, FALL, True
        if base in ("arpl",):
            return 0, FZ, False
        return 0, 0, True

    def shift_count_const(self, ins):
        ops = ins.ci.operands
        if len(ops) == 1:
            return 1
        if ops[1].type == x86.X86_OP_IMM:
            return ops[1].imm & 0xFF
        return None

    def liveness(self, f: Func) -> dict[int, int]:
        p = self.prog
        live_in: dict[int, int] = {a: 0 for a in f.addrs}
        live_out: dict[int, int] = {a: 0 for a in f.addrs}
        addrset = set(f.addrs)
        changed = True
        while changed:
            changed = False
            for a in reversed(f.addrs):
                ins = p.insns[a]
                out = 0
                m = ins.mnem
                if ins.is_ret:
                    out = FALL if (m == "ret" and f.flags_out) else 0
                elif m == "jmp" and not ins.indirect:
                    t = ins.succ[0]
                    if t in addrset and not (t in self.entry_set and t != f.entry):
                        out = live_in[t]
                    else:
                        tf = self.funcs.get(t)
                        out = FALL if (tf is not None and tf.flags_out) else 0
                elif m == "jmp":
                    out = 0
                    if ins.table:
                        for t in ins.succ:
                            out |= live_in[t]
                elif m in COND_JUMPS:
                    nxt, t = ins.succ
                    out = live_in.get(nxt, 0)
                    if t in addrset and not (t in self.entry_set and t != f.entry):
                        out |= live_in[t]
                    else:
                        tf = self.funcs.get(t)
                        out |= FALL if (tf is not None and tf.flags_out) else 0
                else:
                    for s in ins.succ:
                        out |= live_in.get(s, 0)
                use, d, kills = self.insn_flags(ins)
                if m == "call":
                    tf = self.funcs.get(ins.call_target) if ins.call_target is not None else None
                    if tf is not None and tf.flags_out:
                        use, d, kills = FALL, FALL, False
                    else:
                        use, d, kills = 0, FALL, True
                if kills:
                    inn = use | (out & ~d)
                else:
                    inn = use | out
                if out != live_out[a] or inn != live_in[a]:
                    live_out[a] = out
                    live_in[a] = inn
                    changed = True
        return live_out

    def compute_flags_out(self) -> None:
        # a callee "returns flags" when the instruction after a call to it reads flags first
        changed = True
        self.live = {}
        while changed:
            changed = False
            for f in self.funcs.values():
                lo = self.live[f.entry] = self.liveness(f)
                for a in f.addrs:
                    ins = self.prog.insns[a]
                    if ins.mnem == "call" and ins.call_target is not None:
                        if lo[a] & (FC | FZ | FS | FO):
                            tf = self.funcs.get(ins.call_target)
                            if tf is not None and not tf.flags_out:
                                tf.flags_out = True
                                changed = True

    # ---- C emission -----------------------------------------------------------------
    def fname(self, a: int) -> str:
        return f"{self.prefix}_{a:06x}"

    def emit_all(self, outdir: str, sha1: str, chunk_insns: int = 6000) -> None:
        os.makedirs(outdir, exist_ok=True)
        entries = sorted(self.funcs)
        P = self.prefix
        # header with prototypes
        hdr = [f"/* generated by tools/recomp.py from {os.path.basename(self.path)} - do not edit */",
               "#pragma once", '#include "rt/recomp.h"']
        if self.reloc:
            hdr.append(f"extern uint32_t {P}_delta; /* run-time load address minus link-time address */")
        for e in entries:
            hdr.append(f"void {self.fname(e)}(void);")
        files = []
        cur = []
        n = 0
        idx = 0
        for e in entries:
            f = self.funcs[e]
            cur.append(e)
            n += len(f.addrs)
            if n >= chunk_insns:
                files.append(cur)
                cur, n = [], 0
        if cur:
            files.append(cur)
        names = []
        for k, group in enumerate(files):
            out = [f"/* generated by tools/recomp.py from {os.path.basename(self.path)} - do not edit */",
                   f'#include "{P}.h"', ""]
            for e in group:
                out.extend(self.emit_func(self.funcs[e]))
                out.append("")
            name = f"{P}_{k:02d}.c"
            names.append(name)
            with open(os.path.join(outdir, name), "w", newline="\n") as fh:
                fh.write("\n".join(out))
            idx += 1
        with open(os.path.join(outdir, f"{P}.h"), "w", newline="\n") as fh:
            fh.write("\n".join(hdr) + "\n")
        # module table
        tab = [f"/* generated by tools/recomp.py from {os.path.basename(self.path)} - do not edit */",
               f'#include "{P}.h"', "",
               f"static const RtFunc {P}_funcs[] = {{"]
        for e in entries:
            tab.append(f"    {{0x{e:08x}u, {self.fname(e)}}},")
        tab.append("};")
        tab.append("")
        if self.reloc:
            # the first 8 code bytes of every function, mask 0 over fixup sites: finds where the host loaded us
            o = self.img.objects[0]
            sites = self.prog.fixup_at
            tab.append(f"uint32_t {P}_delta;")
            tab.append(f"static const uint8_t {P}_sig[] = {{")
            for e in entries:
                b = self.img.read(e, 8)
                msk = [0 if any((e + k - j) in sites for j in range(4)) else 0xFF for k in range(8)]
                tab.append("    " + ", ".join(f"0x{c:02x}" for c in b) + ",  " +
                           ", ".join(f"0x{c:02x}" for c in msk) + ",")
            tab.append("};")
            tab.append("")
        tab.append(f"const RtModuleDef {P}_module = {{")
        tab.append(f'    "{os.path.basename(self.path)}", "{sha1}", 0x{self.img.entry:08x}u, 0x{self.img.stack:08x}u,')
        tab.append(f"    {P}_funcs, {len(entries)}u,")
        if self.reloc:
            tab.append(f"    &{P}_delta, 0x{o.base:08x}u, 0x{o.vsize:08x}u, {P}_sig,")
        tab.append("};")
        with open(os.path.join(outdir, f"{P}_table.c"), "w", newline="\n") as fh:
            fh.write("\n".join(tab) + "\n")
        names.append(f"{P}_table.c")
        with open(os.path.join(outdir, f"{P}.cmake"), "w", newline="\n") as fh:
            fh.write(f"set({P.upper()}_SOURCES\n" + "".join(f"    ${{CMAKE_CURRENT_LIST_DIR}}/{n}\n" for n in names) + ")\n")
        if self.unhandled:
            print("unhandled:", self.unhandled, file=sys.stderr)

    # ---- per function -------------------------------------------------------------
    def emit_func(self, f: Func) -> list[str]:
        p = self.prog
        self.cur_func = f
        live = self.live[f.entry]
        out = [f"void {self.fname(f.entry)}(void)", "{"]
        out.append("    uint32_t ea, t0, t1, t2; (void)ea; (void)t0; (void)t1; (void)t2;")
        out.append(f"    RT_ENTER(0x{f.entry:x}u);")
        addrs = f.addrs
        if addrs[0] != f.entry:
            # blocks below the entry (shared epilogues): start at the entry
            out.append(f"    goto L_{f.entry:06x};")
            f.labels.add(f.entry)
        addrset = set(addrs)
        i = 0
        prev_falls = True
        while i < len(addrs):
            a = addrs[i]
            ins = p.insns[a]
            # an instruction reached by fall-through from a non-adjacent predecessor needs a jump
            if i > 0:
                pa = addrs[i - 1]
                pins = p.insns[pa]
                if prev_falls and pa + pins.size != a:
                    nxt = pa + pins.size
                    out.append(f"    goto L_{nxt:06x};")
                    f.labels.add(nxt)
            lines, consumed = self.emit_insn(f, ins, live, addrs, i, addrset)
            if a in f.labels or a == f.entry and False:
                out.append(f"L_{a:06x}:;")
            out.append(f"    /* {a:06x}: {ins.mnem} {ins.op_str} */")
            out.extend("    " + ln for ln in lines)
            i += consumed
            last = p.insns[addrs[i - 1]]
            prev_falls = not (last.is_ret or last.mnem == "jmp" or last.mnem == "hlt")
        if prev_falls:
            pa = addrs[-1]
            nxt = pa + p.insns[pa].size
            out.append(f"    goto L_{nxt:06x};")
            f.labels.add(nxt)
        out.append("}")
        # labels that were added late (fall-through gotos) must exist: re-emit if missing
        text = "\n".join(out)
        for lab in sorted(f.labels):
            if f"L_{lab:06x}:;" not in text:
                if lab not in addrset:
                    raise RecompError(f"{f.entry:#x}: label {lab:#x} outside function")
        # insert missing label definitions
        res = []
        for ln in out:
            if ln.startswith("    /* ") and ln[7:13].strip():
                try:
                    a = int(ln[7:13], 16)
                except ValueError:
                    a = None
                if a is not None and a in f.labels and (not res or res[-1] != f"L_{a:06x}:;"):
                    res.append(f"L_{a:06x}:;")
            res.append(ln)
        return res

    # ---- operands -------------------------------------------------------------------
    def reg_read(self, name: str) -> str:
        if name in REG32:
            return f"R.{name}"
        if name in REG16:
            return f"(uint16_t)R.{REG16[name]}"
        if name in REG8L:
            return f"(uint8_t)R.{REG8L[name]}"
        if name in REG8H:
            return f"(uint8_t)(R.{REG8H[name]} >> 8)"
        if name in SREGS:
            return f"R.{name}"
        raise RecompError(f"register {name}")

    def reg_write(self, name: str, v: str) -> str:
        if name in REG32:
            return f"R.{name} = {v};"
        if name in REG16:
            r = REG16[name]
            return f"R.{r} = (R.{r} & 0xFFFF0000u) | (uint16_t)({v});"
        if name in REG8L:
            r = REG8L[name]
            return f"R.{r} = (R.{r} & 0xFFFFFF00u) | (uint8_t)({v});"
        if name in REG8H:
            r = REG8H[name]
            return f"R.{r} = (R.{r} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({v}) << 8);"
        if name in SREGS:
            return f"rt_load_seg(SEG_{name.upper()}, (uint16_t)({v}));"
        raise RecompError(f"register {name}")

    def ea_expr(self, ins, op, with_seg: bool = True) -> str:
        m = op.mem
        cs = ins.ci
        parts = []
        if m.base:
            parts.append(f"R.{cs.reg_name(m.base)}")
        if m.index:
            r = f"R.{cs.reg_name(m.index)}"
            parts.append(r if m.scale == 1 else f"{r} * {m.scale}u")
        disp = m.disp & 0xFFFFFFFF
        if cs.disp_offset and self.fixed(ins.addr + cs.disp_offset):
            parts.append(f"{self.RB} + 0x{disp:x}u")
        elif disp or not parts:
            parts.append(f"0x{disp:x}u")
        if cs.prefix[3] == 0x67:
            raise RecompError(f"{ins.addr:#x}: 16-bit addressing")
        e = " + ".join(parts)
        if not with_seg:
            return e
        seg = cs.reg_name(m.segment) if m.segment else None
        if seg in ("es", "fs", "gs"):
            e = f"R.{seg}_base + {e}"
        elif self.cur_func is not None and self.cur_func.ds_dyn and (seg == "ds" or (
                seg is None and cs.reg_name(m.base) not in ("esp", "ebp"))):
            e = f"R.ds_base + {e}"
        return e

    def opsize(self, op) -> int:
        return op.size

    def read_op(self, ins, op, ea="ea") -> str:
        if op.type == x86.X86_OP_REG:
            return self.reg_read(ins.ci.reg_name(op.reg))
        if op.type == x86.X86_OP_IMM:
            if op.size == 4 and ins.ci.imm_offset and self.fixed(ins.addr + ins.ci.imm_offset):
                return f"({self.RB} + 0x{op.imm & 0xFFFFFFFF:x}u)"
            return f"0x{op.imm & int(MASK[op.size][:-1], 16):x}u"
        if op.type == x86.X86_OP_MEM:
            return f"RD{op.size * 8}({ea})"
        raise RecompError("operand")

    def write_op(self, ins, op, v: str, ea="ea") -> str:
        if op.type == x86.X86_OP_REG:
            return self.reg_write(ins.ci.reg_name(op.reg), v)
        if op.type == x86.X86_OP_MEM:
            return f"WR{op.size * 8}({ea}, {v});"
        raise RecompError("write operand")

    def need_ea(self, ins) -> list[str]:
        for op in ins.ci.operands:
            if op.type == x86.X86_OP_MEM:
                return [f"ea = {self.ea_expr(ins, op)};"]
        return []

    # ---- condition expressions -------------------------------------------------------
    def flag_cond(self, c: str) -> str:
        return {
            "o": "R.of", "no": "!R.of", "b": "R.cf", "ae": "!R.cf", "e": "R.zf", "ne": "!R.zf",
            "be": "(R.cf || R.zf)", "a": "(!R.cf && !R.zf)", "s": "R.sf", "ns": "!R.sf", "p": "R.pf",
            "np": "!R.pf", "l": "(R.sf != R.of)", "ge": "(R.sf == R.of)", "le": "(R.zf || R.sf != R.of)",
            "g": "(!R.zf && R.sf == R.of)",
        }[c]

    def fused_cond(self, kind: str, sz: int, c: str) -> str | None:
        """condition from fa/fb/fr locals (t0=a, t1=b, t2=r) for the producing op kind"""
        u = UT[sz]
        s = ST[sz]
        a, b, r = f"({u})t0", f"({u})t1", f"({u})t2"
        sa, sb, sr = f"({s})t0", f"({s})t1", f"({s})t2"
        if kind == "sub":
            return {"e": f"{a} == {b}", "ne": f"{a} != {b}", "b": f"{a} < {b}", "ae": f"{a} >= {b}",
                    "be": f"{a} <= {b}", "a": f"{a} > {b}", "l": f"{sa} < {sb}", "ge": f"{sa} >= {sb}",
                    "le": f"{sa} <= {sb}", "g": f"{sa} > {sb}", "s": f"{sr} < 0", "ns": f"{sr} >= 0"}.get(c)
        if kind == "logic":
            return {"e": f"{r} == 0", "ne": f"{r} != 0", "s": f"{sr} < 0", "ns": f"{sr} >= 0",
                    "b": "0", "ae": "1", "be": f"{r} == 0", "a": f"{r} != 0", "l": f"{sr} < 0",
                    "ge": f"{sr} >= 0", "le": f"{sr} <= 0", "g": f"{sr} > 0", "o": "0", "no": "1"}.get(c)
        if kind == "res":  # inc/dec/add/neg...: only Z and S from the result
            return {"e": f"{r} == 0", "ne": f"{r} != 0", "s": f"{sr} < 0", "ns": f"{sr} >= 0"}.get(c)
        return None

    # ---- instruction emission ---------------------------------------------------------
    def emit_insn(self, f: Func, ins, live, addrs, i, addrset) -> tuple[list[str], int]:
        m = ins.mnem
        ops = ins.ci.operands
        lo = live[ins.addr]
        L: list[str] = []
        poll = ""

        def target_code(t: int) -> list[str]:
            if t in addrset and not (t in self.entry_set and t != f.entry):
                pre = ["POLL();"] if t <= ins.addr else []
                return pre + [f"goto L_{t:06x};"]
            # tail call into another function
            return [f"{self.fname(t)}(); return;"] if t in self.funcs else [f"rt_call_ind({self.rel(t)}); return;"]

        # fused compare + conditional jump / setcc
        nxt_i = i + 1
        if m in ("cmp", "test", "sub", "and", "or", "xor", "inc", "dec", "add") and nxt_i < len(addrs):
            nins = self.prog.insns[addrs[nxt_i]]
            if addrs[nxt_i] == ins.addr + ins.size and addrs[nxt_i] not in f.labels and \
                    ((nins.mnem in COND_JUMPS and nins.mnem not in ("jcxz", "jecxz", "loop", "loope", "loopne"))
                     or nins.mnem.startswith("set")):
                c = cond_of(nins.mnem)
                kind = {"cmp": "sub", "sub": "sub", "test": "logic", "and": "logic", "or": "logic",
                        "xor": "logic", "inc": "res", "dec": "res", "add": "res"}[m]
                _, d, _ = self.insn_flags(ins)
                sz = ops[0].size
                cond = self.fused_cond(kind, sz, c)
                if cond is not None and (live[nins.addr] & d) == 0 and \
                        not (m in ("inc", "dec") and COND_USE[c] & FC):
                    body = self.alu_fused(ins, m, kind)
                    if nins.mnem.startswith("set"):
                        body.append(self.write_op(nins, nins.ci.operands[0], f"({cond}) ? 1u : 0u",
                                                  ea="t1" if False else "ea"))
                        if nins.ci.operands[0].type == x86.X86_OP_MEM:
                            body.insert(len(body) - 1, f"ea = {self.ea_expr(nins, nins.ci.operands[0])};")
                        return body, 2
                    t = nins.ci.operands[0].imm & 0xFFFFFFFF
                    body.append(f"if ({cond}) {{ {' '.join(target_code(t))} }}")
                    return body, 2

        need = lo  # flags needed after this instruction
        L.extend(self.emit_one(f, ins, need, target_code))
        return L, 1

    def alu_fused(self, ins, m, kind) -> list[str]:
        """compute operands into t0/t1, result into t2, write the result back unless cmp/test"""
        ops = ins.ci.operands
        L = self.need_ea(ins)
        sz = ops[0].size
        a = self.read_op(ins, ops[0])
        if m in ("inc", "dec"):
            b = "1u"
        else:
            b = self.read_op(ins, ops[1])
        L.append(f"t0 = {a}; t1 = {b};")
        op = {"cmp": "-", "sub": "-", "test": "&", "and": "&", "or": "|", "xor": "^", "inc": "+", "dec": "-",
              "add": "+"}[m]
        L.append(f"t2 = ({UT[sz]})(t0 {op} t1);")
        if m not in ("cmp", "test"):
            L.append(self.write_op(ins, ops[0], f"t2"))
        return L

    def push_before_ret(self, ins) -> bool:
        """true when the straight-line code before this ret pushed a value it never popped"""
        insns = self.prog.insns
        cur = ins.addr
        for _ in range(32):
            prev = None
            for k in range(1, 16):
                c = insns.get(cur - k)
                if c is not None and c.addr + c.size == cur:
                    prev = c
                    break
            if prev is None:
                return False
            m = prev.mnem
            if m.startswith("push"):
                return m in ("push",)
            if m.startswith("pop") or m in ("call", "ret", "jmp", "leave", "enter", "iretd", "retf") or m.startswith("j")                     or m.startswith("loop"):
                return False
            if "esp" in prev.op_str.split(",")[0]:
                return False
            cur = prev.addr
        return False

    def emit_one(self, f: Func, ins, need: int, target_code) -> list[str]:
        m = ins.mnem
        ops = ins.ci.operands
        cs = ins.ci
        L: list[str] = []
        nf = need & FALL
        rep = ""
        base = m
        if m.startswith(("rep ", "repe ", "repne ", "repz ", "repnz ")):
            rep, base = m.split()
        prefix0 = cs.prefix[0]
        if prefix0 == 0:
            # capstone drops the F2 prefix of "F2 A5" (repne movsd, ambiguous with SSE2 movsd): scan the raw bytes
            for pb in cs.bytes:
                if pb in (0xF2, 0xF3):
                    prefix0 = pb
                    break
                if pb not in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67):
                    break
        if prefix0 in (0xF2, 0xF3) and not rep and base in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                                                             "lodsb", "lodsw", "lodsd", "scasb", "scasw",
                                                             "scasd", "cmpsb", "cmpsw", "cmpsd", "outsb", "outsw",
                                                             "outsd", "insb", "insw", "insd"):
            rep = "repne" if prefix0 == 0xF2 else "rep"

        def sz0():
            return ops[0].size

        # ---------------- moves
        if base == "mov":
            L += self.need_ea(ins)
            L.append(self.write_op(ins, ops[0], self.read_op(ins, ops[1])))
            return L
        if base in ("movzx", "movsx"):
            L += self.need_ea(ins)
            v = self.read_op(ins, ops[1])
            if base == "movsx":
                v = f"({UT[ops[0].size]})({ST[ops[1].size]}){v}"
            L.append(self.write_op(ins, ops[0], v))
            return L
        if base == "lea":
            e = self.ea_expr(ins, ops[1], with_seg=False)
            L.append(self.write_op(ins, ops[0], f"({e})"))
            return L
        if base == "xchg":
            L += self.need_ea(ins)
            a = self.read_op(ins, ops[0])
            b = self.read_op(ins, ops[1])
            L.append(f"t0 = {a}; t1 = {b};")
            L.append(self.write_op(ins, ops[0], "t1"))
            L.append(self.write_op(ins, ops[1], "t0"))
            return L
        if base == "cwde":
            return ["R.eax = (uint32_t)(int32_t)(int16_t)R.eax;"]
        if base == "cbw":
            return ["R.eax = (R.eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)R.eax;"]
        if base == "cdq":
            return ["R.edx = ((int32_t)R.eax < 0) ? 0xFFFFFFFFu : 0u;"]
        if base == "xlatb":
            seg = "R.ds_base + " if f.ds_dyn else ""
            return [f"R.eax = (R.eax & 0xFFFFFF00u) | RD8({seg}R.ebx + (uint8_t)R.eax);"]
        if base in ("shld", "shrd"):
            if ops[0].size != 4:
                raise RecompError(f"{ins.addr:#x}: 16-bit {base}")
            L += self.need_ea(ins)
            a = self.read_op(ins, ops[0])
            b = self.read_op(ins, ops[1])
            c = self.read_op(ins, ops[2])
            L.append(self.write_op(ins, ops[0], f"rt_{base}32({a}, {b}, {c}, 0x{nf:x})"))
            return L
        if base == "cwd":
            return ["R.edx = (R.edx & 0xFFFF0000u) | (((int16_t)R.eax < 0) ? 0xFFFFu : 0u);"]

        # ---------------- stack
        if base == "push":
            op = ops[0]
            if op.type == x86.X86_OP_REG and cs.reg_name(op.reg) in SREGS:
                return [f"PUSH32((uint32_t)R.{cs.reg_name(op.reg)});"]
            L += self.need_ea(ins)
            v = self.read_op(ins, op)
            if op.size == 2:
                L.append(f"PUSH16({v});")
            else:
                L.append(f"PUSH32({v});")
            return L
        if base == "pop":
            op = ops[0]
            if op.type == x86.X86_OP_REG and cs.reg_name(op.reg) in SREGS:
                return [f"t0 = POP32(); rt_load_seg(SEG_{cs.reg_name(op.reg).upper()}, (uint16_t)t0);"]
            if op.size == 2:
                L.append("t0 = POP16();")
            else:
                L.append("t0 = POP32();")
            if op.type == x86.X86_OP_MEM:
                L += self.need_ea(ins)  # esp-relative addressing uses the incremented esp
            L.append(self.write_op(ins, op, "t0"))
            return L
        if base == "pushal":
            return ["t0 = R.esp; PUSH32(R.eax); PUSH32(R.ecx); PUSH32(R.edx); PUSH32(R.ebx); PUSH32(t0); "
                    "PUSH32(R.ebp); PUSH32(R.esi); PUSH32(R.edi);"]
        if base == "popal":
            return ["R.edi = POP32(); R.esi = POP32(); R.ebp = POP32(); (void)POP32(); R.ebx = POP32(); "
                    "R.edx = POP32(); R.ecx = POP32(); R.eax = POP32();"]
        if base in ("pushfd", "pushf"):
            return ["PUSH32(rt_get_eflags());"] if base == "pushfd" else ["PUSH16((uint16_t)rt_get_eflags());"]
        if base in ("popfd", "popf"):
            return ["rt_set_eflags(POP32());"] if base == "popfd" else \
                ["rt_set_eflags((rt_get_eflags() & 0xFFFF0000u) | POP16());"]
        if base == "leave":
            return ["R.esp = R.ebp; R.ebp = POP32();"]
        if base == "enter":
            size = ops[0].imm & 0xFFFF
            level = ops[1].imm & 0x1F
            if level:
                raise RecompError(f"{ins.addr:#x}: enter level {level}")
            return [f"PUSH32(R.ebp); R.ebp = R.esp; R.esp -= {size}u;"]

        # ---------------- far pointers / segments
        if base in ("lgs", "lfs", "les", "lds", "lss"):
            seg = base[1:]
            L += self.need_ea(ins)
            L.append(f"t0 = RD32(ea); t1 = RD16(ea + 4u);")
            L.append(f"rt_load_seg(SEG_{seg.upper()}, (uint16_t)t1);")
            L.append(self.reg_write(cs.reg_name(ops[0].reg), "t0"))
            return L

        # ---------------- ALU
        if base in ("add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"):
            L += self.need_ea(ins)
            sz = sz0()
            a = self.read_op(ins, ops[0])
            b = self.read_op(ins, ops[1])
            fn = {"add": "ADD", "sub": "SUB", "adc": "ADC", "sbb": "SBB", "and": "AND", "or": "OR", "xor": "XOR",
                  "cmp": "SUB", "test": "AND"}[base]
            # xor r,r / sub r,r idiom
            if base in ("xor", "sub") and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_REG and \
                    ops[0].reg == ops[1].reg:
                L.append(self.write_op(ins, ops[0], "0u"))
                if nf:
                    L.append(f"rt_flags_logic{sz * 8}(0u, 0x{nf:x});")
                return L
            expr = f"{fn}{sz * 8}({a}, {b}, 0x{nf:x})"
            if base in ("cmp", "test"):
                if nf:
                    L.append(f"(void){expr};")
                return L
            if nf == 0 and base not in ("adc", "sbb"):
                op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[base]
                L.append(self.write_op(ins, ops[0], f"({UT[sz]})({a} {op} {b})"))
                return L
            L.append(self.write_op(ins, ops[0], expr))
            return L
        if base in ("inc", "dec"):
            L += self.need_ea(ins)
            sz = sz0()
            a = self.read_op(ins, ops[0])
            if nf:
                L.append(self.write_op(ins, ops[0], f"{base.upper()}{sz * 8}({a}, 0x{nf:x})"))
            else:
                L.append(self.write_op(ins, ops[0], f"({UT[sz]})({a} {'+' if base == 'inc' else '-'} 1u)"))
            return L
        if base == "neg":
            L += self.need_ea(ins)
            sz = sz0()
            a = self.read_op(ins, ops[0])
            L.append(self.write_op(ins, ops[0], f"SUB{sz * 8}(0u, {a}, 0x{nf:x})"))
            return L
        if base == "not":
            L += self.need_ea(ins)
            sz = sz0()
            a = self.read_op(ins, ops[0])
            L.append(self.write_op(ins, ops[0], f"({UT[sz]})~{a}"))
            return L
        if base in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            L += self.need_ea(ins)
            sz = sz0()
            a = self.read_op(ins, ops[0])
            c = "1u" if len(ops) == 1 else self.read_op(ins, ops[1])
            fn = {"shl": "SHL", "sal": "SHL", "shr": "SHR", "sar": "SAR", "rol": "ROL", "ror": "ROR", "rcl": "RCL",
                  "rcr": "RCR"}[base]
            L.append(self.write_op(ins, ops[0], f"{fn}{sz * 8}({a}, {c}, 0x{nf:x})"))
            return L
        if base == "imul":
            if len(ops) == 1:
                sz = sz0()
                L += self.need_ea(ins)
                L.append(f"IMUL1_{sz * 8}({self.read_op(ins, ops[0])}, 0x{nf:x});")
                return L
            L += self.need_ea(ins)
            sz = sz0()
            if len(ops) == 2:
                a, b = self.read_op(ins, ops[0]), self.read_op(ins, ops[1])
            else:
                a, b = self.read_op(ins, ops[1]), self.read_op(ins, ops[2])
            L.append(self.write_op(ins, ops[0], f"IMUL{sz * 8}({a}, {b}, 0x{nf:x})"))
            return L
        if base in ("mul", "div", "idiv"):
            L += self.need_ea(ins)
            sz = sz0()
            L.append(f"{base.upper()}{sz * 8}({self.read_op(ins, ops[0])}, 0x{nf:x});")
            return L
        if base == "bsr":
            L += self.need_ea(ins)
            sz = sz0()
            L.append(f"t0 = {self.read_op(ins, ops[1])};")
            L.append(f"if (t0) {{ {self.write_op(ins, ops[0], 'rt_bsr(t0)')} R.zf = 0; }} else R.zf = 1;")
            return L

        # ---------------- flags
        if base == "clc":
            return ["R.cf = 0;"]
        if base == "stc":
            return ["R.cf = 1;"]
        if base == "cmc":
            return ["R.cf ^= 1;"]
        if base == "cld":
            return ["R.df = 0;"]
        if base == "std":
            return ["R.df = 1;"]
        if base == "cli":
            return ["rt_cli();"]
        if base == "sti":
            return ["rt_sti();"]
        if base == "lahf":
            return ["R.eax = (R.eax & 0xFFFF00FFu) | ((rt_get_eflags() & 0xFFu) << 8);"]
        if base == "sahf":
            return ["rt_set_eflags((rt_get_eflags() & 0xFFFFFF00u) | ((R.eax >> 8) & 0xD5u));"]
        if base.startswith("set"):
            L += self.need_ea(ins)
            L.append(self.write_op(ins, ops[0], f"{self.flag_cond(cond_of(base))} ? 1u : 0u"))
            return L

        # ---------------- control flow
        if base == "jmp":
            if not ins.indirect:
                return target_code(ins.succ[0])
            L += self.need_ea(ins)
            v = self.read_op(ins, ops[0])
            if ins.table:
                L.append(f"t0 = {v};")
                L.append(f"switch (t0 - {self.RB}) {{" if self.reloc else "switch (t0) {")
                for t in ins.succ:
                    if t in self.entry_set and t != f.entry and False:
                        L.append(f"case 0x{t:x}u: {self.fname(t)}(); return;")
                    else:
                        L.append(f"case 0x{t:x}u: goto L_{t:06x};")
                L.append("default: rt_call_ind(t0); return;")
                L.append("}")
                return L
            L.append(f"rt_call_ind({v}); return;")
            return L
        if base in COND_JUMPS:
            t = ops[0].imm & 0xFFFFFFFF
            tc = " ".join(target_code(t))
            if base == "jcxz":
                return [f"if ((uint16_t)R.ecx == 0) {{ {tc} }}"]
            if base == "jecxz":
                return [f"if (R.ecx == 0) {{ {tc} }}"]
            if base == "loop":
                if cs.prefix[3] == 0x67:
                    return [f"R.ecx = (R.ecx & 0xFFFF0000u) | (uint16_t)(R.ecx - 1u); "
                            f"if ((uint16_t)R.ecx != 0) {{ {tc} }}"]
                return [f"if (--R.ecx != 0) {{ {tc} }}"]
            if base == "loope":
                return [f"if (--R.ecx != 0 && R.zf) {{ {tc} }}"]
            if base == "loopne":
                return [f"if (--R.ecx != 0 && !R.zf) {{ {tc} }}"]
            return [f"if ({self.flag_cond(cond_of(base))}) {{ {tc} }}"]
        if base == "call":
            ret = ins.addr + ins.size
            if not ins.indirect:
                t = ins.call_target
                if t in self.funcs:
                    return [f"PUSH32({self.rel(ret)}); {self.fname(t)}();"]
                return [f"PUSH32({self.rel(ret)}); rt_call_ind({self.rel(t)});"]
            L += self.need_ea(ins)
            v = self.read_op(ins, ops[0])
            L.append(f"t0 = {v}; PUSH32({self.rel(ret)}); rt_call_ind(t0);")
            return L
        if base == "ret":
            n = ops[0].imm if ops else 0
            if n == 0 and self.push_before_ret(ins):
                # "push target ... ret": a computed jump (e.g. into a table of "int N; ret" stubs)
                return [f"t0 = POP32(); RT_LEAVE(0x{ins.addr:x}u); rt_jump_ind(t0); return;"]
            return [f"RT_LEAVE(0x{ins.addr:x}u); R.esp += {4 + n}u; return;"]
        if base == "retf":
            n = ops[0].imm if ops else 0
            return [f"rt_retf({n}u); return;"]
        if base in ("iretd", "iret"):
            return ["rt_iret(); return;"]
        if base == "int":
            return [f"rt_int(0x{ops[0].imm & 0xFF:x}u);"]
        if base == "hlt":
            return ["rt_hlt(); return;"]
        if base == "nop":
            return []

        # ---------------- I/O
        if base == "in":
            sz = ops[0].size
            port = self.read_op(ins, ops[1]) if ops[1].type == x86.X86_OP_IMM else "(uint16_t)R.edx"
            return [self.write_op(ins, ops[0], f"rt_in{sz * 8}({port})")]
        if base == "out":
            sz = ops[1].size
            port = self.read_op(ins, ops[0]) if ops[0].type == x86.X86_OP_IMM else "(uint16_t)R.edx"
            return [f"rt_out{sz * 8}({port}, {self.read_op(ins, ops[1])});"]

        # ---------------- strings
        if base in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                    "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd", "outsb", "outsw", "outsd",
                    "insb", "insw", "insd"):
            # segment override on the source (ds:esi) side
            seg = "R.ds_base" if f.ds_dyn else "0u"
            for op in ops:
                if op.type == x86.X86_OP_MEM and op.mem.segment and cs.reg_name(op.mem.base) == "esi":
                    sname = cs.reg_name(op.mem.segment)
                    if sname in ("es", "fs", "gs"):
                        seg = f"R.{sname}_base"
            kind = {"": "", "rep": "REP_", "repe": "REPE_", "repz": "REPE_", "repne": "REPNE_",
                    "repnz": "REPNE_"}[rep]
            if kind == "REPNE_" and base[:4] not in ("scas", "cmps"):
                kind = "REP_"
            if kind == "REPE_" and base[:4] not in ("scas", "cmps"):
                kind = "REP_"
            return [f"{kind}{base.upper()}({seg}, 0x{nf:x});"]

        if base == "arpl":
            self.unhandled[base] = self.unhandled.get(base, 0) + 1
            return [f"rt_unimplemented(0x{ins.addr:x}u, \"arpl\");"]
        self.unhandled[base] = self.unhandled.get(base, 0) + 1
        return [f"rt_unimplemented(0x{ins.addr:x}u, \"{m} {ins.op_str}\");"]


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("prefix")
    ap.add_argument("outdir")
    ap.add_argument("--reloc", action="store_true", help="an AIL/32 driver loaded and relocated at run time")
    args = ap.parse_args()
    with open(args.exe, "rb") as fh:
        sha1 = hashlib.sha1(fh.read()).hexdigest()
    r = Recompiler(args.exe, args.prefix, args.reloc)
    r.build_funcs()
    r.compute_flags_out()
    r.emit_all(args.outdir, sha1)
    fo = [hex(f.entry) for f in r.funcs.values() if f.flags_out]
    print(f"{args.exe}: {len(r.funcs)} functions ({len(r.gap_entries)} from gaps), "
          f"{len(r.prog.insns)} instructions, flags-out: {fo[:20]}{'...' if len(fo) > 20 else ''}")


if __name__ == "__main__":
    main()
