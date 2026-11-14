"""VU micro-code decoder and C generator (task S3-M1).

A VU micro instruction is a 64-bit pair: the lower word (lower address) holds the
lower-unit op (load/store, integer, branch, FDIV, EFU, flags, XGKICK, ...), the upper
word holds the FMAC op plus the I/E/M/D/T bits:

    upper bit 31 I  (lower word is a float immediate for the I register)
              30 E  (end of program after the next instruction)
              29 M  (VU0 only: EE sync)
              28 D  (debug break; ignored, as PCSX2 microVU: doDBitHandling = false)
              27 T  (halt + interrupt when FBRST.TE is set)

Encodings are the PCSX2 microVU tables (third_party/pcsx2/pcsx2/x86/microVU_Tables.inl,
read only): upper op = bits 5..0, with 0x3C..0x3F selecting the FD_00..FD_11 tables
indexed by bits 10..6; lower op = bits 31..25, 0x40 selects the LowerOP table (bits 5..0)
whose 0x3C..0x3F entries select the T3_00..T3_11 tables (bits 10..6).  Field macros
follow microVU_Misc.h (_Ft_, _Fs_, _Fd_, _Imm5_, _Imm11_, _Imm12_, _Imm15_, _Imm24_).

Semantics follow microVU (the comparison target of S3-M3), see microVU_Upper.inl,
microVU_Lower.inl, microVU_Analyze.inl, microVU_Flags.inl, microVU_Compile.inl:

* FMAC ops go through the "SSE op" wrappers (microVU_Misc.inl clampOp): with the
  extra-overflow clamp mode (vuClampMode 2, which the PCSX2 GameDB sets for SLUS-20622)
  every add/sub/mul/div clamps both operands and the result (NaN -> +FLT_MAX, +-Inf ->
  +-FLT_MAX).  Mode 1 (PCSX2 default) clamps only the per-opcode operands listed in
  mVU_UPPER (cFs/cFt).  The generator writes the mode as VU_CLAMP_MODE.
* MAX/MINI compare the float bit patterns as sign-magnitude integers and never set flags.
* MAC/STATUS/CLIP flags have a 4-cycle latency (4 flag instances in mVU); FSxxx/FMxxx/
  FCxxx read the newest instance whose write cycle + 4 <= current cycle.  O/U flags are
  never set (microVU only sets them with a game fix).
* Q (DIV 7, SQRT 7, RSQRT 13 cycles) and P (EFU latencies) are pipelined; reading Q/P
  before the result is ready returns the old value; WAITQ/WAITP and a new FDIV/EFU op
  stall.  The FDIV I/D status bits are written 4 cycles before Q becomes visible.
* VF stalls (4 cycles after an FMAC/LQ/MOVE... write) are computed statically inside each
  straight-line path (the pipeline is assumed empty at every label), and advance the
  cycle counter that the flag/Q/P model uses.
* The two halves of an instruction read their registers before either writes; when both
  write the same VF register the lower result is dropped (mVUsetCycles).  The I-bit sets
  I after the upper op (doIbit).
* A conditional branch whose VI register was written by the instruction right before it
  reads the value from before the oldest such write in the last 4 instructions
  (analyzeBranchVI).
* XGKICK transfers the packet after the next instruction has executed (mVU_XGKICK_DELAY).
* The generated code expects the host MXCSR to be FTZ|DAZ|round-toward-zero (0xFFC0),
  the PCSX2 default VU1 FPCR (Pcsx2Config.cpp DEFAULT_VU_FP_CONTROL_REGISTER).

Generated C: one function per program (code-hash + entry), see emit_program().  The
runtime state struct is described in INTERFACE_DOC / reference_state_header(); the
runtime side (ps2_vu1.h, vu1.c) belongs to S3-M2.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

# ----------------------------------------------------------------- constants
I_BIT = 1 << 31
E_BIT = 1 << 30
M_BIT = 1 << 29
D_BIT = 1 << 28
T_BIT = 1 << 27

FIELDS = "xyzw"


class VUUnsupported(Exception):
    """The program uses something the translator does not handle (the runtime must
    fall back to microVU for it)."""


# ----------------------------------------------------------------- tables
# microVU_Tables.inl mVU_UPPER_OPCODE[64]
_UPPER_MAIN = (
    ["ADDx", "ADDy", "ADDz", "ADDw", "SUBx", "SUBy", "SUBz", "SUBw",
     "MADDx", "MADDy", "MADDz", "MADDw", "MSUBx", "MSUBy", "MSUBz", "MSUBw",
     "MAXx", "MAXy", "MAXz", "MAXw", "MINIx", "MINIy", "MINIz", "MINIw",
     "MULx", "MULy", "MULz", "MULw", "MULq", "MAXi", "MULi", "MINIi",
     "ADDq", "MADDq", "ADDi", "MADDi", "SUBq", "MSUBq", "SUBi", "MSUBi",
     "ADD", "MADD", "MUL", "MAX", "SUB", "MSUB", "OPMSUB", "MINI"]
    + [None] * 12 + ["#FD0", "#FD1", "#FD2", "#FD3"])

# mVU_UPPER_FD_00..11_TABLE (index bits 10..6; entries 12..31 unknown)
_UPPER_FD = (
    ["ADDAx", "SUBAx", "MADDAx", "MSUBAx", "ITOF0", "FTOI0", "MULAx", "MULAq",
     "ADDAq", "SUBAq", "ADDA", "SUBA"],
    ["ADDAy", "SUBAy", "MADDAy", "MSUBAy", "ITOF4", "FTOI4", "MULAy", "ABS",
     "MADDAq", "MSUBAq", "MADDA", "MSUBA"],
    ["ADDAz", "SUBAz", "MADDAz", "MSUBAz", "ITOF12", "FTOI12", "MULAz", "MULAi",
     "ADDAi", "SUBAi", "MULA", "OPMULA"],
    ["ADDAw", "SUBAw", "MADDAw", "MSUBAw", "ITOF15", "FTOI15", "MULAw", "CLIP",
     "MADDAi", "MSUBAi", None, "NOP"],
)

# mVULOWER_OPCODE[128] (bits 31..25)
_LOWER_MAIN = {
    0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU",
    0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR",
    0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
    0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET",
    0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR",
    0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ",
}
# mVULowerOP_OPCODE[64] (bits 5..0, when bits 31..25 == 0x40)
_LOWER_OP = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
# mVULowerOP_T3_00..11_OPCODE[32] (bits 10..6, when bits 5..0 == 0x3C..0x3F)
_LOWER_T3 = (
    {0x0C: "MOVE", 0x0D: "LQI", 0x0E: "DIV", 0x0F: "MTIR", 0x10: "RNEXT", 0x19: "MFP",
     0x1A: "XTOP", 0x1B: "XGKICK", 0x1C: "ESADD", 0x1D: "EATANxy", 0x1E: "ESQRT",
     0x1F: "ESIN"},
    {0x0C: "MR32", 0x0D: "SQI", 0x0E: "SQRT", 0x0F: "MFIR", 0x10: "RGET", 0x1A: "XITOP",
     0x1C: "ERSADD", 0x1D: "EATANxz", 0x1E: "ERSQRT", 0x1F: "EATAN"},
    {0x0D: "LQD", 0x0E: "RSQRT", 0x0F: "ILWR", 0x10: "RINIT", 0x1C: "ELENG", 0x1D: "ESUM",
     0x1E: "ERCPR", 0x1F: "EEXP"},
    {0x0D: "SQD", 0x0E: "WAITQ", 0x0F: "ISWR", 0x10: "RXOR", 0x1C: "ERLENG",
     0x1E: "WAITP"},
)

# EFU latencies (microVU_Lower.inl mVUanalyzeEFU1/2 calls)
EFU_LATENCY = {
    "ESADD": 11, "ERSADD": 18, "ELENG": 18, "ERLENG": 24, "EATAN": 54, "EATANxy": 54,
    "EATANxz": 54, "ESQRT": 12, "ERSQRT": 18, "ESIN": 29, "EEXP": 44, "ESUM": 12,
    "ERCPR": 12,
}
FDIV_LATENCY = {"DIV": 7, "SQRT": 7, "RSQRT": 13}
# EFU ops that read one element (fs.fsf) vs the vector (EFU2)
_EFU_SCALAR = {"EATAN", "EEXP", "ERCPR", "ERSQRT", "ESIN", "ESQRT"}

UPPER_NAMES = sorted({n for n in _UPPER_MAIN if n and not n.startswith("#")}
                     | {n for t in _UPPER_FD for n in t if n})
LOWER_NAMES = sorted(set(_LOWER_MAIN.values()) | set(_LOWER_OP.values())
                     | {n for t in _LOWER_T3 for n in t.values()})

BRANCHES = {"B", "BAL", "JR", "JALR", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"}
_FLAG_READERS = {"FSAND": "st", "FSEQ": "st", "FSOR": "st",
                 "FMAND": "mac", "FMEQ": "mac", "FMOR": "mac",
                 "FCAND": "clip", "FCEQ": "clip", "FCOR": "clip", "FCGET": "clip"}
_VU0_NOPS = {"XGKICK", "XTOP", "MFP", "WAITP"} | set(EFU_LATENCY)


# ----------------------------------------------------------------- decoding
def _sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


@dataclass
class VUOp:
    """One half of an instruction.  Field accessors follow microVU_Misc.h."""
    name: str           # canonical mnemonic ("MADDAx", "LQI", "NOP", "???")
    word: int
    unit: str           # "U" or "L"

    dest = property(lambda s: (s.word >> 21) & 15)      # bit 3 = x ... bit 0 = w
    ft = property(lambda s: (s.word >> 16) & 31)
    fs = property(lambda s: (s.word >> 11) & 31)
    fd = property(lambda s: (s.word >> 6) & 31)
    it = property(lambda s: (s.word >> 16) & 15)
    is_ = property(lambda s: (s.word >> 11) & 15)
    id_ = property(lambda s: (s.word >> 6) & 15)
    bc = property(lambda s: s.word & 3)
    fsf = property(lambda s: (s.word >> 21) & 3)
    ftf = property(lambda s: (s.word >> 23) & 3)
    imm5 = property(lambda s: _sx((s.word >> 6) & 31, 5))
    imm11 = property(lambda s: _sx(s.word & 0x7FF, 11))
    imm12 = property(lambda s: (((s.word >> 21) & 1) << 11) | (s.word & 0x7FF))
    imm15 = property(lambda s: (((s.word >> 21) & 0xF) << 11) | (s.word & 0x7FF))
    imm24 = property(lambda s: s.word & 0xFFFFFF)

    @property
    def known(self):
        return self.name != "???"


def decode_upper(w):
    w &= 0xFFFFFFFF
    n = _UPPER_MAIN[w & 0x3F]
    if n and n.startswith("#FD"):
        t = _UPPER_FD[w & 3]
        i = (w >> 6) & 31
        n = t[i] if i < len(t) else None
    return VUOp(n or "???", w, "U")


def decode_lower(w):
    w &= 0xFFFFFFFF
    op = w >> 25
    if op == 0x40:
        lo = w & 0x3F
        if lo >= 0x3C:
            n = _LOWER_T3[lo - 0x3C].get((w >> 6) & 31)
        else:
            n = _LOWER_OP.get(lo)
    else:
        n = _LOWER_MAIN.get(op)
    return VUOp(n or "???", w, "L")


@dataclass
class VUInsn:
    addr: int                 # byte address in micro memory
    lo: int
    hi: int
    upper: VUOp
    lower: VUOp | None        # None when the I bit makes the lower word an immediate

    ibit = property(lambda s: bool(s.hi & I_BIT))
    ebit = property(lambda s: bool(s.hi & E_BIT))
    mbit = property(lambda s: bool(s.hi & M_BIT))
    dbit = property(lambda s: bool(s.hi & D_BIT))
    tbit = property(lambda s: bool(s.hi & T_BIT))

    @property
    def lname(self):
        return self.lower.name if self.lower else "I"

    @property
    def is_branch(self):
        return self.lower is not None and self.lower.name in BRANCHES


def decode(lo, hi, addr=0):
    hi &= 0xFFFFFFFF
    lo &= 0xFFFFFFFF
    return VUInsn(addr, lo, hi, decode_upper(hi), None if hi & I_BIT else decode_lower(lo))


def decode_mem(code, addr):
    """Decode the instruction at byte address `addr` of a micro memory image."""
    lo, hi = struct.unpack_from("<II", code, addr)
    return decode(lo, hi, addr)


# ----------------------------------------------------------------- encoding (tests)
def _dest_bits(dest):
    if isinstance(dest, str):
        v = 0
        for ch in dest.lower():
            v |= 8 >> FIELDS.index(ch)
        return v
    return dest & 15


def _build_upper_index():
    idx = {}
    for i, n in enumerate(_UPPER_MAIN):
        if n and not n.startswith("#"):
            idx[n] = i
    for t, tab in enumerate(_UPPER_FD):
        for i, n in enumerate(tab):
            if n:
                idx[n] = 0x3C | t | (i << 6)
    return idx


def _build_lower_index():
    idx = {}
    for op, n in _LOWER_MAIN.items():
        idx[n] = op << 25
    for lo, n in _LOWER_OP.items():
        idx[n] = (0x40 << 25) | lo
    for t, tab in enumerate(_LOWER_T3):
        for i, n in tab.items():
            idx[n] = (0x40 << 25) | 0x3C | t | (i << 6)
    return idx


_UPPER_INDEX = _build_upper_index()
_LOWER_INDEX = _build_lower_index()


def encode_upper(name, dest="xyzw", fd=0, fs=0, ft=0, flags=0):
    """Assemble an upper op.  `flags` = I_BIT/E_BIT/... ORed in."""
    base = _UPPER_INDEX[name]
    w = base | (_dest_bits(dest) << 21) | ((ft & 31) << 16) | ((fs & 31) << 11)
    if (base & 0x3F) < 0x3C:
        w |= (fd & 31) << 6
    return (w | flags) & 0xFFFFFFFF


def encode_lower(name, dest="xyzw", ft=0, fs=0, fd=0, it=None, is_=None, id_=None,
                 fsf=0, ftf=0, imm=0):
    """Assemble a lower op.  it/is/id alias ft/fs/fd; imm is the op's immediate
    (imm5/imm11/imm12/imm15/imm24 or the branch offset in instructions)."""
    ft = ft if it is None else it
    fs = fs if is_ is None else is_
    fd = fd if id_ is None else id_
    w = _LOWER_INDEX[name]
    if name in ("DIV", "SQRT", "RSQRT", "MTIR", "RINIT", "RXOR") or name in _EFU_SCALAR:
        w |= ((fsf & 3) << 21) | ((ftf & 3) << 23)
    elif name in ("FSEQ", "FSSET", "FSAND", "FSOR"):
        w |= ((imm >> 11) & 1) << 21
    elif name in ("IADDIU", "ISUBIU"):
        w |= ((imm >> 11) & 0xF) << 21
    elif name not in ("FCEQ", "FCSET", "FCAND", "FCOR"):
        w |= _dest_bits(dest) << 21
    w |= ((ft & 31) << 16) | ((fs & 31) << 11)
    if name in ("FCEQ", "FCSET", "FCAND", "FCOR"):
        w |= imm & 0xFFFFFF
    elif name in ("FSEQ", "FSSET", "FSAND", "FSOR", "IADDIU", "ISUBIU",
                  "LQ", "SQ", "ILW", "ISW", "B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ",
                  "IBLEZ", "IBGEZ"):
        w |= imm & 0x7FF
    elif name == "IADDI":
        w |= (imm & 31) << 6
    elif (w >> 25) == 0x40 and (w & 0x3F) < 0x3C:
        w |= (fd & 31) << 6
    return w & 0xFFFFFFFF


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


# ----------------------------------------------------------------- disassembly
def _d(dest):
    return "".join(FIELDS[i] for i in range(4) if dest & (8 >> i))


_FMAC_BASES = ("MADD", "MSUB", "MINI", "MAX", "MUL", "ADD", "SUB")


def parse_fmac(name):
    """('MADD', acc, src) for FMAC mnemonics; src in '', 'x'..'w', 'i', 'q'."""
    for b in _FMAC_BASES:
        if name.startswith(b):
            rest = name[len(b):]
            acc = rest.startswith("A")
            if acc:
                rest = rest[1:]
            if rest in ("", "x", "y", "z", "w", "i", "q"):
                return b, acc, rest
    return None


def disasm_upper(op):
    n = op.name
    d = _d(op.dest)
    f = parse_fmac(n)
    if f:
        b, acc, src = f
        dst = "ACC" if acc else f"vf{op.fd:02d}"
        if src == "":
            t = f"vf{op.ft:02d}"
        elif src in "xyzw":
            t = f"vf{op.ft:02d}{src}"
        else:
            t = src.upper()
        return f"{n}.{d} {dst}, vf{op.fs:02d}, {t}"
    if n in ("OPMULA",):
        return f"OPMULA.{d} ACC, vf{op.fs:02d}, vf{op.ft:02d}"
    if n == "OPMSUB":
        return f"OPMSUB.{d} vf{op.fd:02d}, vf{op.fs:02d}, vf{op.ft:02d}"
    if n == "CLIP":
        return f"CLIPw.xyz vf{op.fs:02d}, vf{op.ft:02d}w"
    if n == "NOP":
        return "NOP"
    if n.startswith(("ITOF", "FTOI", "ABS")):
        return f"{n}.{d} vf{op.ft:02d}, vf{op.fs:02d}"
    return f"??? {op.word:08X}"


def disasm_lower(op):
    n = op.name
    d = _d(op.dest)
    F = lambda r: f"vf{r:02d}"
    I = lambda r: f"vi{r:02d}"
    if n in ("LQ",):
        return f"LQ.{d} {F(op.ft)}, {op.imm11}({I(op.is_)})"
    if n == "SQ":
        return f"SQ.{d} {F(op.fs)}, {op.imm11}({I(op.it)})"
    if n in ("LQI", "LQD"):
        return f"{n}.{d} {F(op.ft)}, ({I(op.is_)}{'++' if n == 'LQI' else '--'})"
    if n in ("SQI", "SQD"):
        return f"{n}.{d} {F(op.fs)}, ({I(op.it)}{'++' if n == 'SQI' else '--'})"
    if n in ("ILW", "ISW"):
        return f"{n}.{d} {I(op.it)}, {op.imm11}({I(op.is_)})"
    if n in ("ILWR", "ISWR"):
        return f"{n}.{d} {I(op.it)}, ({I(op.is_)})"
    if n in ("IADD", "ISUB", "IAND", "IOR"):
        return f"{n} {I(op.id_)}, {I(op.is_)}, {I(op.it)}"
    if n == "IADDI":
        return f"IADDI {I(op.it)}, {I(op.is_)}, {op.imm5}"
    if n in ("IADDIU", "ISUBIU"):
        return f"{n} {I(op.it)}, {I(op.is_)}, 0x{op.imm15:x}"
    if n in ("MOVE", "MR32"):
        if n == "MOVE" and op.word == 0x8000033C:
            return "NOP"
        return f"{n}.{d} {F(op.ft)}, {F(op.fs)}"
    if n == "MFIR":
        return f"MFIR.{d} {F(op.ft)}, {I(op.is_)}"
    if n == "MTIR":
        return f"MTIR {I(op.it)}, {F(op.fs)}{FIELDS[op.fsf]}"
    if n == "MFP":
        return f"MFP.{d} {F(op.ft)}, P"
    if n == "DIV":
        return f"DIV Q, {F(op.fs)}{FIELDS[op.fsf]}, {F(op.ft)}{FIELDS[op.ftf]}"
    if n == "SQRT":
        return f"SQRT Q, {F(op.ft)}{FIELDS[op.ftf]}"
    if n == "RSQRT":
        return f"RSQRT Q, {F(op.fs)}{FIELDS[op.fsf]}, {F(op.ft)}{FIELDS[op.ftf]}"
    if n in ("WAITQ", "WAITP"):
        return n
    if n in EFU_LATENCY:
        if n in _EFU_SCALAR:
            return f"{n} P, {F(op.fs)}{FIELDS[op.fsf]}"
        return f"{n} P, {F(op.fs)}"
    if n in ("FMAND", "FMEQ", "FMOR"):
        return f"{n} {I(op.it)}, {I(op.is_)}"
    if n in ("FSAND", "FSEQ", "FSOR"):
        return f"{n} {I(op.it)}, 0x{op.imm12:x}"
    if n == "FSSET":
        return f"FSSET 0x{op.imm12:x}"
    if n in ("FCAND", "FCEQ", "FCOR"):
        return f"{n} vi01, 0x{op.imm24:x}"
    if n == "FCSET":
        return f"FCSET 0x{op.imm24:x}"
    if n == "FCGET":
        return f"FCGET {I(op.it)}"
    if n == "B":
        return f"B {op.imm11:+d}"
    if n == "BAL":
        return f"BAL {I(op.it)}, {op.imm11:+d}"
    if n == "JR":
        return f"JR {I(op.is_)}"
    if n == "JALR":
        return f"JALR {I(op.it)}, {I(op.is_)}"
    if n in ("IBEQ", "IBNE"):
        return f"{n} {I(op.it)}, {I(op.is_)}, {op.imm11:+d}"
    if n in ("IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        return f"{n} {I(op.is_)}, {op.imm11:+d}"
    if n == "XGKICK":
        return f"XGKICK {I(op.is_)}"
    if n in ("XTOP", "XITOP"):
        return f"{n} {I(op.it)}"
    if n == "RINIT":
        return f"RINIT R, {F(op.fs)}{FIELDS[op.fsf]}"
    if n == "RXOR":
        return f"RXOR R, {F(op.fs)}{FIELDS[op.fsf]}"
    if n in ("RGET", "RNEXT"):
        return f"{n}.{d} {F(op.ft)}, R"
    return f"??? {op.word:08X}"


def disasm(ins):
    bits = "".join(c for c, b in (("I", ins.ibit), ("E", ins.ebit), ("M", ins.mbit),
                                  ("D", ins.dbit), ("T", ins.tbit)) if b)
    up = disasm_upper(ins.upper)
    lo = f"LOI {u2f(ins.lo):g} (0x{ins.lo:08X})" if ins.lower is None else disasm_lower(ins.lower)
    return f"{up}{' [' + bits + ']' if bits else ''} | {lo}"


# ----------------------------------------------------------------- register usage
def _mask_fields(m):
    return [i for i in range(4) if m & (8 >> i)]


def upper_reads(op):
    """[(vf, fieldmask)] read by an upper op (analyzeReg1/3/4)."""
    n = op.name
    f = parse_fmac(n)
    if f:
        _, _, src = f
        r = [(op.fs, op.dest)]
        if src == "":
            r.append((op.ft, op.dest))
        elif src in "xyzw":
            r.append((op.ft, 8 >> "xyzw".index(src)))
        return r
    if n in ("OPMULA", "OPMSUB"):
        return [(op.fs, op.dest), (op.ft, op.dest)]
    if n.startswith(("ITOF", "FTOI")) or n == "ABS":
        return [(op.fs, op.dest)]
    if n == "CLIP":
        return [(op.fs, op.dest), (op.ft, 1)]
    return []


def upper_write(op):
    """(vf, mask) written (0 = none/ACC)."""
    n = op.name
    f = parse_fmac(n)
    if f:
        return (0, 0) if f[1] else (op.fd, op.dest)
    if n == "OPMSUB":
        return (op.fd, op.dest)
    if n.startswith(("ITOF", "FTOI")) or n == "ABS":
        return (op.ft, op.dest)
    return (0, 0)


def upper_sets_flags(op):
    f = parse_fmac(op.name)
    if f:
        return f[0] not in ("MAX", "MINI")
    return op.name in ("OPMULA", "OPMSUB")


def lower_info(op, vu=1):
    """Register usage of a lower op:
    dict(vf_r=[(reg,mask)], vf_w=(reg,mask), vi_r=[reg], vi_w=(reg, latency))."""
    n = op.name
    r, w, ir, iw = [], (0, 0), [], (0, 0)
    one = lambda f: 8 >> f
    if vu == 0 and n in _VU0_NOPS:
        return dict(vf_r=r, vf_w=w, vi_r=ir, vi_w=iw)
    if n == "LQ":
        ir, w = [op.is_], (op.ft, op.dest)
    elif n in ("LQI", "LQD"):
        ir, w, iw = [op.is_], (op.ft, op.dest), (op.is_, 1)
    elif n == "SQ":
        r, ir = [(op.fs, op.dest)], [op.it]
    elif n in ("SQI", "SQD"):
        r, ir, iw = [(op.fs, op.dest)], [op.it], (op.it, 1)
    elif n in ("ILW", "ILWR"):
        ir, iw = [op.is_], (op.it, 4)
    elif n in ("ISW", "ISWR"):
        ir = [op.is_, op.it]
    elif n in ("IADD", "ISUB", "IAND", "IOR"):
        ir, iw = [op.is_, op.it], (op.id_, 1)
    elif n in ("IADDI", "IADDIU", "ISUBIU"):
        ir, iw = [op.is_], (op.it, 1)
    elif n == "MOVE":
        r, w = [(op.fs, op.dest)], (op.ft, op.dest)
    elif n == "MR32":
        m = op.dest
        rot =(8 if m & 1 else 0) | (4 if m & 8 else 0) | (2 if m & 4 else 0) | (1 if m & 2 else 0)
        r, w = [(op.fs, rot)], (op.ft, op.dest)
    elif n == "MFIR":
        ir, w = [op.is_], (op.ft, op.dest)
    elif n == "MTIR":
        r, iw = [(op.fs, one(op.fsf))], (op.it, 1)
    elif n == "MFP":
        w = (op.ft, op.dest)
    elif n in ("DIV", "RSQRT"):
        r = [(op.fs, one(op.fsf)), (op.ft, one(op.ftf))]
    elif n == "SQRT":
        r = [(op.ft, one(op.ftf))]
    elif n in EFU_LATENCY:
        r = [(op.fs, one(op.fsf))] if n in _EFU_SCALAR else [(op.fs, op.dest)]
    elif n in ("RINIT", "RXOR"):
        r = [(op.fs, one(op.fsf))]
    elif n in ("RGET", "RNEXT"):
        w = (op.ft, op.dest)
    elif n in ("FMAND", "FMEQ", "FMOR"):
        ir, iw = [op.is_], (op.it, 1)
    elif n in ("FSAND", "FSEQ", "FSOR", "FCGET"):
        iw = (op.it, 1)
    elif n in ("FCAND", "FCEQ", "FCOR"):
        iw = (1, 1)
    elif n in ("BAL",):
        iw = (op.it, 1)
    elif n == "JALR":
        ir, iw = [op.is_], (op.it, 1)
    elif n == "JR":
        ir = [op.is_]
    elif n in ("IBEQ", "IBNE"):
        ir = [op.is_, op.it]
    elif n in ("IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        ir = [op.is_]
    elif n == "XGKICK":
        ir = [op.is_]
    elif n in ("XTOP", "XITOP"):
        iw = (op.it, 1)
    return dict(vf_r=r, vf_w=w, vi_r=ir, vi_w=iw)


# ----------------------------------------------------------------- program discovery
@dataclass
class VUProgram:
    vu: int
    entry: int
    code: bytes                     # the micro memory image the program was found in
    insns: dict = field(default_factory=dict)    # addr -> VUInsn (reachable)
    labels: set = field(default_factory=set)     # addresses that need a C label
    delay: dict = field(default_factory=dict)    # delay-slot addr -> owner addr
    warnings: list = field(default_factory=list)
    extra_entries: tuple = ()

    @property
    def pc_mask(self):
        return len(self.code) - 8

    def ranges(self):
        """Covered [start, end) byte ranges, ascending."""
        out = []
        for a in sorted(self.insns):
            if out and out[-1][1] == a:
                out[-1][1] = a + 8
            else:
                out.append([a, a + 8])
        return [tuple(r) for r in out]

    def code_words(self):
        words = []
        for s, e in self.ranges():
            words += struct.unpack_from(f"<{(e - s) // 4}I", self.code, s)
        return words

    def hash(self):
        """FNV-1a 64 over the covered bytes (ranges in ascending order)."""
        h = 0xCBF29CE484222325
        for s, e in self.ranges():
            for b in self.code[s:e]:
                h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        return h

    @property
    def name(self):
        return f"vu{self.vu}_{self.hash():016x}_{self.entry:04x}"


def discover(code, entry, vu=1, extra_entries=()):
    """Find every instruction reachable from `entry` (and `extra_entries`) in the micro
    memory image `code` (16 KiB for VU1, 4 KiB for VU0)."""
    code = bytes(code)
    if len(code) not in (0x1000, 0x4000):
        raise ValueError("micro memory must be 4 KiB (VU0) or 16 KiB (VU1)")
    prog = VUProgram(vu, entry, code, extra_entries=tuple(extra_entries))
    mask = len(code) - 8
    nxt = lambda a: (a + 8) & mask
    work = [entry & mask] + [e & mask for e in extra_entries]
    prog.labels.update(work)
    seen_start = set()
    while work:
        a = work.pop()
        if a in seen_start:
            continue
        seen_start.add(a)
        while True:
            if a in prog.insns and a not in prog.delay:
                break
            ins = decode_mem(code, a)
            prog.insns[a] = ins
            if not ins.upper.known:
                raise VUUnsupported(f"{a:04X}: unknown upper op {ins.hi:08X}")
            if ins.lower is not None and not ins.lower.known:
                raise VUUnsupported(f"{a:04X}: unknown lower op {ins.lo:08X}")
            if ins.is_branch:
                d = nxt(a)
                dins = decode_mem(code, d)
                if dins.is_branch:
                    raise VUUnsupported(f"{a:04X}: branch in branch delay slot")
                if dins.ebit:
                    raise VUUnsupported(f"{a:04X}: E-bit in branch delay slot")
                if ins.ebit:
                    raise VUUnsupported(f"{a:04X}: branch with E bit")
                prog.insns[d] = dins
                prog.delay[d] = a
                after = nxt(d)
                n = ins.lower.name
                if n in ("B", "BAL") or n.startswith("IB"):
                    t = (a + 8 + ins.lower.imm11 * 8) & mask
                    prog.labels.add(t)
                    work.append(t)
                if n in ("BAL", "JALR") or n.startswith("IB"):
                    # BAL/JALR: the return address may be reached by a later JR
                    prog.labels.add(after)
                    work.append(after)
                break
            if ins.ebit:
                d = nxt(a)
                dins = decode_mem(code, d)
                if dins.is_branch:
                    prog.warnings.append(f"{d:04X}: branch in E-bit delay slot (ignored, as microVU)")
                prog.insns[d] = dins
                prog.delay[d] = a
                break
            if ins.tbit:
                prog.labels.add(nxt(a))        # resume point after a T-bit stop
            a = nxt(a)
            if a in prog.insns and a not in prog.delay:
                prog.labels.add(a)
                break
            if a in prog.delay:
                # falling into an address that is also a delay slot: needs its own copy
                prog.labels.add(a)
    # every delay slot that is also reached normally needs a label too
    return prog


# ----------------------------------------------------------------- static pipeline model
class _Pipe:
    """Static stall model of one straight-line path (microVU pass 1: analyzeReg*,
    mVUincCycles, mVUsetCycles).  Reset (empty pipeline) at every label."""

    def __init__(self):
        self.vf = {}        # (reg, field) -> remaining latency
        self.vi = {}        # reg -> remaining latency
        self.hist = []      # [(insn, stall, slot)] most recent last
        self.kick = False   # an XGKICK may be pending (statically)

    def copy(self):
        p = _Pipe()
        p.vf, p.vi, p.hist, p.kick = dict(self.vf), dict(self.vi), list(self.hist), self.kick
        return p

    def _dec(self, x):
        if x <= 0:
            return
        self.vf = {k: v - x for k, v in self.vf.items() if v - x > 0}
        self.vi = {k: v - x for k, v in self.vi.items() if v - x > 0}

    def step(self, ins, vu):
        self._dec(1)
        stall = 0
        reads = upper_reads(ins.upper)
        li = lower_info(ins.lower, vu) if ins.lower else None
        if li:
            reads = reads + li["vf_r"]
        for reg, m in reads:
            if reg:
                for f in _mask_fields(m):
                    stall = max(stall, self.vf.get((reg, f), 0))
        if li:
            for reg in li["vi_r"]:
                if reg:
                    stall = max(stall, self.vi.get(reg, 0))
        self._dec(stall)
        reg, m = upper_write(ins.upper)
        if reg:
            for f in _mask_fields(m):
                self.vf[(reg, f)] = max(self.vf.get((reg, f), 0), 4)
        if li:
            reg, m = li["vf_w"]
            if reg:
                for f in _mask_fields(m):
                    self.vf[(reg, f)] = max(self.vf.get((reg, f), 0), 4)
            reg, lat = li["vi_w"]
            if reg:
                self.vi[reg] = max(self.vi.get(reg, 0), lat)
        return stall


# ----------------------------------------------------------------- C emission
def _hex(v):
    return f"0x{v:X}u"


_VF0 = ("0.0f", "0.0f", "0.0f", "1.0f")
_VF0U = ("0x0u", "0x0u", "0x0u", "0x3F800000u")


def _vf(r, i):
    return _VF0[i] if r == 0 else f"st->vf[{r}].F[{i}]"


def _vfu(r, i):
    return _VF0U[i] if r == 0 else f"st->vf[{r}].UW[{i}]"


def _vi(r):
    return "0u" if r == 0 else f"(uint32_t)st->vi[{r}]"


# mVU_UPPER per-opcode operand clamps (mode 1 only), cFt = 1, cFs = 2
def _clamp_type(base, acc, src, dest):
    ps = dest == 0xF
    if base == "SUB" and not acc:
        return 3 if ps else 0
    if base == "MUL":
        if not acc:
            return 3 if ps else 2
        if src in ("x", "y", "z"):
            return 2
        if src == "w":
            return 3 if ps else 2
        return 0
    if base == "MADD":
        return 2 if src in ("x", "y", "z", "w") else 0
    return 0


class _Emitter:
    def __init__(self, prog, clamp_mode):
        self.p = prog
        self.vu = prog.vu
        self.clamp = clamp_mode
        self.out = []
        self.ntmp = 0
        names = {i.lower.name for i in prog.insns.values() if i.lower}
        self.track = {k: any(_FLAG_READERS.get(n) == k for n in names)
                      for k in ("st", "mac", "clip")}
        self.uses_fdiv = bool(names & set(FDIV_LATENCY))
        self.uses_kick = self.vu == 1 and "XGKICK" in names
        self.te_bit = 0x800 if self.vu == 1 else 0x8
        self.has_tbit = any(i.tbit for i in prog.insns.values())

    # ---- helpers
    def tmp(self, prefix="t"):
        self.ntmp += 1
        return f"{prefix}{self.ntmp}"

    def fp_write(self, which, val):
        if self.track[which]:
            return f"vu_fp_write(&x.{which}, {val}, x.cyc);"
        return f"x.{which}.latest = {val};"

    def lbl(self, a):
        return f"L_{a:04X}"

    # ---- upper
    def upper(self, ins):
        """C lines computing the upper op into U (no commit) and the commit lines."""
        op = ins.upper
        n = op.name
        pre, post = [], []
        f = parse_fmac(n)
        flags = upper_sets_flags(op)
        dest = op.dest
        fl = _mask_fields(dest)
        if f:
            base, acc, src = f
            ct = _clamp_type(base, acc, src, dest) if self.clamp == 1 else 0
            if src == "q":
                pre.append("{ float qv = vu_q(&x);")
            for i in fl:
                if src == "":
                    t, tu = _vf(op.ft, i), _vfu(op.ft, i)
                elif src in "xyzw":
                    j = "xyzw".index(src)
                    t, tu = _vf(op.ft, j), _vfu(op.ft, j)
                elif src == "i":
                    t, tu = "st->i.f", "st->i.u"
                else:
                    t, tu = "qv", "vu_fu(qv)"
                s, su = _vf(op.fs, i), _vfu(op.fs, i)
                if ct & 2:
                    s = f"vu_cl1({s})"
                if ct & 1:
                    t = f"vu_cl1({t})"
                a = f"st->acc.F[{i}]"
                if base in ("MAX", "MINI"):
                    fn = "vu_max_u" if base == "MAX" else "vu_min_u"
                    pre.append(f"U.UW[{i}] = {fn}({su}, {tu});")
                    continue
                if base == "SUB" and src == "" and op.fs == op.ft:
                    e = "0.0f"                       # doSafeSub
                elif base == "ADD":
                    e = f"VU_ADD({s}, {t})"
                elif base == "SUB":
                    e = f"VU_SUB({s}, {t})"
                elif base == "MUL":
                    e = f"VU_MUL({s}, {t})"
                elif base == "MADD":
                    e = f"VU_ADD(VU_MUL({s}, {t}), {a})" if not acc else f"VU_ADD({a}, VU_MUL({s}, {t}))"
                else:  # MSUB
                    e = f"VU_SUB({a}, VU_MUL({s}, {t}))"
                pre.append(f"U.F[{i}] = {e};")
            if src == "q":
                pre.append("}")
            dst = "st->acc" if acc else (f"st->vf[{op.fd}]" if op.fd else None)
            if dst:
                for i in fl:
                    post.append(f"{dst}.UW[{i}] = U.UW[{i}];")
        elif n in ("OPMULA", "OPMSUB"):
            # Fs.yzx * Ft.zxy (pshufd 0xC9 / 0xD2)
            sh_s, sh_t = (1, 2, 0, 3), (2, 0, 1, 3)
            for i in fl:
                m = f"VU_MUL({_vf(op.fs, sh_s[i])}, {_vf(op.ft, sh_t[i])})"
                if n == "OPMULA":
                    pre.append(f"U.F[{i}] = {m};")
                else:
                    pre.append(f"U.F[{i}] = VU_SUB(st->acc.F[{i}], {m});")
            dst = "st->acc" if n == "OPMULA" else (f"st->vf[{op.fd}]" if op.fd else None)
            if dst:
                for i in fl:
                    post.append(f"{dst}.UW[{i}] = U.UW[{i}];")
        elif n == "ABS":
            if op.ft:
                for i in fl:
                    pre.append(f"U.UW[{i}] = {_vfu(op.fs, i)} & 0x7FFFFFFFu;")
                    post.append(f"st->vf[{op.ft}].UW[{i}] = U.UW[{i}];")
        elif n.startswith("FTOI"):
            sc = {"0": None, "4": "16.0f", "12": "4096.0f", "15": "32768.0f"}[n[4:]]
            if op.ft:
                for i in fl:
                    v = _vf(op.fs, i) if sc is None else f"{_vf(op.fs, i)} * {sc}"
                    pre.append(f"U.SW[{i}] = vu_ftoi({v});")
                    post.append(f"st->vf[{op.ft}].UW[{i}] = U.UW[{i}];")
        elif n.startswith("ITOF"):
            sc = {"0": None, "4": "0.0625f", "12": "0.000244140625f",
                  "15": "0.000030517578125f"}[n[4:]]
            if op.ft:
                for i in fl:
                    src = "0" if op.fs == 0 and i < 3 else (
                        "1065353216" if op.fs == 0 else f"st->vf[{op.fs}].SW[{i}]")
                    v = f"(float){src}" if sc is None else f"(float){src} * {sc}"
                    pre.append(f"U.F[{i}] = {v};")
                    post.append(f"st->vf[{op.ft}].UW[{i}] = U.UW[{i}];")
        elif n == "CLIP":
            args = ", ".join(_vfu(op.fs, i) for i in range(3))
            pre.append(f"{{ uint32_t cf = vu_clip_bits({args}, {_vfu(op.ft, 3)});")
            pre.append("  " + self.fp_write("clip", "((x.clip.latest << 6) | cf) & 0xFFFFFFu") + " }")
        elif n == "NOP":
            pass
        else:
            raise VUUnsupported(f"{ins.addr:04X}: upper {n}")
        if flags:
            pre.append(f"vu_fmac_flags(&x, vu_macf(&U, {_hex(dest)}), {int(self.track['mac'])}, "
                       f"{int(self.track['st'])});")
        return pre, post

    # ---- lower
    def lower(self, ins, cond_state):
        """Lines for the lower op.  Returns (lines, vf_commit_lines, branch) where
        branch = None | ('cond', expr, target) | ('jump', expr) | ('b', target)."""
        op = ins.lower
        if op is None:
            return [], [], None
        n = op.name
        vu = self.vu
        L, C = [], []
        br = None
        mq = 0x3FF if vu == 1 else 0xFF
        fl = _mask_fields(op.dest)
        mask = self.p.pc_mask
        if vu == 0 and n in _VU0_NOPS:
            return [f"/* {n}: NOP on VU0 */"], [], None

        def setvi(r, expr):
            if r:
                L.append(f"st->vi[{r}] = (uint16_t)({expr});")

        def qaddr(expr):
            return f"vu_qw(st, ({expr}) & {_hex(mq)})"

        if n in ("LQ", "LQI", "LQD"):
            if n == "LQ":
                a = f"{_vi(op.is_)} + {op.imm11}"
            elif n == "LQI":
                a = _vi(op.is_)
            else:
                a = f"{_vi(op.is_)} - 1u" if op.is_ else "0u"
            if op.ft:
                p = self.tmp("p")
                L.append(f"{{ const uint8_t *{p} = {qaddr(a)};")
                for i in fl:
                    L.append(f"  memcpy(&L.UW[{i}], {p} + {4 * i}, 4);")
                L.append("}")
                for i in fl:
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
            if n == "LQI":
                setvi(op.is_, f"st->vi[{op.is_}] + 1u")
            elif n == "LQD":
                setvi(op.is_, f"st->vi[{op.is_}] - 1u")
            return L, C, None
        if n in ("SQ", "SQI", "SQD"):
            if n == "SQ":
                a = f"{_vi(op.it)} + {op.imm11}"
            elif n == "SQI":
                a = _vi(op.it)
            else:
                a = f"{_vi(op.it)} - 1u" if op.it else "0u"
            p = self.tmp("p")
            L.append(f"{{ uint8_t *{p} = {qaddr(a)};")
            for i in fl:
                L.append(f"  {{ uint32_t v = {_vfu(op.fs, i)}; memcpy({p} + {4 * i}, &v, 4); }}")
            L.append("}")
            if n == "SQI":
                setvi(op.it, f"st->vi[{op.it}] + 1u")
            elif n == "SQD":
                setvi(op.it, f"st->vi[{op.it}] - 1u")
            return L, C, None
        if n in ("ILW", "ILWR"):
            a = f"{_vi(op.is_)} + {op.imm11}" if n == "ILW" else _vi(op.is_)
            off = 4 * (fl[0] if fl else 3)
            if op.it:
                L.append(f"{{ uint16_t v; memcpy(&v, {qaddr(a)} + {off}, 2); st->vi[{op.it}] = v; }}")
            return L, C, None
        if n in ("ISW", "ISWR"):
            a = f"{_vi(op.is_)} + {op.imm11}" if n == "ISW" else _vi(op.is_)
            p = self.tmp("p")
            L.append(f"{{ uint8_t *{p} = {qaddr(a)}; uint32_t v = {_vi(op.it)};")
            for i in fl:
                L.append(f"  memcpy({p} + {4 * i}, &v, 4);")
            L.append("}")
            return L, C, None
        if n in ("IADD", "ISUB", "IAND", "IOR"):
            o = {"IADD": "+", "ISUB": "-", "IAND": "&", "IOR": "|"}[n]
            setvi(op.id_, f"{_vi(op.is_)} {o} {_vi(op.it)}")
            return L, C, None
        if n == "IADDI":
            setvi(op.it, f"{_vi(op.is_)} + (uint32_t){op.imm5}")
            return L, C, None
        if n == "IADDIU":
            setvi(op.it, f"{_vi(op.is_)} + {op.imm15}u")
            return L, C, None
        if n == "ISUBIU":
            setvi(op.it, f"{_vi(op.is_)} - {op.imm15}u")
            return L, C, None
        if n == "MOVE":
            if op.ft and op.ft != op.fs:
                for i in fl:
                    L.append(f"L.UW[{i}] = {_vfu(op.fs, i)};")
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
            return L, C, None
        if n == "MR32":
            if op.ft:
                for i in fl:
                    L.append(f"L.UW[{i}] = {_vfu(op.fs, (i + 1) & 3)};")
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
            return L, C, None
        if n == "MFIR":
            if op.ft:
                for i in fl:
                    L.append(f"L.SW[{i}] = (int32_t)(int16_t)st->vi[{op.is_}];" if op.is_
                             else f"L.SW[{i}] = 0;")
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
            return L, C, None
        if n == "MTIR":
            setvi(op.it, f"{_vfu(op.fs, op.fsf)} & 0xFFFFu")
            return L, C, None
        if n == "MFP":
            if op.ft:
                L.append("{ float pv = vu_p(&x);")
                for i in fl:
                    L.append(f"  L.F[{i}] = pv;")
                L.append("}")
                for i in fl:
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
            return L, C, None
        if n in FDIV_LATENCY:
            lat = FDIV_LATENCY[n]
            s, t = _vf(op.fs, op.fsf), _vf(op.ft, op.ftf)
            if n == "DIV":
                c = f"vu_div_calc({s}, {t}, &fl)"
            elif n == "SQRT":
                c = f"vu_sqrt_calc({t}, &fl)"
            else:
                c = f"vu_rsqrt_calc({s}, {t}, &fl)"
            L.append(f"{{ uint32_t fl; float qn = {c}; vu_fdiv_issue(&x, qn, fl, {lat}, "
                     f"{int(self.track['st'])}); }}")
            return L, C, None
        if n == "WAITQ" or n == "WAITP":
            return [f"/* {n}: stall handled above */"], C, None
        if n in EFU_LATENCY:
            lat = EFU_LATENCY[n]
            if n in _EFU_SCALAR:
                arg = _vf(op.fs, op.fsf)
            else:
                arg = ", ".join(_vf(op.fs, i) for i in range(4))
            L.append(f"vu_efu_issue(&x, vu_{n.lower()}({arg}), {lat});")
            return L, C, None
        if n == "FMAND":
            setvi(op.it, f"{_vi(op.is_)} & vu_fp_read(&x.mac, x.cyc)")
        elif n == "FMOR":
            setvi(op.it, f"{_vi(op.is_)} | vu_fp_read(&x.mac, x.cyc)")
        elif n == "FMEQ":
            setvi(op.it, f"{_vi(op.is_)} == (vu_fp_read(&x.mac, x.cyc) & 0xFFFFu)")
        elif n == "FSAND":
            setvi(op.it, f"vu_fp_read(&x.st, x.cyc) & {_hex(op.imm12)}")
        elif n == "FSOR":
            setvi(op.it, f"vu_fp_read(&x.st, x.cyc) | {_hex(op.imm12)}")
        elif n == "FSEQ":
            setvi(op.it, f"(vu_fp_read(&x.st, x.cyc) & 0xFFFu) == {_hex(op.imm12)}")
        elif n == "FSSET":
            L.append(self.fp_write("st", f"(x.st.latest & 0x3Fu) | {_hex(op.imm12 & 0xFC0)}"))
        elif n == "FCAND":
            L.append(f"st->vi[1] = (uint16_t)((vu_fp_read(&x.clip, x.cyc) & {_hex(op.imm24)}) != 0);")
        elif n == "FCEQ":
            L.append(f"st->vi[1] = (uint16_t)((vu_fp_read(&x.clip, x.cyc) & 0xFFFFFFu) == {_hex(op.imm24)});")
        elif n == "FCOR":
            L.append(f"st->vi[1] = (uint16_t)(((vu_fp_read(&x.clip, x.cyc) | {_hex(op.imm24)}) & 0xFFFFFFu) == 0xFFFFFFu);")
        elif n == "FCSET":
            L.append(self.fp_write("clip", _hex(op.imm24)))
        elif n == "FCGET":
            setvi(op.it, "vu_fp_read(&x.clip, x.cyc) & 0xFFFu")
        elif n == "XTOP":
            setvi(op.it, "st->vif_top & 0xFFFFu")
        elif n == "XITOP":
            setvi(op.it, f"st->vif_itop & {_hex(mq)}")
        elif n == "XGKICK":
            L.append(f"vu_kick_flush(&x, st); x.kick_pend = 1; x.kick_addr = ({_vi(op.is_)} & 0x3FFu) << 4;")
        elif n == "RINIT":
            L.append(f"st->r = 0x3F800000u | ({_vfu(op.fs, op.fsf)} & 0x7FFFFFu);")
        elif n == "RXOR":
            L.append(f"st->r ^= {_vfu(op.fs, op.fsf)} & 0x7FFFFFu;")
        elif n in ("RGET", "RNEXT"):
            if n == "RNEXT":
                L.append("st->r = vu_rnext(st->r);")
            if op.ft:
                for i in fl:
                    L.append(f"L.UW[{i}] = st->r;")
                    C.append(f"st->vf[{op.ft}].UW[{i}] = L.UW[{i}];")
        elif n in BRANCHES:
            link = ((ins.addr + 16) & mask) // 8
            if n == "B":
                br = ("b", (ins.addr + 8 + op.imm11 * 8) & mask)
            elif n == "BAL":
                setvi(op.it, f"{link}u")
                br = ("b", (ins.addr + 8 + op.imm11 * 8) & mask)
            elif n in ("JR", "JALR"):
                jt = self.tmp("jt")
                L.append(f"{jt} = ({_vi(op.is_)} * 8u) & {_hex(mask)};")
                if n == "JALR":
                    setvi(op.it, f"{link}u")
                br = ("jump", jt)
            else:
                s_expr, t_expr = cond_state
                cmp = {"IBEQ": f"{s_expr} == {t_expr}", "IBNE": f"{s_expr} != {t_expr}",
                       "IBLTZ": f"(int16_t){s_expr} < 0", "IBGTZ": f"(int16_t){s_expr} > 0",
                       "IBLEZ": f"(int16_t){s_expr} <= 0", "IBGEZ": f"(int16_t){s_expr} >= 0"}[n]
                c = self.tmp("br")
                L.append(f"{c} = ({cmp});")
                br = ("cond", c, (ins.addr + 8 + op.imm11 * 8) & mask)
        else:
            raise VUUnsupported(f"{ins.addr:04X}: lower {n}")
        return L, C, br

    # ---- branch VI quirk (analyzeBranchVI)
    def branch_regs(self, ins, pipe, stall, slot_out):
        """Returns (is_expr, it_expr) for a conditional branch, inserting VI backups
        into earlier instructions' pre-slots when the quirk applies."""
        op = ins.lower
        regs = [op.is_, op.it] if op.name in ("IBEQ", "IBNE") else [op.is_]
        exprs = []
        for r in regs:
            e = f"(uint16_t){_vi(r)}"
            if r and stall == 0:
                j = None
                cyc = 0
                i = 0
                while i < 4 and cyc < 4:
                    if i >= len(pipe.hist):
                        break
                    hins, hstall, hslot = pipe.hist[-1 - i]
                    li = lower_info(hins.lower, self.vu) if hins.lower else None
                    if li and li["vi_w"][0] == r:
                        if hins.lower.name in _FLAG_READERS:
                            break
                        j = i
                    elif i == 0:
                        break
                    cyc += hstall + 1
                    i += 1
                if j is not None:
                    hins, hstall, hslot = pipe.hist[-1 - j]
                    v = self.tmp("vib")
                    hslot.append(f"uint16_t {v} = st->vi[{r}];  /* branch VI delay (analyzeBranchVI) */")
                    self.decls.append(v)
                    e = v
            exprs.append(e)
        if len(exprs) == 1:
            exprs.append("0")
        return exprs

    # ---- one instruction
    def insn(self, ins, pipe, out, end_kick=True):
        """Emit one instruction into `out` (list); returns the branch info."""
        p = self.p
        stall = pipe.step(ins, self.vu)
        slot = []                      # lines inserted later (VI backups) go here
        out.append(slot)
        out.append(f"/* {ins.addr:04X}: {disasm(ins)} */")
        out.append(f"x.cyc += {1 + stall}u;")
        ln = ins.lname
        if ln in FDIV_LATENCY or ln == "WAITQ":
            out.append("vu_q_stall(&x);")
        if self.vu == 1 and (ln in EFU_LATENCY or ln == "WAITP"):
            out.append("vu_p_stall(&x);")
        cond_state = None
        if ins.lower is not None and ins.lower.name in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ",
                                                        "IBLEZ", "IBGEZ"):
            cond_state = self.branch_regs(ins, pipe, stall, slot)
        pre, post = self.upper(ins)
        L, C, br = self.lower(ins, cond_state)
        # status order inside one instruction: FMAC flags, FDIV I/D, FSSET (mVU)
        body = list(pre)
        if self.uses_fdiv:
            body.append(f"vu_divflag_sync(&x, {int(self.track['st'])});")
        body += L
        # both halves write the same VF register: the lower write is dropped
        ureg, _ = upper_write(ins.upper)
        lreg = lower_info(ins.lower, self.vu)["vf_w"][0] if ins.lower else 0
        if not (ureg and ureg == lreg):
            body += C
        body += post
        if ins.lower is None:
            imm = ins.lo
            if (imm & 0x7FFFFFFF) >= 0x7F800000:   # doIbit: clamp I (CHECK_VU_OVERFLOW)
                imm = (imm & 0x80000000) | 0x7F7FFFFF if self.clamp >= 1 else imm
            body.append(f"st->i.u = {_hex(imm)};")
        out.append("{")
        out += ["    " + s for s in body]
        out.append("}")
        if pipe.kick and self.uses_kick and not (ins.lower and ins.lower.name == "XGKICK"):
            out.append("vu_kick_flush(&x, st);")
        pipe.kick = self.uses_kick and ins.lower is not None and ins.lower.name == "XGKICK"
        pipe.hist.append((ins, stall, slot))
        return br

    def exit(self, code, pc):
        return f"return vu_ctx_end(&x, st, {code}, {_hex(pc)});"

    def tbit_check(self, ins, out, next_pc):
        if ins.tbit:
            out.append(f"if (st->fbrst & {_hex(self.te_bit)}) {self.exit('VU_EXIT_TBIT', next_pc)}")

    # ---- whole program
    def emit(self):
        p = self.p
        mask = p.pc_mask
        nxt = lambda a: (a + 8) & mask
        self.decls = []
        body = []
        order = sorted(p.insns)
        # labels needed for continuations after inline delay slots
        labels = set(p.labels)
        for a in order:
            ins = p.insns[a]
            if a in p.delay:
                continue
            if ins.is_branch or ins.ebit:
                pass
        for a, owner in p.delay.items():
            o = p.insns[owner]
            if o.is_branch and o.lower.name not in ("B", "BAL", "JR", "JALR"):
                labels.add(nxt(a))
            if o.is_branch and o.lower.name in ("BAL", "JALR"):
                labels.add(nxt(a))
        # the fall-through successor of every non-delay instruction must be emitted
        # right after it or be a label
        emitted = [a for a in order if a not in p.delay or a in labels]
        for k, a in enumerate(emitted):
            ins = p.insns[a]
            if ins.is_branch or ins.ebit:
                continue
            n = nxt(a)
            if n in p.insns and (k + 1 >= len(emitted) or emitted[k + 1] != n):
                labels.add(n)
        emitted = [a for a in order if a not in p.delay or a in labels]
        self.labels = labels
        pipe = _Pipe()
        prev_falls = False
        for k, a in enumerate(emitted):
            ins = p.insns[a]
            if a in labels:
                body.append(f"{self.lbl(a)}: ;")
                pipe = _Pipe()
                if prev_falls:
                    pipe.kick = self.uses_kick      # dynamic flag covers the rest
            elif not prev_falls:
                body.append(f"/* unreachable fall-through to {a:04X} */")
            br = self.insn(ins, pipe, body)
            if ins.is_branch:
                d = nxt(a)
                dins = p.insns[d]
                self.insn(dins, pipe, body)
                if pipe.kick and self.uses_kick:
                    body.append("vu_kick_flush(&x, st);")
                kind = br[0]
                after = nxt(d)
                tb = ins.tbit or dins.tbit
                if kind == "cond":
                    _, c, t = br
                    if tb:
                        body.append(f"if (st->fbrst & {_hex(self.te_bit)}) {{ if ({c}) {self.exit('VU_EXIT_TBIT', t)} {self.exit('VU_EXIT_TBIT', after)} }}")
                    body.append(f"if ({c}) goto {self.lbl(t)};")
                    body.append(f"goto {self.lbl(after)};")
                elif kind == "b":
                    if tb:
                        body.append(f"if (st->fbrst & {_hex(self.te_bit)}) {self.exit('VU_EXIT_TBIT', br[1])}")
                    body.append(f"goto {self.lbl(br[1])};")
                else:
                    if tb:
                        body.append(f"if (st->fbrst & {_hex(self.te_bit)}) return vu_ctx_end(&x, st, VU_EXIT_TBIT, {br[1]});")
                    body.append(f"pc = {br[1]}; goto dispatch;")
                prev_falls = False
                continue
            if ins.ebit:
                d = nxt(a)
                dins = p.insns[d]
                if dins.is_branch:
                    # branch in E-bit delay slot: treated as NOP (microVU branchWarning)
                    dins = VUInsn(dins.addr, 0x8000033C, dins.hi, dins.upper, decode_lower(0x8000033C))
                self.insn(dins, pipe, body)
                body.append(self.exit("VU_EXIT_EBIT", nxt(d)))
                prev_falls = False
                continue
            self.tbit_check(ins, body, nxt(a))
            n = nxt(a)
            if k + 1 < len(emitted) and emitted[k + 1] == n and n not in p.delay:
                prev_falls = True
            elif n in p.insns:
                body.append(f"goto {self.lbl(n)};")
                prev_falls = False
            else:
                prev_falls = False
        # assemble
        name = p.name
        head = [f"/* VU{p.vu} micro program {name}: entry 0x{p.entry:04X}, "
                f"{len(p.insns)} instructions, clamp mode {self.clamp} */",
                f"int {name}(VUState *st, uint32_t pc)",
                "{",
                "    VUCtx x;",
                "    VUReg128 U, L;",
                "    (void)U; (void)L;"]
        nb = self.ntmp
        tmpdecl = []
        for i in range(1, nb + 1):
            pass
        # collect temporaries actually used
        text = "\n".join(s for s in _flatten(body))
        for prefix, ctype in (("br", "int"), ("jt", "uint32_t")):
            vs = sorted({w for w in _words(text) if w.startswith(prefix) and w[len(prefix):].isdigit()},
                        key=lambda v: int(v[len(prefix):]))
            if vs:
                tmpdecl.append(f"    {ctype} {', '.join(vs)};")
        head += tmpdecl
        head.append("    vu_ctx_begin(&x, st);")
        head.append("dispatch:")
        head.append("    switch (pc) {")
        for a in sorted(self.labels):
            if a in p.insns:
                head.append(f"    case {_hex(a)}: goto {self.lbl(a)};")
        head.append(f"    default: return vu_ctx_end(&x, st, VU_EXIT_JUMP, pc);")
        head.append("    }")
        lines = head
        for s in _flatten(body):
            lines.append(s if s.startswith("L_") and s.endswith(": ;") else "    " + s)
        lines.append("}")
        return "\n".join(lines) + "\n"


