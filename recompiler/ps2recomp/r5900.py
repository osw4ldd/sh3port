"""R5900 (PS2 Emotion Engine) instruction decoder.

Decodes a 32-bit little-endian instruction word into an `Insn` carrying the
mnemonic, operand fields and control-flow classification used by the
analysis and code generation passes.
"""

from dataclasses import dataclass, field

# Control-flow kinds
K_NORMAL = 0
K_BRANCH = 1      # conditional PC-relative branch (has delay slot)
K_JUMP = 2        # j target (absolute, no link)
K_CALL = 3        # jal / bal / bltzal ... (absolute or relative, links ra)
K_JR = 4          # jr rs
K_JALR = 5        # jalr rd, rs
K_ERET = 6
K_SYSCALL = 7
K_BREAK = 8
K_INVALID = 9

GPR_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


@dataclass
class Insn:
    addr: int
    word: int
    name: str
    kind: int = K_NORMAL
    likely: bool = False
    target: int = None          # static branch/jump target
    uncond: bool = False        # branch that is always taken (b / bal)
    fields: dict = field(default_factory=dict)

    @property
    def op(self):
        return self.word >> 26

    @property
    def rs(self):
        return (self.word >> 21) & 31

    @property
    def rt(self):
        return (self.word >> 16) & 31

    @property
    def rd(self):
        return (self.word >> 11) & 31

    @property
    def sa(self):
        return (self.word >> 6) & 31

    @property
    def funct(self):
        return self.word & 63

    @property
    def imm(self):
        v = self.word & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    @property
    def uimm(self):
        return self.word & 0xFFFF

    @property
    def has_delay_slot(self):
        return self.kind in (K_BRANCH, K_JUMP, K_CALL, K_JR, K_JALR)

    def __str__(self):
        return f"{self.addr:08X}: {self.word:08X}  {self.name}"


_SPECIAL = {
    0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav",
    8: "jr", 9: "jalr", 10: "movz", 11: "movn", 12: "syscall", 13: "break",
    15: "sync", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo",
    20: "dsllv", 22: "dsrlv", 23: "dsrav", 24: "mult", 25: "multu",
    26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub", 35: "subu",
    36: "and", 37: "or", 38: "xor", 39: "nor", 40: "mfsa", 41: "mtsa",
    42: "slt", 43: "sltu", 44: "dadd", 45: "daddu", 46: "dsub", 47: "dsubu",
    48: "tge", 49: "tgeu", 50: "tlt", 51: "tltu", 52: "teq", 54: "tne",
    56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32", 62: "dsrl32", 63: "dsra32",
}

_REGIMM = {
    0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 8: "tgei", 9: "tgeiu",
    10: "tlti", 11: "tltiu", 12: "teqi", 14: "tnei", 16: "bltzal",
    17: "bgezal", 18: "bltzall", 19: "bgezall", 24: "mtsab", 25: "mtsah",
}

_PRIMARY = {
    2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz",
    8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi", 13: "ori",
    14: "xori", 15: "lui", 20: "beql", 21: "bnel", 22: "blezl", 23: "bgtzl",
    24: "daddi", 25: "daddiu", 26: "ldl", 27: "ldr", 30: "lq", 31: "sq",
    32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr",
    39: "lwu", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 44: "sdl", 45: "sdr",
    46: "swr", 47: "cache", 49: "lwc1", 51: "pref", 54: "lqc2", 55: "ld",
    57: "swc1", 62: "sqc2", 63: "sd",
}

_MMI = {
    0: "madd", 1: "maddu", 4: "plzcw", 16: "mfhi1", 17: "mthi1", 18: "mflo1",
    19: "mtlo1", 24: "mult1", 25: "multu1", 26: "div1", 27: "divu1",
    32: "madd1", 33: "maddu1", 52: "psllh", 54: "psrlh", 55: "psrah",
    60: "psllw", 62: "psrlw", 63: "psraw",
}

