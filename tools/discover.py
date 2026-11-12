"""Code discovery for LE images: recursive-descent disassembly.

Roots: the entry point and every fixup target that lies inside an executable
object (function pointers, jump-table entries, pushed callbacks).  Following
direct branches and calls from the roots finds the reachable code.  Jump
tables (`jmp [disp + reg*4]`) are recognised and their entries (which carry
fixups) become successors of the jump.
"""
from __future__ import annotations

import struct
from collections import Counter, defaultdict
from dataclasses import dataclass, field

import capstone
from capstone import x86

import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from le import LEImage  # noqa: E402

CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
CS.detail = True

COND_JUMPS = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp",
    "jl", "jge", "jle", "jg", "jcxz", "jecxz", "loop", "loope", "loopne",
}


@dataclass
class Insn:
    addr: int
    size: int
    mnem: str
    op_str: str
    bytes: bytes
    ci: object  # capstone instruction (detail)
    succ: list = field(default_factory=list)   # fall-through / branch successors in the same function
    call_target: int | None = None
    is_ret: bool = False
    table: list | None = None                  # jump-table entries for indirect jmp
    indirect: bool = False


class Program:
    def __init__(self, img: LEImage, roots: list[int] | None = None):
        self.img = img
        self.explicit_roots = roots  # for images without a usable entry (e.g. AIL drivers)
        self.code_objs = [o for o in img.objects if o.is_code]
        self.fixup_at = {f.site: f for f in img.fixups}
        self.fixup_sites = sorted(self.fixup_at)
        self.insns: dict[int, Insn] = {}
        self.func_entries: set[int] = set()
        self.table_targets: set[int] = set()
        self.problems: list[str] = []

    def in_code(self, a: int) -> bool:
        return any(o.base <= a < o.base + o.vsize for o in self.code_objs)

    def decode(self, a: int) -> Insn | None:
        if a in self.insns:
            return self.insns[a]
        data = self.img.read(a, 16)
        try:
            ci = next(CS.disasm(data, a, 1))
        except StopIteration:
            self.problems.append(f"undecodable at {a:#x}: {data.hex()}")
            return None
        ins = Insn(a, ci.size, ci.mnemonic, ci.op_str, bytes(ci.bytes), ci)
        self.insns[a] = ins
        return ins

    def jump_table(self, ins: Insn) -> list[int] | None:
        """`jmp dword ptr [disp + reg*4]` or `[reg + disp]`: read entries while each slot carries a fixup
        into code (the table's slots are absolute code addresses, so the loader relocates them)."""
        ops = ins.ci.operands
        if len(ops) != 1 or ops[0].type != x86.X86_OP_MEM:
            return None
        m = ops[0].mem
        if m.index == 0 and m.base == 0:
            return None
        disp = m.disp & 0xFFFFFFFF
        if not self.in_code(disp) or (ins.addr + ins.size - 4) not in self.fixup_at and                 (ins.addr + ins.size - 4) not in self.fixup_at:
            pass
        entries = []
        a = disp
        while True:
            f = self.fixup_at.get(a)
            if f is None or not self.in_code(f.target):
                break
            if a in self.insns and a != disp:
                break
            entries.append(f.target)
            self.table_slots.add(a)
            a += 4
        return entries or None

    def fixups_in(self, ins: Insn):
        for k in range(ins.size - 3):
            f = self.fixup_at.get(ins.addr + k)
            if f is not None:
                yield k, f

    def code_refs(self, ins: Insn) -> list[int]:
        """code addresses an instruction takes as an immediate value (push offset fn / mov reg, offset fn);
        memory displacements (tables, data in the code object) are not code references"""
        out = []
        if ins.ci.imm_offset == 0:
            return out
        f = self.fixup_at.get(ins.addr + ins.ci.imm_offset)
        if f is not None and self.in_code(f.target) and ins.mnem not in ("call", "jmp")                 and not ins.mnem.startswith("j"):
            # an image that mixes code and data in one object: the target must look like a routine
            if self.explicit_roots is None or self.plausible_code(f.target):
                out.append(f.target)
        return out

    BAD_MNEMS = {"arpl", "bound", "into", "aaa", "aas", "daa", "das", "aam", "aad", "salc", "hlt", "insb", "insd",
                 "outsb", "outsd", "lodsb", "lodsd", "les", "lds", "sahf", "lahf", "wait", "fwait"}

    def plausible_code(self, a: int) -> bool:
        """linear decoding from a reaches a return or jump without data-like instructions"""
        end = a + 512
        while a < end:
            data = self.img.read(a, 16)
            if data[:2] in (bytes(2), bytes((0xFF, 0xFF))):
                return False
            try:
                ci = next(CS.disasm(data, a, 1))
            except StopIteration:
                return False
            m = ci.mnemonic
            if m in self.BAD_MNEMS or m.startswith("f"):
                return False
            if m in ("ret", "iretd", "iret", "retf", "jmp"):
                return True
            a += ci.size
        return False

    def run(self) -> None:
        self.table_slots: set[int] = set()
        if self.explicit_roots is not None:
            roots = list(self.explicit_roots)
            self.func_entries.update(roots)
        else:
            roots = [self.img.entry]
            self.func_entries.add(self.img.entry)
            for f in self.img.fixups:
                if self.in_code(f.target) and not self.in_code(f.site):
                    roots.append(f.target)
                    self.func_entries.add(f.target)
        work = list(roots)
        seen = set()
        while work:
            a = work.pop()
            while a not in seen:
                if not self.in_code(a):
                    self.problems.append(f"flow leaves code at {a:#x}")
                    break
                seen.add(a)
                ins = self.decode(a)
                if ins is None:
                    break
                for t in self.code_refs(ins):
                    if t not in self.func_entries:
                        self.func_entries.add(t)
                        work.append(t)
                m = ins.mnem
                nxt = a + ins.size
                if m == "call":
                    op = ins.ci.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        ins.call_target = t
                        self.func_entries.add(t)
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
                    tab = self.jump_table(ins)
                    if tab:
                        ins.table = tab
                        ins.succ = list(dict.fromkeys(tab))
                        self.table_targets.update(tab)
                        work.extend(tab)
                    break
                if m in COND_JUMPS:
                    t = ins.ci.operands[0].imm & 0xFFFFFFFF
                    ins.succ = [nxt, t]
                    work.append(t)
                    a = nxt
                    continue
                if m == "hlt":
                    break
                ins.succ = [nxt]
                a = nxt
        # fixups inside code that no decoded instruction or table covers
        covered = set()
        for i in self.insns.values():
            for k in range(i.size):
                covered.add(i.addr + k)
        self.uncovered_fixups = [f for f in self.img.fixups if self.in_code(f.site)
                                 and f.site not in covered and f.site not in self.table_slots]