def _flatten(items):
    for it in items:
        if isinstance(it, list):
            yield from _flatten(it)
        else:
            yield it


def _words(text):
    w, cur = [], []
    for ch in text:
        if ch.isalnum() or ch == "_":
            cur.append(ch)
        else:
            if cur:
                w.append("".join(cur))
                cur = []
    if cur:
        w.append("".join(cur))
    return w


def translate(code, entry, vu=1, clamp_mode=2, extra_entries=()):
    """Discover and translate one program.  Returns (VUProgram, C source of the function)."""
    prog = discover(code, entry, vu, extra_entries)
    return prog, _Emitter(prog, clamp_mode).emit()


# ----------------------------------------------------------------- files
OPS_HEADER = r"""/*
 * ps2_vu_ops.h - semantic helpers for VU micro programs translated by
 * recompiler/ps2recomp/vu.py (task S3-M1).  Generated file: do not edit.
 *
 * Semantics follow PCSX2 microVU (see vu.py).  Requirements on the build/host:
 *   - MXCSR = 0xFFC0 (FTZ, DAZ, round toward zero, exceptions masked) while a program
 *     runs (PCSX2 DEFAULT_VU_FP_CONTROL_REGISTER).
 *   - no FMA contraction, no fast-math (gcc/clang: -ffp-contract=off -frounding-math;
 *     MSVC: /fp:precise or /fp:strict, never /fp:fast).
 * The state struct VUState / VUReg128 and the exit codes come from ps2_vu1.h (S3-M2).
 */
#ifndef PS2_VU_OPS_H
#define PS2_VU_OPS_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "ps2_vu1.h"

#if defined(_MSC_VER)
#pragma fp_contract(off)
#pragma fenv_access(on)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#ifndef VU_CLAMP_MODE
#define VU_CLAMP_MODE @CLAMP@
#endif

static inline uint32_t vu_fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float vu_uf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* mVUclamp1: minps(x, FLT_MAX) then maxps(x, -FLT_MAX); a NaN becomes +FLT_MAX */
static inline float vu_cl1(float x)
{
    uint32_t u = vu_fu(x);
    if ((u & 0x7F800000u) == 0x7F800000u)
        u = (u & 0x007FFFFFu) ? 0x7F7FFFFFu : ((u & 0x80000000u) | 0x7F7FFFFFu);
    return vu_uf(u);
}
/* mVUclamp2 sign-preserving variant (clamp mode 3) */
static inline float vu_cl2(float x)
{
    uint32_t u = vu_fu(x);
    if ((u & 0x7F800000u) == 0x7F800000u)
        u = (u & 0x80000000u) | 0x7F7FFFFFu;
    return vu_uf(u);
}

/* "SSE op" wrappers (microVU_Misc.inl clampOp): operand / result clamps per mode */
#if VU_CLAMP_MODE >= 3
#define VU_OPND(v) vu_cl2(v)
#define VU_RES(v) (v)
#elif VU_CLAMP_MODE == 2
#define VU_OPND(v) vu_cl1(v)
#define VU_RES(v) vu_cl1(v)
#else
#define VU_OPND(v) (v)
#define VU_RES(v) (v)
#endif
static inline float VU_ADD(float a, float b) { return VU_RES(VU_OPND(a) + VU_OPND(b)); }
static inline float VU_SUB(float a, float b) { return VU_RES(VU_OPND(a) - VU_OPND(b)); }
static inline float VU_MUL(float a, float b) { return VU_RES(VU_OPND(a) * VU_OPND(b)); }
static inline float VU_DIV(float a, float b) { return VU_RES(VU_OPND(a) / VU_OPND(b)); }

/* MAX/MINI: integer compare of sign-magnitude keys (MIN_MAX_PS) */
static inline int32_t vu_mmkey(uint32_t u) { return (int32_t)(u ^ ((uint32_t)((int32_t)u >> 31) >> 1)); }
static inline uint32_t vu_max_u(uint32_t a, uint32_t b) { return vu_mmkey(a) > vu_mmkey(b) ? a : b; }
static inline uint32_t vu_min_u(uint32_t a, uint32_t b) { return vu_mmkey(b) > vu_mmkey(a) ? a : b; }

/* FTOI: cvttps2dq, positive overflow (> 0x4EFFFFFF as int) -> 0x7FFFFFFF */
static inline int32_t vu_ftoi(float f)
{
    uint32_t u = vu_fu(f);
    if ((int32_t)u > 0x4EFFFFFF)
        return 0x7FFFFFFF;
    if ((u & 0x7F800000u) >= 0x4F000000u)
        return (int32_t)0x80000000u;
    return (int32_t)f;
}

/* CLIP: integer compares against |w|, denormal inputs count as zero */
static inline uint32_t vu_clip_bits(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    uint32_t v[3] = {x, y, z}, f = 0, lim = w & 0x7FFFFFFFu;
    for (int i = 0; i < 3; i++) {
        uint32_t a = (v[i] & 0x7F800000u) ? v[i] : 0u;
        if ((int32_t)a > (int32_t)lim) f |= 1u << (2 * i);
        if ((int32_t)(a ^ 0x80000000u) > (int32_t)lim) f |= 2u << (2 * i);
    }
    return f;
}

static inline uint32_t vu_rnext(uint32_t r)
{
    uint32_t bit = ((r >> 4) ^ (r >> 22)) & 1u;
    return 0x3F800000u | (((r << 1) ^ bit) & 0x7FFFFFu);
}

static inline uint8_t *vu_qw(VUState *st, uint32_t qindex) { return st->mem + (qindex << 4); }

/* ---- flag pipeline: 4 instances, visible 4 cycles after the write (microVU_Flags.inl) */
typedef struct vu_fpipe {
    uint32_t latest, vis, n;
    uint32_t val[4], rdy[4];
} vu_fpipe;

static inline void vu_fp_init(vu_fpipe *f, uint32_t v) { f->latest = f->vis = v; f->n = 0; }
static inline void vu_fp_pop(vu_fpipe *f)
{
    f->vis = f->val[0];
    f->n--;
    for (uint32_t k = 0; k < f->n; k++) {
        f->val[k] = f->val[k + 1];
        f->rdy[k] = f->rdy[k + 1];
    }
}
static inline uint32_t vu_fp_read(vu_fpipe *f, uint32_t cyc)
{
    while (f->n && (int32_t)(cyc - f->rdy[0]) >= 0)
        vu_fp_pop(f);
    return f->vis;
}
static inline void vu_fp_write(vu_fpipe *f, uint32_t v, uint32_t cyc)
{
    while (f->n && (int32_t)(cyc - f->rdy[0]) >= 0)
        vu_fp_pop(f);
    if (f->n == 4)
        vu_fp_pop(f);
    f->val[f->n] = v;
    f->rdy[f->n] = cyc + 4;
    f->n++;
    f->latest = v;
}

typedef struct VUCtx {
    uint32_t cyc;
    vu_fpipe st, mac, clip;
    float q, qn, p, pn;
    uint32_t q_rdy, q_pend, d_flag, d_pend;
    uint32_t p_rdy, p_pend;
    uint32_t kick_pend, kick_addr;
} VUCtx;

/* MAC flag of a result: bits 7..4 sign x..w, bits 3..0 zero x..w (mVUupdateFlags) */
static inline uint32_t vu_macf(const VUReg128 *r, uint32_t dest)
{
    uint32_t m = 0;
    for (int i = 0; i < 4; i++)
        if (dest & (8u >> i)) {
            if (r->UW[i] & 0x80000000u) m |= 0x80u >> i;
            if (r->F[i] == 0.0f) m |= 0x8u >> i;
        }
    return m;
}
/* STATUS: Z/S replaced, ZS/SS sticky, U/O cleared (never set, as microVU) */
static inline void vu_fmac_flags(VUCtx *x, uint32_t m, int track_mac, int track_st)
{
    uint32_t s = x->st.latest & ~0xFu;
    if (m & 0x0Fu) s |= 0x41u;
    if (m & 0xF0u) s |= 0x82u;
    if (track_mac) vu_fp_write(&x->mac, m, x->cyc); else x->mac.latest = m;
    if (track_st) vu_fp_write(&x->st, s, x->cyc); else x->st.latest = s;
}

/* ---- Q (FDIV) */
static inline float vu_q(VUCtx *x)
{
    if (x->q_pend && (int32_t)(x->cyc - x->q_rdy) >= 0) { x->q = x->qn; x->q_pend = 0; }
    return x->q;
}
static inline void vu_q_stall(VUCtx *x)
{
    if (x->q_pend && (int32_t)(x->cyc - x->q_rdy) < 0) x->cyc = x->q_rdy;
}
static inline void vu_divflag_apply(VUCtx *x, int track_st)
{
    uint32_t s = (x->st.latest & ~0x30u) | x->d_flag;
    if (track_st) vu_fp_write(&x->st, s, x->cyc); else x->st.latest = s;
    x->d_pend = 0;
}
/* I/D written when the FDIV counter reaches 4 (mVUincCycles doDivFlag) */
static inline void vu_divflag_sync(VUCtx *x, int track_st)
{
    if (x->d_pend && (int32_t)(x->cyc - (x->q_rdy - 4u)) >= 0) vu_divflag_apply(x, track_st);
}
static inline void vu_fdiv_issue(VUCtx *x, float v, uint32_t flags, uint32_t lat, int track_st)
{
    if (x->q_pend) { x->q = x->qn; x->q_pend = 0; }
    if (x->d_pend) vu_divflag_apply(x, track_st);
    x->qn = v; x->q_rdy = x->cyc + lat; x->q_pend = 1;
    x->d_flag = flags; x->d_pend = 1;
}
static inline float vu_div_calc(float s, float t, uint32_t *fl)
{
    if (t == 0.0f) {
        *fl = (s == 0.0f) ? 0x410u : 0x820u;
        return vu_uf(((vu_fu(s) ^ vu_fu(t)) & 0x80000000u) | 0x7F7FFFFFu);
    }
    *fl = 0;
    return vu_cl1(VU_DIV(s, t));
}
static inline float vu_sqrt_calc(float t, uint32_t *fl)
{
    uint32_t u = vu_fu(t);
    *fl = 0;
    if (u & 0x80000000u) { *fl = 0x410u; u &= 0x7FFFFFFFu; }
#if VU_CLAMP_MODE >= 1
    if (u > 0x7F7FFFFFu) u = 0x7F7FFFFFu;
#endif
    return sqrtf(vu_uf(u));
}
static inline float vu_rsqrt_calc(float s, float t, uint32_t *fl)
{
    uint32_t u = vu_fu(t);
    float r;
    *fl = 0;
    if (u & 0x80000000u) { *fl = 0x410u; u &= 0x7FFFFFFFu; }
    r = sqrtf(vu_uf(u));
    if (r == 0.0f) {
        *fl = (s == 0.0f) ? 0x410u : 0x820u;
        return vu_uf((vu_fu(s) & 0x80000000u) | 0x7F7FFFFFu);
    }
    return vu_cl1(VU_DIV(s, r));
}

/* ---- P (EFU) */
static inline float vu_p(VUCtx *x)
{
    if (x->p_pend && (int32_t)(x->cyc - x->p_rdy) >= 0) { x->p = x->pn; x->p_pend = 0; }
    return x->p;
}
static inline void vu_p_stall(VUCtx *x)
{
    if (x->p_pend && (int32_t)(x->cyc + 1u - x->p_rdy) < 0) x->cyc = x->p_rdy - 1u;
}
static inline void vu_efu_issue(VUCtx *x, float v, uint32_t lat)
{
    if (x->p_pend) { x->p = x->pn; x->p_pend = 0; }
    x->pn = v; x->p_rdy = x->cyc + lat; x->p_pend = 1;
}

#define VU_K(h) vu_uf(h##u)
static inline float vu_sum3(float x, float y, float z, float w) { (void)w; return (x * x + y * y) + z * z; }
static inline float vu_esadd(float x, float y, float z, float w) { return vu_sum3(x, y, z, w); }
static inline float vu_ersadd(float x, float y, float z, float w) { return VU_DIV(1.0f, vu_sum3(x, y, z, w)); }
static inline float vu_eleng(float x, float y, float z, float w) { return sqrtf(vu_sum3(x, y, z, w)); }
static inline float vu_erleng(float x, float y, float z, float w) { return VU_DIV(1.0f, sqrtf(vu_sum3(x, y, z, w))); }
static inline float vu_esum(float x, float y, float z, float w) { return VU_ADD(VU_ADD(x, w), VU_ADD(y, z)); }
static inline float vu_esqrt(float x) { return sqrtf(vu_uf(vu_fu(x) & 0x7FFFFFFFu)); }
static inline float vu_ersqrt(float x) { return VU_DIV(1.0f, sqrtf(vu_uf(vu_fu(x) & 0x7FFFFFFFu))); }
static inline float vu_ercpr(float x) { return VU_DIV(1.0f, x); }
static inline float vu_esin(float x)
{
    float pq = x, f, t1, t2;
    f = VU_MUL(x, x); t1 = f; f = VU_MUL(f, pq); t2 = f;
    f = f * VU_K(0xbe2aaaa4); pq = VU_ADD(pq, f);
    t2 = VU_MUL(t2, t1); f = t2 * VU_K(0x3c08873e); pq = VU_ADD(pq, f);
    t2 = VU_MUL(t2, t1); f = t2 * VU_K(0xb94fb21f); pq = VU_ADD(pq, f);
    t2 = VU_MUL(t2, t1); t2 = t2 * VU_K(0x362e9c14); pq = VU_ADD(pq, t2);
    return pq;
}
static inline float vu_eatan_poly(float f)
{
    static const uint32_t k[7] = {0xbeaaa61cu, 0x3e4c40a6u, 0xbe0e6c63u, 0x3dc577dfu,
                                  0xbd6501c4u, 0x3cb31652u, 0xbb84d7e7u};
    float pq = f * VU_K(0x3f7ffff5), t2 = f;
    for (int i = 0; i < 7; i++) {
        t2 = VU_MUL(t2, f);
        t2 = VU_MUL(t2, f);
        pq = VU_ADD(pq, t2 * vu_uf(k[i]));
    }
    return pq + VU_K(0x3f490fdb);
}
static inline float vu_eatan(float x) { return vu_eatan_poly(VU_DIV(x - 1.0f, x + 1.0f)); }
static inline float vu_eatanxy(float x, float y, float z, float w) { (void)z; (void)w; return vu_eatan_poly(VU_DIV(VU_SUB(y, x), VU_ADD(x, y))); }
static inline float vu_eatanxz(float x, float y, float z, float w) { (void)y; (void)w; return vu_eatan_poly(VU_DIV(VU_SUB(z, x), VU_ADD(x, z))); }
static inline float vu_eexp(float x)
{
    float pq = x * VU_K(0x3e7fffa8), t1, t2;
    pq = pq + 1.0f;
    t1 = VU_MUL(x, x); t2 = t1; t1 = t1 * VU_K(0x3d0007f4); pq = VU_ADD(pq, t1);
    t2 = VU_MUL(t2, x); t1 = t2 * VU_K(0x3b29d3ff); pq = VU_ADD(pq, t1);
    t2 = VU_MUL(t2, x); t1 = t2 * VU_K(0x3933e553); pq = VU_ADD(pq, t1);
    t2 = VU_MUL(t2, x); t1 = t2 * VU_K(0x36b63510); pq = VU_ADD(pq, t1);
    t2 = VU_MUL(t2, x); t2 = t2 * VU_K(0x353961ac); pq = VU_ADD(pq, t2);
    pq = VU_MUL(pq, pq); pq = VU_MUL(pq, pq);
    return VU_DIV(1.0f, pq);
}

/* ---- XGKICK: the transfer runs after the next instruction (mVU_XGKICK_DELAY) */
static inline void vu_kick_flush(VUCtx *x, VUState *st)
{
    if (x->kick_pend) {
        x->kick_pend = 0;
        st->xgkick(st, x->kick_addr);
    }
}

/* ---- entry / exit */
static inline void vu_ctx_begin(VUCtx *x, VUState *st)
{
    memset(x, 0, sizeof(*x));
    vu_fp_init(&x->st, st->status & 0xFFFu);
    vu_fp_init(&x->mac, st->mac & 0xFFFFu);
    vu_fp_init(&x->clip, st->clip & 0xFFFFFFu);
    x->q = st->q.f;
    x->p = st->p.f;
}
/* E-bit / T-bit / jump-out: the pipeline drains (mVUendProgram mVUincCycles(100)) */
static inline int vu_ctx_end(VUCtx *x, VUState *st, int code, uint32_t pc)
{
    vu_kick_flush(x, st);
    if (x->q_pend) { x->q = x->qn; x->q_pend = 0; }
    if (x->d_pend) { x->st.latest = (x->st.latest & ~0x30u) | x->d_flag; x->d_pend = 0; }
    if (x->p_pend) { x->p = x->pn; x->p_pend = 0; }
    st->status = x->st.latest;
    st->mac = x->mac.latest;
    st->clip = x->clip.latest;
    st->q.f = x->q;
    st->p.f = x->p;
    st->tpc = pc;
    st->cycles += x->cyc;
    return code;
}

#endif /* PS2_VU_OPS_H */
"""