_MMI0 = {
    0: "paddw", 1: "psubw", 2: "pcgtw", 3: "pmaxw", 4: "paddh", 5: "psubh",
    6: "pcgth", 7: "pmaxh", 8: "paddb", 9: "psubb", 10: "pcgtb",
    16: "paddsw", 17: "psubsw", 18: "pextlw", 19: "ppacw", 20: "paddsh",
    21: "psubsh", 22: "pextlh", 23: "ppach", 24: "paddsb", 25: "psubsb",
    26: "pextlb", 27: "ppacb", 30: "pext5", 31: "ppac5",
}

_MMI1 = {
    1: "pabsw", 2: "pceqw", 3: "pminw", 4: "padsbh", 5: "pabsh", 6: "pceqh",
    7: "pminh", 10: "pceqb", 16: "padduw", 17: "psubuw", 18: "pextuw",
    20: "padduh", 21: "psubuh", 22: "pextuh", 24: "paddub", 25: "psubub",
    26: "pextub", 27: "qfsrv",
}

_MMI2 = {
    0: "pmaddw", 2: "psllvw", 3: "psrlvw", 4: "pmsubw", 8: "pmfhi", 9: "pmflo",
    10: "pinth", 12: "pmultw", 13: "pdivw", 14: "pcpyld", 16: "pmaddh",
    17: "phmadh", 18: "pand", 19: "pxor", 20: "pmsubh", 21: "phmsbh",
    26: "pexeh", 27: "prevh", 28: "pmulth", 29: "pdivbw", 30: "pexew",
    31: "prot3w",
}

_MMI3 = {
    0: "pmadduw", 3: "psravw", 8: "pmthi", 9: "pmtlo", 10: "pinteh",
    12: "pmultuw", 13: "pdivuw", 14: "pcpyud", 18: "por", 19: "pnor",
    26: "pexch", 27: "pcpyh", 30: "pexcw",
}

_PMFHL = {0: "pmfhl.lw", 1: "pmfhl.uw", 2: "pmfhl.slw", 3: "pmfhl.lh", 4: "pmfhl.sh"}

_COP0_C0 = {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 24: "eret", 56: "ei", 57: "di"}

_FPU_S = {
    0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s",
    6: "mov.s", 7: "neg.s", 22: "rsqrt.s", 24: "adda.s", 25: "suba.s",
    26: "mula.s", 28: "madd.s", 29: "msub.s", 30: "madda.s", 31: "msuba.s",
    36: "cvt.w.s", 40: "max.s", 41: "min.s", 48: "c.f.s", 50: "c.eq.s",
    52: "c.lt.s", 54: "c.le.s",
}

# VU0 macro mode, SPECIAL1 table (funct < 0x3C)
_VU_SPECIAL1 = {}
for _i, _bc in enumerate("xyzw"):
    _VU_SPECIAL1[0x00 + _i] = "vadd" + _bc
    _VU_SPECIAL1[0x04 + _i] = "vsub" + _bc
    _VU_SPECIAL1[0x08 + _i] = "vmadd" + _bc
    _VU_SPECIAL1[0x0C + _i] = "vmsub" + _bc
    _VU_SPECIAL1[0x10 + _i] = "vmax" + _bc
    _VU_SPECIAL1[0x14 + _i] = "vmini" + _bc
    _VU_SPECIAL1[0x18 + _i] = "vmul" + _bc
_VU_SPECIAL1.update({
    0x1C: "vmulq", 0x1D: "vmaxi", 0x1E: "vmuli", 0x1F: "vminii",
    0x20: "vaddq", 0x21: "vmaddq", 0x22: "vaddi", 0x23: "vmaddi",
    0x24: "vsubq", 0x25: "vmsubq", 0x26: "vsubi", 0x27: "vmsubi",
    0x28: "vadd", 0x29: "vmadd", 0x2A: "vmul", 0x2B: "vmax",
    0x2C: "vsub", 0x2D: "vmsub", 0x2E: "vopmsub", 0x2F: "vmini",
    0x30: "viadd", 0x31: "visub", 0x32: "viaddi", 0x34: "viand",
    0x35: "vior", 0x38: "vcallms", 0x39: "vcallmsr",
})

