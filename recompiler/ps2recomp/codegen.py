"""C code generation for recompiled R5900 functions.

Every MIPS function becomes `void F_XXXXXXXX(R5900 *c)`.  All guest state
lives in the context struct and guest memory, so tail calls, gotos and
dispatcher calls are all semantically equivalent ways to transfer control.
Most instruction semantics live in inline helpers in runtime/include so the
generated code stays small and readable.
"""

from .r5900 import (K_NORMAL, K_BRANCH, K_JUMP, K_CALL, K_JR, K_JALR, K_ERET,
                    K_SYSCALL, K_BREAK, K_INVALID)


def fname(addr):
    return f"F_{addr:08X}"


def lbl(addr):
    return f"L_{addr:08X}"


def _hex(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class UnsupportedInsn(Exception):
    pass


# --------------------------------------------------------------- operands
def U64(r):
    return f"c->r[{r}].UD[0]"


def S64(r):
    return f"c->r[{r}].SD[0]"


def U32(r):
    return f"c->r[{r}].UW[0]"


def S32(r):
    return f"c->r[{r}].SW[0]"


def addr_expr(ins):
    if ins.imm == 0:
        return U32(ins.rs)
    return f"({U32(ins.rs)} + {_hex(ins.imm)})"


def set64(r, expr):
    return "" if r == 0 else f"{U64(r)} = {expr};"


def sets32(r, expr):
    return "" if r == 0 else f"{S64(r)} = (int32_t)({expr});"


# ------------------------------------------------------- per-insn semantics
def _special(ins):
    n, rs, rt, rd, sa = ins.name, ins.rs, ins.rt, ins.rd, ins.sa
    if n == "sll":
        return sets32(rd, f"{U32(rt)} << {sa}")
    if n == "srl":
        return sets32(rd, f"{U32(rt)} >> {sa}")
    if n == "sra":
        return sets32(rd, f"{S32(rt)} >> {sa}")
    if n == "sllv":
        return sets32(rd, f"{U32(rt)} << ({U32(rs)} & 31)")
    if n == "srlv":
        return sets32(rd, f"{U32(rt)} >> ({U32(rs)} & 31)")
    if n == "srav":
        return sets32(rd, f"{S32(rt)} >> ({U32(rs)} & 31)")
    if n == "movz":
        return "" if rd == 0 else f"if ({U64(rt)} == 0) {U64(rd)} = {U64(rs)};"
    if n == "movn":
        return "" if rd == 0 else f"if ({U64(rt)} != 0) {U64(rd)} = {U64(rs)};"
    if n == "sync":
        return ""
    if n == "mfhi":
        return set64(rd, "c->hi.UD[0]")
    if n == "mthi":
        return f"c->hi.UD[0] = {U64(rs)};"
    if n == "mflo":
        return set64(rd, "c->lo.UD[0]")
    if n == "mtlo":
        return f"c->lo.UD[0] = {U64(rs)};"
    if n == "dsllv":
        return set64(rd, f"{U64(rt)} << ({U32(rs)} & 63)")
    if n == "dsrlv":
        return set64(rd, f"{U64(rt)} >> ({U32(rs)} & 63)")
    if n == "dsrav":
        return set64(rd, f"(uint64_t)({S64(rt)} >> ({U32(rs)} & 63))")
    if n in ("mult", "multu", "div", "divu"):
        return f"ps2_{n}(c, {rd}, {rs}, {rt}, 0);"
    if n in ("add", "addu"):
        return sets32(rd, f"{U32(rs)} + {U32(rt)}")
    if n in ("sub", "subu"):
        return sets32(rd, f"{U32(rs)} - {U32(rt)}")
    if n == "and":
        return set64(rd, f"{U64(rs)} & {U64(rt)}")
    if n == "or":
        return set64(rd, f"{U64(rs)} | {U64(rt)}")
    if n == "xor":
        return set64(rd, f"{U64(rs)} ^ {U64(rt)}")
    if n == "nor":
        return set64(rd, f"~({U64(rs)} | {U64(rt)})")
    if n == "mfsa":
        return set64(rd, "c->sa")
    if n == "mtsa":
        return f"c->sa = {U32(rs)} & 15;"
    if n == "slt":
        return set64(rd, f"{S64(rs)} < {S64(rt)}")
    if n == "sltu":
        return set64(rd, f"{U64(rs)} < {U64(rt)}")
    if n in ("dadd", "daddu"):
        return set64(rd, f"{U64(rs)} + {U64(rt)}")
    if n in ("dsub", "dsubu"):
        return set64(rd, f"{U64(rs)} - {U64(rt)}")
    traps = {"tge": ">=", "tlt": "<", "teq": "==", "tne": "!="}
    if n in traps:
        return f"if ({S64(rs)} {traps[n]} {S64(rt)}) ps2_trap(c, {_hex(ins.addr)});"
    if n in ("tgeu", "tltu"):
        op = ">=" if n == "tgeu" else "<"
        return f"if ({U64(rs)} {op} {U64(rt)}) ps2_trap(c, {_hex(ins.addr)});"
    if n == "dsll":
        return set64(rd, f"{U64(rt)} << {sa}")
    if n == "dsrl":
        return set64(rd, f"{U64(rt)} >> {sa}")
    if n == "dsra":
        return set64(rd, f"(uint64_t)({S64(rt)} >> {sa})")
    if n == "dsll32":
        return set64(rd, f"{U64(rt)} << {sa + 32}")
    if n == "dsrl32":
        return set64(rd, f"{U64(rt)} >> {sa + 32}")
    if n == "dsra32":
        return set64(rd, f"(uint64_t)({S64(rt)} >> {sa + 32})")
    raise UnsupportedInsn(n)


_LOADS = {
    "lb": ("ps2_read8", "(int8_t)", True),
    "lbu": ("ps2_read8", "", True),
    "lh": ("ps2_read16", "(int16_t)", True),
    "lhu": ("ps2_read16", "", True),
    "lw": ("ps2_read32", "(int32_t)", True),
    "lwu": ("ps2_read32", "", True),
    "ld": ("ps2_read64", "", True),
}
_STORES = {
    "sb": ("ps2_write8", "(uint8_t)"),
    "sh": ("ps2_write16", "(uint16_t)"),
    "sw": ("ps2_write32", "(uint32_t)"),
    "sd": ("ps2_write64", ""),
}


def _primary(ins):
    n, rs, rt = ins.name, ins.rs, ins.rt
    imm = ins.imm
    if n in ("addi", "addiu"):
        return sets32(rt, f"{U32(rs)} + {_hex(imm)}")
    if n == "slti":
        return set64(rt, f"{S64(rs)} < (int64_t){imm}")
    if n == "sltiu":
        return set64(rt, f"{U64(rs)} < (uint64_t)(int64_t){imm}")
    if n == "andi":
        return set64(rt, f"{U64(rs)} & 0x{ins.uimm:X}u")
    if n == "ori":
        return set64(rt, f"{U64(rs)} | 0x{ins.uimm:X}u")
    if n == "xori":
        return set64(rt, f"{U64(rs)} ^ 0x{ins.uimm:X}u")
    if n == "lui":
        return "" if rt == 0 else f"{S64(rt)} = (int32_t){_hex(ins.uimm << 16)};"
    if n in ("daddi", "daddiu"):
        return set64(rt, f"{U64(rs)} + (uint64_t)(int64_t){imm}")
    if n in _LOADS:
        fn, cast, _ = _LOADS[n]
        if rt == 0:
            return f"(void){fn}({addr_expr(ins)});"
        if cast:
            return f"{S64(rt)} = {cast}{fn}({addr_expr(ins)});"
        return f"{U64(rt)} = {fn}({addr_expr(ins)});"
    if n in _STORES:
        fn, cast = _STORES[n]
        src = U64(rt)
        return f"{fn}({addr_expr(ins)}, {cast}{src});"
    if n == "lq":
        if rt == 0:
            return ""
        return f"ps2_read128({addr_expr(ins)} & ~15u, &c->r[{rt}]);"
    if n == "sq":
        return f"ps2_write128({addr_expr(ins)} & ~15u, &c->r[{rt}]);"
    if n in ("lwl", "lwr", "ldl", "ldr"):
        if rt == 0:
            return ""
        return f"ps2_{n}(c, {rt}, {addr_expr(ins)});"
    if n in ("swl", "swr", "sdl", "sdr"):
        return f"ps2_{n}(c, {rt}, {addr_expr(ins)});"
    if n in ("cache", "pref"):
        return ""
    if n == "lwc1":
        return f"c->f[{rt}].u = ps2_read32({addr_expr(ins)});"
    if n == "swc1":
        return f"ps2_write32({addr_expr(ins)}, c->f[{rt}].u);"
    if n == "lqc2":
        if rt == 0:
            return ""
        return f"ps2_read128({addr_expr(ins)} & ~15u, (u128 *)&c->vf[{rt}]);"
    if n == "sqc2":
        return f"ps2_write128({addr_expr(ins)} & ~15u, (const u128 *)&c->vf[{rt}]);"
    raise UnsupportedInsn(n)


def _regimm(ins):
    n = ins.name
    imm = ins.imm
    if n == "mtsab":
        return f"c->sa = ({U32(ins.rs)} & 15) ^ ({imm} & 15);"
    if n == "mtsah":
        return f"c->sa = (({U32(ins.rs)} & 7) ^ ({imm} & 7)) << 1;"
    traps = {"tgei": ">=", "tlti": "<", "teqi": "==", "tnei": "!="}
    if n in traps:
        return f"if ({S64(ins.rs)} {traps[n]} (int64_t){imm}) ps2_trap(c, {_hex(ins.addr)});"
    if n in ("tgeiu", "tltiu"):
        op = ">=" if n == "tgeiu" else "<"
        return f"if ({U64(ins.rs)} {op} (uint64_t)(int64_t){imm}) ps2_trap(c, {_hex(ins.addr)});"
    raise UnsupportedInsn(n)


def _cop0(ins):
    n = ins.name
    if n == "mfc0":
        return f"{{ uint32_t v = ps2_mfc0(c, {ins.rd}, {ins.word & 0x7FF}); " \
               f"{sets32(ins.rt, 'v') or '(void)v;'} }}"
    if n == "mtc0":
        return f"ps2_mtc0(c, {ins.rd}, {ins.word & 0x7FF}, {U32(ins.rt)});"
    if n == "ei":
        return "ps2_ei(c);"
    if n == "di":
        return "ps2_di(c);"
    if n in ("tlbr", "tlbwi", "tlbwr", "tlbp"):
        return ""
    raise UnsupportedInsn(n)


def _cop1(ins):
    n = ins.name
    w = ins.word
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    if n == "mfc1":
        return sets32(ins.rt, f"c->f[{fs}].s")
    if n == "mtc1":
        return f"c->f[{fs}].u = {U32(ins.rt)};"
    if n == "cfc1":
        return sets32(ins.rt, f"ps2_cfc1(c, {fs})")
    if n == "ctc1":
        return f"ps2_ctc1(c, {fs}, {U32(ins.rt)});"
    simple = {
        "add.s": f"c->f[{fd}].f = fpu_add(c->f[{fs}].f, c->f[{ft}].f);",
        "sub.s": f"c->f[{fd}].f = fpu_sub(c->f[{fs}].f, c->f[{ft}].f);",
        "mul.s": f"c->f[{fd}].f = fpu_mul(c->f[{fs}].f, c->f[{ft}].f);",
        "div.s": f"c->f[{fd}].f = fpu_div(c, c->f[{fs}].f, c->f[{ft}].f);",
        "sqrt.s": f"c->f[{fd}].f = fpu_sqrt(c, c->f[{ft}].f);",
        "rsqrt.s": f"c->f[{fd}].f = fpu_rsqrt(c, c->f[{fs}].f, c->f[{ft}].f);",
        "abs.s": f"c->f[{fd}].u = c->f[{fs}].u & 0x7FFFFFFFu;",
        "mov.s": f"c->f[{fd}].u = c->f[{fs}].u;",
        "neg.s": f"c->f[{fd}].u = c->f[{fs}].u ^ 0x80000000u;",
        "adda.s": f"c->facc.f = fpu_add(c->f[{fs}].f, c->f[{ft}].f);",
        "suba.s": f"c->facc.f = fpu_sub(c->f[{fs}].f, c->f[{ft}].f);",
        "mula.s": f"c->facc.f = fpu_mul(c->f[{fs}].f, c->f[{ft}].f);",
        "madd.s": f"c->f[{fd}].f = fpu_add(c->facc.f, fpu_mul(c->f[{fs}].f, c->f[{ft}].f));",
        "msub.s": f"c->f[{fd}].f = fpu_sub(c->facc.f, fpu_mul(c->f[{fs}].f, c->f[{ft}].f));",
        "madda.s": f"c->facc.f = fpu_add(c->facc.f, fpu_mul(c->f[{fs}].f, c->f[{ft}].f));",
        "msuba.s": f"c->facc.f = fpu_sub(c->facc.f, fpu_mul(c->f[{fs}].f, c->f[{ft}].f));",
        "cvt.w.s": f"c->f[{fd}].s = fpu_cvtw(c->f[{fs}].f);",
        "cvt.s.w": f"c->f[{fd}].f = (float)c->f[{fs}].s;",
        "max.s": f"c->f[{fd}].f = fpu_max(c->f[{fs}].f, c->f[{ft}].f);",
        "min.s": f"c->f[{fd}].f = fpu_min(c->f[{fs}].f, c->f[{ft}].f);",
        "c.f.s": "c->fcr31 &= ~FPU_C;",
        "c.eq.s": f"fpu_setc(c, c->f[{fs}].f == c->f[{ft}].f);",
        "c.lt.s": f"fpu_setc(c, c->f[{fs}].f < c->f[{ft}].f);",
        "c.le.s": f"fpu_setc(c, c->f[{fs}].f <= c->f[{ft}].f);",
    }
    if n in simple:
        return simple[n]
    raise UnsupportedInsn(n)


def _cop2(ins):
    n = ins.name
    w = ins.word
    rt, rd = ins.rt, ins.rd
    if n == "qmfc2":
        return "" if rt == 0 else f"memcpy(&c->r[{rt}], &c->vf[{rd}], 16);"
    if n == "qmtc2":
        return "" if rd == 0 else f"memcpy(&c->vf[{rd}], &c->r[{rt}], 16);"
    if n == "cfc2":
        return sets32(rt, f"vu0_cfc2(c, {rd})")
    if n == "ctc2":
        return f"vu0_ctc2(c, {rd}, {U32(rt)});"
    # Macro-mode VU0 operations: helpers named after the mnemonic.
    return f"VU0_{n.upper()}(c, {_hex(w)});"


def _mmi(ins):
    n = ins.name.replace(".", "_")
    return f"MMI_{n.upper()}(c, {ins.rd}, {ins.rs}, {ins.rt}, {ins.sa});"


def semantics(ins):
    """C statement(s) for a non control-flow instruction."""
    op = ins.op
    if ins.name == "nop":
        return ""
    if op == 0:
        return _special(ins)
    if op == 1:
        return _regimm(ins)
    if op == 16:
        return _cop0(ins)
    if op == 17:
        return _cop1(ins)
    if op == 18:
        return _cop2(ins)
    if op == 28:
        return _mmi(ins)
    return _primary(ins)


def branch_cond(ins):
    """C boolean expression for a conditional branch."""
    n, rs, rt = ins.name.rstrip("l") if ins.likely else ins.name, ins.rs, ins.rt
    n = {"bltzal": "bltz", "bgezal": "bgez", "bltza": "bltz", "bgeza": "bgez"}.get(n, n)
    if n == "beq":
        return f"{U64(rs)} == {U64(rt)}"
    if n == "bne":
        return f"{U64(rs)} != {U64(rt)}"
    if n == "blez":
        return f"{S64(rs)} <= 0"
    if n == "bgtz":
        return f"{S64(rs)} > 0"
    if n == "bltz":
        return f"{S64(rs)} < 0"
    if n == "bgez":
        return f"{S64(rs)} >= 0"
    if n in ("bc1f", "bc1t"):
        return ("" if n == "bc1t" else "!") + "(c->fcr31 & FPU_C)"
    if n in ("bc0f", "bc0t"):
        return ("" if n == "bc0t" else "!") + "ps2_cpcond0(c)"
    if n in ("bc2f", "bc2t"):
        return ("" if n == "bc2t" else "!") + "vu0_cpcond2(c)"
    raise UnsupportedInsn(ins.name)


# ------------------------------------------------------------- functions
_RA_LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "lwl", "lwr", "ld", "ldl", "ldr", "lq"}
_COP_TO_GPR = {"mfc0", "mfc1", "cfc1", "qmfc2", "cfc2"}