# The proposed runtime header (S3-M2 implements it as runtime/include/ps2_vu1.h).
REFERENCE_STATE_HEADER = r"""/* ps2_vu1.h - VU micro-mode state shared with programs translated by
 * recompiler/ps2recomp/vu.py.  Interface proposal of task S3-M1; the runtime owner
 * (S3-M2) may add fields AFTER the ones listed here but must keep these names/types. */
#ifndef PS2_VU1_H
#define PS2_VU1_H
#include <stdint.h>

typedef union VUReg128 {
    float F[4];
    uint32_t UW[4];
    int32_t SW[4];
} VUReg128;                         /* element 0 = x ... 3 = w */

typedef union VUReg32 { float f; uint32_t u; } VUReg32;

typedef struct VUState {
    VUReg128 vf[32];                /* vf[0] = (0,0,0,1.0); translated code never writes it */
    VUReg128 acc;
    uint16_t vi[16];                /* vi[0] = 0; never written */
    uint32_t status;                /* VI16 STATUS (12 bits, PS2 layout: Z S U O I D ZS SS US OS IS DS) */
    uint32_t mac;                   /* VI17 MAC (16 bits: O[15:12] U[11:8] S[7:4] Z[3:0], x = high bit) */
    uint32_t clip;                  /* VI18 CLIP (24 bits, 4 judgements of 6 bits) */
    uint32_t r;                     /* VI20 R (0x3F800000 | 23 random bits) */
    VUReg32 i, q, p;                /* VI21 I, VI22 Q, VI23 P */
    uint32_t tpc;                   /* VI26 TPC (bytes), set on every exit */
    uint32_t fbrst;                 /* FBRST (VU1: 0x400 DE, 0x800 TE; VU0: 0x4, 0x8) */
    uint32_t vif_top, vif_itop;     /* VIF1 TOP / ITOP for XTOP / XITOP (VU1) */
    uint8_t *mem;                   /* data memory: 16 KiB (VU1) or 4 KiB (VU0) */
    uint64_t cycles;                /* += VU cycles executed by each call */
    void (*xgkick)(struct VUState *st, uint32_t addr);  /* PATH1 packet at mem + addr (bytes, may wrap at 16 KiB) */
    void *user;
} VUState;

enum {
    VU_EXIT_EBIT = 0,               /* program ended (E bit); tpc = address after the delay slot */
    VU_EXIT_TBIT = 1,               /* T bit with FBRST.TE: raise the VU interrupt; tpc = resume pc */
    VU_EXIT_JUMP = 2,               /* JR/JALR (or the entry pc) left the translated code: tpc = target */
};

typedef int (*vu_prog_fn)(VUState *st, uint32_t pc);

typedef struct vu_prog_entry {
    uint32_t vu;                    /* 0 or 1 */
    uint32_t entry;                 /* entry pc used to discover the program (bytes) */
    uint64_t hash;                  /* FNV-1a 64 of the covered micro memory bytes */
    uint32_t nranges;
    const uint16_t (*ranges)[2];    /* covered [start, end) byte ranges, ascending */
    const uint32_t *code;           /* the covered words, in range order */
    uint32_t nentries;
    const uint16_t *entries;        /* every pc the function accepts (sorted) */
    vu_prog_fn fn;
} vu_prog_entry;

#endif
"""