# VU0 macro mode, SPECIAL2 table (index = (funct & 3) | (bits 10..6) << 2)
_VU_SPECIAL2 = {}
for _i, _bc in enumerate("xyzw"):
    _VU_SPECIAL2[0x00 + _i] = "vadda" + _bc
    _VU_SPECIAL2[0x04 + _i] = "vsuba" + _bc
    _VU_SPECIAL2[0x08 + _i] = "vmadda" + _bc
    _VU_SPECIAL2[0x0C + _i] = "vmsuba" + _bc
    _VU_SPECIAL2[0x18 + _i] = "vmula" + _bc
_VU_SPECIAL2.update({
    0x10: "vitof0", 0x11: "vitof4", 0x12: "vitof12", 0x13: "vitof15",
    0x14: "vftoi0", 0x15: "vftoi4", 0x16: "vftoi12", 0x17: "vftoi15",
    0x1C: "vmulaq", 0x1D: "vabs", 0x1E: "vmulai", 0x1F: "vclipw",
    0x20: "vaddaq", 0x21: "vmaddaq", 0x22: "vaddai", 0x23: "vmaddai",
    0x24: "vsubaq", 0x25: "vmsubaq", 0x26: "vsubai", 0x27: "vmsubai",
    0x28: "vadda", 0x29: "vmadda", 0x2A: "vmula", 0x2C: "vsuba",
    0x2D: "vmsuba", 0x2E: "vopmula", 0x2F: "vnop", 0x30: "vmove",
    0x31: "vmr32", 0x34: "vlqi", 0x35: "vsqi", 0x36: "vlqd", 0x37: "vsqd",
    0x38: "vdiv", 0x39: "vsqrt", 0x3A: "vrsqrt", 0x3B: "vwaitq",
    0x3C: "vmtir", 0x3D: "vmfir", 0x3E: "vilwr", 0x3F: "viswr",
    0x40: "vrnext", 0x41: "vrget", 0x42: "vrinit", 0x43: "vrxor",
})


def _branch_target(addr, word):
    off = word & 0xFFFF
    if off & 0x8000:
        off -= 0x10000
    return (addr + 4 + (off << 2)) & 0xFFFFFFFF


