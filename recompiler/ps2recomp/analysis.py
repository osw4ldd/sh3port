"""Control-flow analysis: function discovery, jump tables, secondary entries.

Model
-----
* Strong entries (ELF entry point, FUNC symbols, `jal` targets, user config)
  become C functions.  A `j` to a strong entry is emitted as a tail call.
* Every function is the set of instructions reachable from its entry through
  branches/jumps that are not calls ("recursive descent").
* `jr rX` (X != ra) is checked for the gcc switch-table pattern; table targets
  become local labels reached through a C `switch`.
* Words in data that look like code pointers, and `lui/addiu` pairs that build
  code addresses, are "weak" entries: if they land inside an already-known
  function they become secondary entries of that function (reachable via the
  runtime dispatcher), otherwise they seed new functions (callbacks, vtables).
"""

import struct
from collections import defaultdict

from .r5900 import (decode, K_BRANCH, K_JUMP, K_CALL, K_JR, K_JALR, K_ERET,
                    K_INVALID, SUSPICIOUS)


class Function:
    def __init__(self, entry, name=None, weak=False):
        self.entry = entry
        self.name = name
        self.weak = weak
        self.insns = set()        # addresses emitted as standalone instructions
        self.delay = set()        # delay-slot addresses (emitted inline)
        self.labels = set()
        self.jumptables = {}      # jr address -> [targets]
        self.secondary = set()    # extra entry points (dispatcher targets)
        self.tailcalls = set()    # addresses of `j` emitted as tail calls
        self.bad = set()          # control flow that leaves the code ranges
        self.invalid = False
        self.suspicious = False   # branch or invalid instruction in a delay slot

    def contains(self, addr):
        return addr in self.insns

    def __repr__(self):
        return f"<Function {self.entry:08X} {self.name or ''} n={len(self.insns)}>"