def emit_ops_header(clamp_mode=2):
    return OPS_HEADER.replace("@CLAMP@", str(clamp_mode))


def emit_program_set(progs, clamp_mode=2, table_name="vu_prog_table"):
    """One C file for a list of translated programs [(VUProgram, csource)]."""
    out = ["/* Generated by recompiler/ps2recomp/vu.py (S3-M1). Do not edit. */",
           f"#define VU_CLAMP_MODE {clamp_mode}",
           '#include "ps2_vu_ops.h"', ""]
    for prog, src in progs:
        out.append(src)
    for prog, src in progs:
        n = prog.name
        rs = prog.ranges()
        out.append(f"static const uint16_t {n}_ranges[][2] = {{"
                   + ", ".join(f"{{0x{s:04X}, 0x{e:04X}}}" for s, e in rs) + "};")
        words = prog.code_words()
        out.append(f"static const uint32_t {n}_code[] = {{")
        for k in range(0, len(words), 8):
            out.append("    " + ", ".join(f"0x{w:08X}u" for w in words[k:k + 8]) + ",")
        out.append("};")
        em = _Emitter(prog, clamp_mode)
        em.emit()
        ents = sorted(a for a in em.labels if a in prog.insns)
        out.append(f"static const uint16_t {n}_entries[] = {{"
                   + ", ".join(f"0x{a:04X}" for a in ents) + "};")
    out.append(f"const vu_prog_entry {table_name}[] = {{")
    for prog, src in progs:
        n = prog.name
        out.append(f"    {{{prog.vu}, 0x{prog.entry:04X}u, 0x{prog.hash():016X}ull, "
                   f"{len(prog.ranges())}, {n}_ranges, {n}_code, "
                   f"(uint32_t)(sizeof({n}_entries) / sizeof({n}_entries[0])), {n}_entries, {n}}},")
    out.append("};")
    out.append(f"const uint32_t {table_name}_count = {len(progs)};")
    return "\n".join(out) + "\n"