def report(path: str) -> Program:
    img = LEImage.load(path)
    p = Program(img)
    p.run()
    code_bytes = sum(i.size for i in p.insns.values())
    total = sum(o.vsize for o in p.code_objs)
    print(f"{path}: {len(p.insns)} insns, {code_bytes}/{total} code bytes covered, "
          f"{len(p.func_entries)} function entries, {len(p.table_targets)} table targets")
    mn = Counter(i.mnem for i in p.insns.values())
    print("  mnemonics:", " ".join(f"{k}:{v}" for k, v in sorted(mn.items(), key=lambda kv: -kv[1])))
    ind = [i for i in p.insns.values() if i.indirect and i.table is None]
    print(f"  indirect jmp/call without table: {len(ind)}")
    for i in ind[:30]:
        print(f"    {i.addr:#x}: {i.mnem} {i.op_str}")
    ints = Counter(i.op_str for i in p.insns.values() if i.mnem == "int")
    print("  int:", dict(ints))
    io = Counter(i.mnem + " " + i.op_str for i in p.insns.values() if i.mnem in ("in", "out", "insb", "outsb", "insd", "outsd", "insw", "outsw"))
    print("  io:", dict(io))
    print(f"  uncovered code fixups: {len(p.uncovered_fixups)}")
    for f in p.uncovered_fixups[:20]:
        print(f"    site {f.site:#x} -> {f.target:#x}")
    for pr in p.problems[:20]:
        print("  problem:", pr)
    return p


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        report(path)