class Program:
    def __init__(self, elf, extra_entries=(), skip_entries=(), log=print):
        self.elf = elf
        self.log = log
        self.ranges = elf.code_ranges()
        self._cache = {}
        self.functions = {}       # entry -> Function
        self.strong = set()
        self.symbols = {}
        for s in elf.function_symbols():
            if self.in_code(s.value):
                self.symbols.setdefault(s.value, s.name)
        self.extra_entries = set(extra_entries)
        self.skip_entries = set(skip_entries)
        self.stats = defaultdict(int)
        self._trial = None

    # ------------------------------------------------------------------ utils
    def in_code(self, addr):
        if addr & 3:
            return False
        for lo, hi in self.ranges:
            if lo <= addr < hi:
                return True
        return False

    def insn(self, addr):
        ins = self._cache.get(addr)
        if ins is None:
            word = self.elf.read32(addr)
            ins = decode(addr, word if word is not None else 0xFFFFFFFF)
            self._cache[addr] = ins
        return ins

    # --------------------------------------------------------------- descent
    @staticmethod
    def _dest_reg(ins):
        """GPR written by a (non control-flow) instruction, or None."""
        op = ins.op
        if op == 0:
            return ins.rd if ins.name not in ("mult", "multu", "div", "divu", "mthi", "mtlo",
                                               "jr", "sync", "syscall", "break") else None
        if op in (8, 9, 10, 11, 12, 13, 14, 15, 24, 25) or 32 <= op <= 39 or op in (26, 27, 30, 55):
            return ins.rt
        if op == 28:
            return ins.rd
        return None

    def _resolve_const(self, reg, before, depth=0, budget=400):
        """Value of `reg` just before address `before`, if it is built from
        lui/addiu/ori/daddu constants on the straight-line path above."""
        if reg == 0:
            return 0
        if depth > 6:
            return None
        a = before - 4
        for _ in range(budget):
            if not self.in_code(a):
                return None
            ins = self.insn(a)
            if ins.kind in (K_JR,) and ins.rs == 31:
                return None          # crossed the start of this function
            if self._dest_reg(ins) == reg:
                n = ins.name
                if n == "lui":
                    return (ins.uimm << 16) & 0xFFFFFFFF
                if n in ("addiu", "daddiu"):
                    v = self._resolve_const(ins.rs, a, depth + 1, budget)
                    return None if v is None else (v + ins.imm) & 0xFFFFFFFF
                if n == "ori":
                    v = self._resolve_const(ins.rs, a, depth + 1, budget)
                    return None if v is None else v | ins.uimm
                if n in ("daddu", "addu", "or") and ins.rt == 0:
                    return self._resolve_const(ins.rs, a, depth + 1, budget)
                return None
            a -= 4
        return None

    def _jump_table(self, jr_addr, reg):
        """Recognise `lw reg, off(base)` + `jr reg` where base = table + index*4
        (gcc and SN compilers); the table base may be set up far above."""
        lw = None
        a = jr_addr - 4
        for _ in range(8):
            ins = self.insn(a)
            if ins.name == "lw" and ins.rt == reg:
                lw = ins
                break
            if self._dest_reg(ins) == reg:
                return None
            a -= 4
        if lw is None:
            return None
        base = None
        # Find how the address register was formed: addu rB, rIdx, rTable.
        b = lw.addr - 4
        for _ in range(16):
            ins = self.insn(b)
            if self._dest_reg(ins) == lw.rs:
                if ins.name in ("addu", "daddu"):
                    for r in (ins.rs, ins.rt):
                        v = self._resolve_const(r, b)
                        if v is not None:
                            base = v
                            break
                elif ins.name == "lui":
                    base = ins.uimm << 16
                break
            b -= 4
        if base is None:
            return None
        table = (base + lw.imm) & 0xFFFFFFFF
        bound = None
        b = jr_addr - 4
        for _ in range(24):
            ins = self.insn(b)
            if ins.name == "sltiu":
                bound = ins.uimm
                break
            b -= 4
        count = bound if bound and bound <= 4096 else 4096
        targets = []
        for i in range(count):
            w = self.elf.read32(table + 4 * i)
            if w is None or not self.in_code(w):
                break
            if not bound and abs(w - jr_addr) > 0x10000:
                break
            targets.append(w)
        if not targets:
            return None
        self.stats["jumptables"] += 1
        return targets

    def _descend(self, f, starts):
        work = list(starts)
        while work:
            pc = work.pop()
            while True:
                if pc in f.insns:
                    break
                if not self.in_code(pc):
                    f.bad.add(pc)
                    break
                ins = self.insn(pc)
                f.insns.add(pc)
                k = ins.kind
                if ins.has_delay_slot:
                    if not self.in_code(pc + 4):
                        f.bad.add(pc + 4)
                        break
                    f.delay.add(pc + 4)
                    dins = self.insn(pc + 4)
                    if dins.has_delay_slot or dins.kind == K_INVALID:
                        # Compilers never emit this: it is data decoded as code.
                        f.suspicious = True
                if k == K_BRANCH:
                    if ins.fields.get("link"):
                        self.new_strong(ins.target)
                        pc += 8
                        continue
                    f.labels.add(ins.target)
                    work.append(ins.target)
                    if ins.uncond:
                        break
                    pc += 8
                    continue
                if k == K_JUMP:
                    t = ins.target
                    if t != f.entry and (t in self.strong or t in self.symbols):
                        f.tailcalls.add(pc)
                        self.new_strong(t)
                    else:
                        f.labels.add(t)
                        work.append(t)
                    break
                if k == K_CALL:
                    self.new_strong(ins.target)
                    pc += 8
                    continue
                if k == K_JALR:
                    pc += 8
                    continue
                if k == K_JR:
                    if ins.rs != 31:
                        tbl = self._jump_table(pc, ins.rs)
                        if tbl:
                            f.jumptables[pc] = tbl
                            for t in tbl:
                                f.labels.add(t)
                                work.append(t)
                    break
                if k == K_ERET:
                    break
                if k == K_INVALID:
                    f.invalid = True
                    break
                pc += 4

    def new_strong(self, addr):
        if addr in self.skip_entries or not self.in_code(addr):
            return
        if self._trial is not None:
            # Building a speculative (pointer-only) function: its call
            # targets only count if the candidate is accepted.
            self._trial.append(addr)
            return
        if addr not in self.strong:
            self.strong.add(addr)
            self._pending.append(addr)

    def _plausible_entry(self, addr):
        """A call target decoded from speculative code must itself decode."""
        ins = self.insn(addr)
        return ins.kind != K_INVALID and ins.name not in SUSPICIOUS

    def _build_function(self, entry, weak=False):
        f = Function(entry, self.symbols.get(entry), weak)
        self._descend(f, [entry])
        return f

    # ----------------------------------------------------------- weak scans
    def _scan_data_pointers(self, owned=None):
        """Code addresses stored as data.  Words inside code ranges only count
        when no known function executes them (tables embedded in .text)."""
        out = set()
        owned = owned or set()
        for seg in self.elf.segments:
            data = seg.data
            for off in range(0, len(data) - 3, 4):
                w = struct.unpack_from("<I", data, off)[0]
                if (w & 3) or not self.in_code(w):
                    continue
                a = seg.vaddr + off
                if self.in_code(a) and a in owned:
                    continue
                out.add(w)
        return out

    def _scan_code_constants(self):
        """lui rX,hi ; addiu/ori rY,rX,lo  ->  candidate code address."""
        out = set()
        for lo, hi in self.ranges:
            known = {}
            clear_in = 0
            for a in range(lo, hi, 4):
                if clear_in:
                    clear_in -= 1
                    if not clear_in:
                        known.clear()
                ins = self.insn(a)
                n = ins.name
                if n == "lui":
                    known[ins.rt] = ins.uimm << 16
                    continue
                if n in ("addiu", "daddiu") and ins.rs in known:
                    v = (known[ins.rs] + ins.imm) & 0xFFFFFFFF
                    if self.in_code(v):
                        out.add(v)
                elif n == "ori" and ins.rs in known:
                    v = known[ins.rs] | ins.uimm
                    if self.in_code(v):
                        out.add(v)
                if ins.kind != 0:
                    # Values do not survive control flow (but the delay slot
                    # still sees them).
                    clear_in = 2
                else:
                    # Conservatively forget whichever register this writes.
                    known.pop(ins.rd if ins.op == 0 else ins.rt, None)
        return out

    def _scan_after_returns(self):
        """Code right after a function's `jr ra` (skipping alignment nops)
        is usually the next function, even if nothing references it
        statically (e.g. handlers registered through syscalls)."""
        out = set()
        owned = set()
        for f in self.functions.values():
            owned |= f.insns
        for f in self.functions.values():
            for a in f.insns:
                ins = self.insn(a)
                if not (ins.kind in (K_JR, K_JUMP) or (ins.kind == K_BRANCH and ins.uncond)):
                    if ins.kind != K_ERET:
                        continue
                c = a + (4 if ins.kind == K_ERET else 8)
                for _ in range(16):
                    if not self.in_code(c) or self.insn(c).word != 0:
                        break
                    c += 4
                if self.in_code(c) and c not in owned and self.insn(c).word != 0:
                    out.add(c)
        return out

    # ------------------------------------------------------------------ run
    def analyze(self):
        self._pending = []
        self.new_strong(self.elf.entry)
        for a in self.symbols:
            self.new_strong(a)
        for a in self.extra_entries:
            self.new_strong(a)
        self._drain()

        weak = self._scan_data_pointers() | self._scan_code_constants()
        self.stats["weak_candidates"] = len(weak)
        rejected = set()
        # Repeat until no new functions appear.  Each round can expose code
        # after the returns of the functions found in the previous one.
        for _round in range(64):
            weak |= self._scan_after_returns()
            if _round % 4 == 1:
                owned = set()
                for f in self.functions.values():
                    owned |= f.insns
                    owned |= f.delay
                weak |= self._scan_data_pointers(owned)
            weak -= rejected
            owner = {}
            for f in self.functions.values():
                for a in f.insns:
                    owner.setdefault(a, f)
            new = []
            for w in sorted(weak):
                if w in self.functions:
                    continue
                f = owner.get(w)
                if f is not None:
                    if w != f.entry:
                        f.secondary.add(w)
                        f.labels.add(w)
                    continue
                new.append(w)
            if not new:
                break
            added = 0
            for w in new:
                self._trial = []
                cand = self._build_function(w, weak=True)
                trial, self._trial = self._trial, None
                if (cand.invalid or cand.suspicious or not cand.insns or cand.bad
                        or any(not self._plausible_entry(t) for t in trial)):
                    self.stats["weak_rejected"] += 1
                    rejected.add(w)
                    continue
                self.functions[w] = cand
                self.strong.add(w)
                for t in trial:
                    self.new_strong(t)
                added += 1
                for a in cand.insns:
                    owner.setdefault(a, cand)
            self.stats["weak_functions"] += added
            self._drain()
            weak -= set(self.functions)
        return self

    def _drain(self):
        while self._pending:
            a = self._pending.pop()
            if a in self.functions:
                continue
            self.functions[a] = self._build_function(a)

    def summary(self):
        n_ins = sum(len(f.insns) for f in self.functions.values())
        n_sec = sum(len(f.secondary) for f in self.functions.values())
        n_bad = sum(1 for f in self.functions.values() if f.invalid or f.bad)
        return (f"{len(self.functions)} functions, {n_ins} instructions, "
                f"{self.stats['jumptables']} jump tables, {n_sec} secondary entries, "
                f"{self.stats['weak_functions']} pointer-only functions, "
                f"{n_bad} functions with invalid/out-of-range flow")