def assemble(pairs, size=0x4000, base=0):
    """Micro memory image from [(lower, upper)] pairs (tests / tools)."""
    mem = bytearray(size)
    for k, (lo, hi) in enumerate(pairs):
        struct.pack_into("<II", mem, base + 8 * k, lo & 0xFFFFFFFF, hi & 0xFFFFFFFF)
    return bytes(mem)


def listing(prog):
    lines = []
    for a in sorted(prog.insns):
        mark = ">" if a in prog.labels else (" d" if a in prog.delay else "  ")
        lines.append(f"{mark:2} {a:04X}: {prog.insns[a].lo:08X} {prog.insns[a].hi:08X}  "
                     f"{disasm(prog.insns[a])}")
    return "\n".join(lines)


def main(argv=None):
    import argparse
    from pathlib import Path
    ap = argparse.ArgumentParser(description="Translate a VU micro program to C")
    ap.add_argument("micro", help="micro memory image (4 KiB VU0 / 16 KiB VU1)")
    ap.add_argument("--entry", type=lambda s: int(s, 0), action="append", required=True)
    ap.add_argument("--vu", type=int, default=1)
    ap.add_argument("--clamp", type=int, default=2, help="PCSX2 VU clamp mode (SH3 GameDB: 2)")
    ap.add_argument("-o", "--out", required=True, help="output directory")
    ap.add_argument("--list", action="store_true", help="print a disassembly listing")
    a = ap.parse_args(argv)
    code = Path(a.micro).read_bytes()
    progs = []
    for e in a.entry:
        prog, src = translate(code, e, a.vu, a.clamp)
        progs.append((prog, src))
        for w in prog.warnings:
            print("warning:", w)
        if a.list:
            print(listing(prog))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "ps2_vu_ops.h").write_text(emit_ops_header(a.clamp))
    (out / "vu_progs.c").write_text(emit_program_set(progs, a.clamp))
    print(f"{len(progs)} program(s) -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