def decode(addr: int, word: int) -> Insn:
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    funct = word & 63

    if op == 0:  # SPECIAL
        name = _SPECIAL.get(funct)
        if name is None:
            return Insn(addr, word, "invalid", K_INVALID)
        if word == 0:
            return Insn(addr, word, "nop")
        if name == "jr":
            return Insn(addr, word, name, K_JR)
        if name == "jalr":
            return Insn(addr, word, name, K_JALR)
        if name == "syscall":
            return Insn(addr, word, name, K_SYSCALL)
        if name == "break":
            return Insn(addr, word, name, K_BREAK)
        return Insn(addr, word, name)

    if op == 1:  # REGIMM
        name = _REGIMM.get(rt)
        if name is None:
            return Insn(addr, word, "invalid", K_INVALID)
        if name.startswith("b"):
            tgt = _branch_target(addr, word)
            likely = name.endswith("l") and name not in ("bltzal", "bgezal")
            if name in ("bltzal", "bgezal", "bltzall", "bgezall"):
                uncond = name == "bgezal" and rs == 0
                ins = Insn(addr, word, name, K_CALL if uncond else K_BRANCH,
                           likely=name.endswith("all"), target=tgt, uncond=uncond)
                ins.fields["link"] = True
                return ins
            uncond = name == "bgez" and rs == 0
            return Insn(addr, word, name, K_BRANCH, likely=likely, target=tgt, uncond=uncond)
        return Insn(addr, word, name)

    if op == 2:
        return Insn(addr, word, "j", K_JUMP,
                    target=((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2))
    if op == 3:
        return Insn(addr, word, "jal", K_CALL,
                    target=((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2))

    if op in (4, 5, 6, 7, 20, 21, 22, 23):
        name = _PRIMARY[op]
        tgt = _branch_target(addr, word)
        uncond = name in ("beq", "beql") and rs == rt  # "b" pseudo-op
        return Insn(addr, word, name, K_BRANCH, likely=op >= 20, target=tgt, uncond=uncond)

    if op == 16:  # COP0
        if rs == 0:
            return Insn(addr, word, "mfc0")
        if rs == 4:
            return Insn(addr, word, "mtc0")
        if rs == 8:
            names = {0: "bc0f", 1: "bc0t", 2: "bc0fl", 3: "bc0tl"}
            if rt in names:
                return Insn(addr, word, names[rt], K_BRANCH, likely=rt >= 2,
                            target=_branch_target(addr, word))
            return Insn(addr, word, "invalid", K_INVALID)
        if rs == 16:
            name = _COP0_C0.get(funct)
            if name == "eret":
                return Insn(addr, word, name, K_ERET)
            if name:
                return Insn(addr, word, name)
        return Insn(addr, word, "invalid", K_INVALID)

    if op == 17:  # COP1 (FPU)
        if rs == 0:
            return Insn(addr, word, "mfc1")
        if rs == 2:
            return Insn(addr, word, "cfc1")
        if rs == 4:
            return Insn(addr, word, "mtc1")
        if rs == 6:
            return Insn(addr, word, "ctc1")
        if rs == 8:
            names = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}
            if rt in names:
                return Insn(addr, word, names[rt], K_BRANCH, likely=rt >= 2,
                            target=_branch_target(addr, word))
            return Insn(addr, word, "invalid", K_INVALID)
        if rs == 16:
            name = _FPU_S.get(funct)
            if name:
                return Insn(addr, word, name)
        if rs == 20 and funct == 32:
            return Insn(addr, word, "cvt.s.w")
        return Insn(addr, word, "invalid", K_INVALID)

    if op == 18:  # COP2 (VU0 macro)
        if rs & 0x10:
            if funct < 0x3C:
                name = _VU_SPECIAL1.get(funct)
            else:
                name = _VU_SPECIAL2.get((funct & 3) | (((word >> 6) & 31) << 2))
            if name:
                return Insn(addr, word, name)
            return Insn(addr, word, "invalid", K_INVALID)
        names = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2"}
        if rs in names:
            return Insn(addr, word, names[rs])
        if rs == 8:
            names = {0: "bc2f", 1: "bc2t", 2: "bc2fl", 3: "bc2tl"}
            if rt in names:
                return Insn(addr, word, names[rt], K_BRANCH, likely=rt >= 2,
                            target=_branch_target(addr, word))
        return Insn(addr, word, "invalid", K_INVALID)

    if op == 28:  # MMI
        if funct == 8:
            name = _MMI0.get((word >> 6) & 31)
        elif funct == 40:
            name = _MMI1.get((word >> 6) & 31)
        elif funct == 9:
            name = _MMI2.get((word >> 6) & 31)
        elif funct == 41:
            name = _MMI3.get((word >> 6) & 31)
        elif funct == 48:
            name = _PMFHL.get((word >> 6) & 31)
        elif funct == 49:
            name = "pmthl.lw" if ((word >> 6) & 31) == 0 else None
        else:
            name = _MMI.get(funct)
        if name:
            return Insn(addr, word, name)
        return Insn(addr, word, "invalid", K_INVALID)

    name = _PRIMARY.get(op)
    if name is None:
        return Insn(addr, word, "invalid", K_INVALID)
    return Insn(addr, word, name)


# Instructions whose result is unpredictable enough that a function containing
# them early is probably data rather than code (used to reject weak entries).
SUSPICIOUS = {"invalid", "tlbr", "tlbwi", "tlbwr", "tlbp"}