def writes_ra_noncall(ins):
    """True if `ins` writes ra other than by linking (jal/bal/bltzal/jalr ra) and other
    than the usual epilogue restore from the stack (load with base sp).  Such a write
    can make `jr ra` jump somewhere that is not the caller, e.g. the "fake return
    address" tail loops of SH3's quicksorts (0x139E98: lui/addiu ra, 0x139D48)."""
    if ins.kind != K_NORMAL:
        return False
    op, n = ins.op, ins.name
    if op == 0:
        dst = ins.rd if n not in ("mult", "multu", "div", "divu", "mthi", "mtlo", "sync",
                                  "mtsa", "tge", "tgeu", "tlt", "tltu", "teq", "tne",
                                  "nop") else None
    elif op == 28:
        dst = ins.rd
    elif n in _COP_TO_GPR:
        dst = ins.rt
    elif op in (8, 9, 10, 11, 12, 13, 14, 15, 24, 25) or n in _RA_LOADS:
        dst = ins.rt
    else:
        dst = None
    if dst != 31:
        return False
    if n in _RA_LOADS and ins.rs == 29:
        return False
    return True


class FunctionEmitter:
    def __init__(self, prog, func, known_funcs, options):
        self.prog = prog
        self.f = func
        self.known = known_funcs   # entry addr -> Function
        self.opt = options
        self.out = []
        self.warnings = []

    def w(self, s, indent=1):
        if s:
            self.out.append("    " * indent + s)

    def stmt(self, ins):
        try:
            return semantics(ins)
        except UnsupportedInsn:
            self.warnings.append(f"{ins.addr:08X}: unsupported {ins.name}")
            return f"ps2_unimplemented(c, {_hex(ins.addr)}, {_hex(ins.word)});"

    def delay_code(self, a):
        ins = self.prog.insn(a + 4)
        if ins.kind == K_BREAK:   # e.g. "beql divisor, zero; break 7"
            return f"ps2_break(c, {_hex(ins.addr)}, {(ins.word >> 6) & 0xFFFFF});"
        if ins.kind == K_SYSCALL:
            return f"c->pc = {_hex(ins.addr)}; ps2_syscall(c, {(ins.word >> 6) & 0xFFFFF});"
        if ins.has_delay_slot or ins.kind == K_ERET:
            # Control flow in a delay slot is undefined on MIPS; execute the
            # side effect-free part only.
            self.warnings.append(f"{a + 4:08X}: control flow in delay slot ({ins.name})")
            return f"/* delay slot: {ins.name} ignored */"
        return self.stmt(ins)

    def call(self, target):
        if target in self.known:
            return f"{fname(target)}(c);"
        return f"ps2_call(c, {_hex(target)});"

    def goto(self, src, target):
        poll = f"PS2_POLL_AT(c, {_hex(src)}); " if target <= src else ""
        if target in self.f.insns:
            return f"{poll}goto {lbl(target)};"
        return f"{poll}ps2_jump(c, {_hex(target)}); return;"

    def emit(self):
        f = self.f
        name = fname(f.entry)
        comment = f" /* {f.name} */" if f.name else ""
        hle = self.opt.get("hle", {}).get(f.entry)
        if hle:
            # Native replacement (runtime/src/core/hle_iop.c); the original body
            # stays callable as F_X__orig (ps2_hle_original()).
            self.out.append(f"void {name}(R5900 *c){comment}")
            self.out.append("{")
            self.w("static int hle_id = -1;")
            self.w(f'if (!c->jump_pc && ps2_hle(c, &hle_id, "{hle}")) return;')
            self.w(f"{name}__orig(c);")
            self.out.append("}")
            self.out.append("")
            name = f"{name}__orig"
        self.out.append(f"void {name}(R5900 *c){comment}")
        self.out.append("{")
        self.has_reg_jr = any(self.prog.insn(a).kind == K_JR and self.prog.insn(a).rs != 31
                              for a in f.insns)
        # ra written by something other than a call: `jr ra` may target this function
        # itself (or anywhere), so it cannot simply be a C return.
        ra_writes = [a for a in f.insns if writes_ra_noncall(self.prog.insn(a))]
        self.ra_dynamic = bool(ra_writes) and any(
            self.prog.insn(a).kind == K_JR and self.prog.insn(a).rs == 31 for a in f.insns)
        self.ra_targets = set()
        if self.ra_dynamic:
            for a in ra_writes:
                v = self.prog._resolve_const(31, a + 4)
                if v is not None and v in f.insns:
                    self.ra_targets.add(v)
            self.ra_targets |= {s for s in f.secondary if s in f.insns}
        if self.has_reg_jr or self.ra_dynamic:
            # "move t4, ra; ...; jr t4" style returns compare against this.
            self.w("const uint32_t entry_ra = c->r[31].UW[0];")
        if f.secondary:
            self.w("if (c->jump_pc) {")
            self.w("uint32_t t = c->jump_pc; c->jump_pc = 0;", 2)
            self.w("switch (t) {", 2)
            for s in sorted(f.secondary):
                self.w(f"case {_hex(s)}: goto {lbl(s)};", 3)
            self.w("default: ps2_bad_entry(c, t); return;", 3)
            self.w("}", 2)
            self.w("}")
        if f.entry != min(f.insns):
            self.w(f"goto {lbl(f.entry)};")
        addrs = sorted(f.insns)
        for i, a in enumerate(addrs):
            nxt = addrs[i + 1] if i + 1 < len(addrs) else None
            ins = self.prog.insn(a)
            fall = self.emit_insn(a, ins)
            if fall is not None and fall != nxt:
                if fall in f.insns:
                    self.w(f"goto {lbl(fall)};")
                else:
                    self.w(f"ps2_jump(c, {_hex(fall)}); return;")
        self.out.append("}")
        self.out.append("")
        return self.out

    def emit_insn(self, a, ins):
        """Emit one instruction; return the fall-through address or None."""
        self.out.append(f"{lbl(a)}: /* {ins.word:08X} {ins.name} */")
        k = ins.kind
        if k == K_NORMAL:
            self.w(self.stmt(ins))
            return a + 4

        if k == K_BRANCH:
            if ins.fields.get("link"):
                cond = branch_cond(ins)
                self.w("{")
                self.w(f"int cond = {cond};", 2)
                self.w(f"c->r[31].SD[0] = (int32_t){_hex(a + 8)};", 2)
                if ins.likely:
                    self.w(f"if (cond) {{ {self.delay_code(a)} {self.call(ins.target)} }}", 2)
                else:
                    self.w(self.delay_code(a), 2)
                    self.w(f"if (cond) {{ {self.call(ins.target)} }}", 2)
                self.w("}")
                return a + 8
            if ins.uncond:
                self.w(self.delay_code(a))
                self.w(self.goto(a, ins.target))
                return None
            cond = branch_cond(ins)
            toggle = self.opt.get("branch_toggles", {}).get(a)
            if toggle is not None:
                # Optional patch "branch -> nop": the branch is never taken
                # while the option is on (switchable at run time).
                cond = f"({cond}) && !ps2_opt_flags[{toggle}]"
            self.w("{")
            self.w(f"int cond = {cond};", 2)
            if ins.likely:
                self.w(f"if (cond) {{ {self.delay_code(a)} {self.goto(a, ins.target)} }}", 2)
            else:
                self.w(self.delay_code(a), 2)
                self.w(f"if (cond) {{ {self.goto(a, ins.target)} }}", 2)
            self.w("}")
            return a + 8

        if k == K_JUMP:
            self.w(self.delay_code(a))
            if a in self.f.tailcalls:
                self.w(f"{self.call(ins.target)} return;")
            else:
                self.w(self.goto(a, ins.target))
            return None

        if k == K_CALL:
            self.w(f"c->r[31].SD[0] = (int32_t){_hex(a + 8)};")
            self.w(self.delay_code(a))
            self.w(self.call(ins.target))
            return a + 8

        if k == K_JALR:
            self.w("{")
            self.w(f"uint32_t t = {U32(ins.rs)};", 2)
            if ins.rd:
                self.w(f"c->r[{ins.rd}].SD[0] = (int32_t){_hex(a + 8)};", 2)
            self.w(self.delay_code(a), 2)
            self.w("ps2_call(c, t);", 2)
            self.w("}")
            return a + 8

        if k == K_JR:
            if ins.rs == 31 and not self.ra_dynamic:
                self.w(self.delay_code(a))
                self.w("return;")
                return None
            self.w("{")
            self.w(f"uint32_t t = {U32(ins.rs)};", 2)
            self.w(self.delay_code(a), 2)
            if ins.rs == 31:
                # ra may hold an address inside this function (fake return address)
                if self.ra_targets:
                    self.w("if (t != entry_ra) switch (t) {", 2)
                    for t in sorted(self.ra_targets):
                        self.w(f"case {_hex(t)}: goto {lbl(t)};", 3)
                    self.w("default: break;", 3)
                    self.w("}", 2)
                self.w("if (t == entry_ra) return;", 2)
                self.w("ps2_jump(c, t); return;", 2)
                self.w("}")
                return None
            table = sorted(set(self.f.jumptables.get(a, ())))
            if table:
                self.w("switch (t) {", 2)
                for t in table:
                    self.w(f"case {_hex(t)}: goto {lbl(t)};", 3)
                self.w("default: break;", 3)
                self.w("}", 2)
            self.w("if (t == entry_ra) return;   /* return through a copy of ra */", 2)
            self.w("ps2_jump(c, t); return;", 2)
            self.w("}")
            return None

        if k == K_SYSCALL:
            self.w(f"c->pc = {_hex(a)}; ps2_syscall(c, {(ins.word >> 6) & 0xFFFFF});")
            return a + 4

        if k == K_BREAK:
            self.w(f"ps2_break(c, {_hex(a)}, {(ins.word >> 6) & 0xFFFFF});")
            return a + 4

        if k == K_ERET:
            self.w("ps2_eret(c); return;")
            return None

        if k == K_INVALID:
            self.w(f"ps2_unimplemented(c, {_hex(a)}, {_hex(ins.word)}); return;")
            return None

        raise AssertionError(k)
