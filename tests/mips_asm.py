"""Tiny R5900 assembler (Python DSL) + ELF writer for building test programs.

Example:
    a = Asm()
    a.func("main")
    a.li("a0", 42)
    a.jr("ra"); a.nop()
    a.write_elf("t.elf")
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))
from ps2recomp import r5900  # noqa: E402

REGS = {n: i for i, n in enumerate(r5900.GPR_NAMES)}
REGS.update({f"r{i}": i for i in range(32)})
REGS["s8"] = 30

TEXT_BASE = 0x00100000


def R(x):
    if isinstance(x, int):
        return x
    return REGS[x.lstrip("$")]


def F(x):
    return int(str(x).lstrip("$f")) if not isinstance(x, int) else x


def V(x):
    return int(str(x).lstrip("$vf")) if not isinstance(x, int) else x


def _inv(table):
    return {v: k for k, v in table.items()}


SPECIAL = _inv(r5900._SPECIAL)
REGIMM = _inv(r5900._REGIMM)
PRIMARY = _inv(r5900._PRIMARY)
MMI = _inv(r5900._MMI)
MMI0, MMI1, MMI2, MMI3 = (_inv(r5900._MMI0), _inv(r5900._MMI1), _inv(r5900._MMI2), _inv(r5900._MMI3))
FPU_S = _inv(r5900._FPU_S)
VU1 = _inv(r5900._VU_SPECIAL1)
VU2 = _inv(r5900._VU_SPECIAL2)


class Asm:
    def __init__(self, text_base=TEXT_BASE):
        self.text_base = text_base
        self.code = []            # list of int words or callables(addr) -> word
        self.labels = {}
        self.funcs = []
        self.data = bytearray()
        self.data_labels = {}
        self.data_relocs = []     # (offset, label)

    # ----------------------------------------------------------- layout
    @property
    def pc(self):
        return self.text_base + 4 * len(self.code)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def func(self, name):
        self.label(name)
        self.funcs.append(name)

    def data_base(self):
        end = self.text_base + 4 * len(self.code)
        return (end + 0xFFF) & ~0xFFF

    def addr_of(self, name):
        if name in self.labels:
            return self.labels[name]
        return self.data_base() + self.data_labels[name]

    def emit(self, w):
        self.code.append(w)

    # ------------------------------------------------------------- data
    def dlabel(self, name):
        self.data_labels[name] = len(self.data)

    def dalign(self, n):
        while len(self.data) % n:
            self.data.append(0)

    def word(self, *vals):
        self.dalign(4)
        for v in vals:
            if isinstance(v, str):
                self.data_relocs.append((len(self.data), v))
                v = 0
            self.data += struct.pack("<I", v & 0xFFFFFFFF)

    def dword(self, *vals):
        self.dalign(8)
        for v in vals:
            self.data += struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF)

    def floats(self, *vals):
        self.dalign(4)
        for v in vals:
            self.data += struct.pack("<f", v)

    def asciiz(self, s):
        self.data += s.encode() + b"\0"

    def space(self, n):
        self.data += bytes(n)

    # --------------------------------------------------------- encoders
    def r(self, funct, rs=0, rt=0, rd=0, sa=0):
        self.emit((R(rs) << 21) | (R(rt) << 16) | (R(rd) << 11) | ((sa & 31) << 6) | funct)

    def i(self, op, rs, rt, imm):
        self.emit((op << 26) | (R(rs) << 21) | (R(rt) << 16) | (imm & 0xFFFF))

    def _branch(self, op, rs, rt, target):
        here = self.pc

        def enc(_=None):
            t = self.addr_of(target) if isinstance(target, str) else target
            off = (t - (here + 4)) >> 2
            return (op << 26) | (R(rs) << 21) | (rt << 16 if isinstance(rt, int) else R(rt) << 16) | (off & 0xFFFF)
        self.emit(enc)

    def _jump(self, op, target):
        def enc(_=None):
            t = self.addr_of(target) if isinstance(target, str) else target
            return (op << 26) | ((t >> 2) & 0x3FFFFFF)
        self.emit(enc)

    # ------------------------------------------------------------ insns
    def nop(self):
        self.emit(0)

    def syscall(self):
        self.emit(12)

    def brk(self, code=0):
        self.emit((code << 6) | 13)

    def sync(self):
        self.emit(15)

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        mn = name.rstrip("_").replace("_", ".")   # and_ -> and, add_s -> add.s
        if mn in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
            return lambda rd, rt, sa: self.r(SPECIAL[mn], 0, rt, rd, sa)
        if mn in ("sllv", "srlv", "srav", "dsllv", "dsrlv", "dsrav"):
            return lambda rd, rt, rs: self.r(SPECIAL[mn], rs, rt, rd)
        if mn in ("mult", "multu"):
            return lambda *a: self.r(SPECIAL[mn], a[-2], a[-1], a[0] if len(a) == 3 else 0)
        if mn in ("div", "divu"):
            return lambda rs, rt: self.r(SPECIAL[mn], rs, rt)
        if mn in ("mfhi", "mflo", "mfsa"):
            return lambda rd: self.r(SPECIAL[mn], 0, 0, rd)
        if mn in ("mthi", "mtlo", "mtsa"):
            return lambda rs: self.r(SPECIAL[mn], rs)
        if mn == "jr":
            return lambda rs: self.r(8, rs)
        if mn == "jalr":
            return lambda rd, rs=None: self.r(9, rs if rs is not None else rd, 0,
                                              rd if rs is not None else "ra")
        if mn in SPECIAL:
            return lambda rd, rs, rt: self.r(SPECIAL[mn], rs, rt, rd)
        if mn in ("beq", "bne", "beql", "bnel"):
            return lambda rs, rt, t: self._branch(PRIMARY[mn], rs, rt, t)
        if mn in ("blez", "bgtz", "blezl", "bgtzl"):
            return lambda rs, t: self._branch(PRIMARY[mn], rs, 0, t)
        if mn in REGIMM and mn.startswith("b"):
            return lambda rs, t: self._branch(1, rs, REGIMM[mn], t)
        if mn in ("mtsab", "mtsah"):
            return lambda rs, imm: self.i(1, rs, REGIMM[mn], imm)
        if mn in ("j", "jal"):
            return lambda t: self._jump(PRIMARY[mn], t)
        if mn == "lui":
            return lambda rt, imm: self.i(15, 0, rt, imm)
        if mn in ("lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lq", "sb", "sh", "sw", "sd",
                  "sq", "lwl", "lwr", "ldl", "ldr", "swl", "swr", "sdl", "sdr"):
            return lambda rt, off, base: self.i(PRIMARY[mn], base, rt, off)
        if mn in ("lwc1", "swc1"):
            return lambda ft, off, base: self.i(PRIMARY[mn], base, F(ft), off)
        if mn in ("lqc2", "sqc2"):
            return lambda vt, off, base: self.i(PRIMARY[mn], base, V(vt), off)
        if mn in PRIMARY:  # immediate arithmetic: rt, rs, imm
            return lambda rt, rs, imm: self.i(PRIMARY[mn], rs, rt, imm)
        if mn in MMI:
            return lambda rd, rs=0, rt=0, sa=0: self.emit(
                (28 << 26) | (R(rs) << 21) | (R(rt) << 16) | (R(rd) << 11) | (sa << 6) | MMI[mn])
        for tbl, fn in ((MMI0, 8), (MMI1, 40), (MMI2, 9), (MMI3, 41)):
            if mn in tbl:
                return lambda rd, rs, rt, tbl=tbl, fn=fn: self.emit(
                    (28 << 26) | (R(rs) << 21) | (R(rt) << 16) | (R(rd) << 11) | (tbl[mn] << 6) | fn)
        if mn in _inv(r5900._PMFHL):
            sub = _inv(r5900._PMFHL)[mn]
            return lambda rd, rs="zero": self.emit((28 << 26) | (R(rd) << 11) | (sub << 6) | 48)
        if mn == "pmthl.lw":
            return lambda rs: self.emit((28 << 26) | (R(rs) << 21) | 49)
        if mn in ("mtc1", "mfc1", "ctc1", "cfc1"):
            sub = {"mfc1": 0, "cfc1": 2, "mtc1": 4, "ctc1": 6}[mn]
            return lambda rt, fs: self.emit((17 << 26) | (sub << 21) | (R(rt) << 16) | (F(fs) << 11))
        if mn in ("bc1f", "bc1t", "bc1fl", "bc1tl"):
            code = {"bc1f": 0, "bc1t": 1, "bc1fl": 2, "bc1tl": 3}[mn]
            return lambda t: self._branch(17, 8, code, t)
        if mn == "cvt.s.w":
            return lambda fd, fs: self.emit((17 << 26) | (20 << 21) | (F(fs) << 11) | (F(fd) << 6) | 32)
        if mn in FPU_S:
            def fpu(*a):
                if mn.startswith("c."):
                    fs, ft = a
                    fd = 0
                elif len(a) == 2:
                    if mn in ("adda.s", "suba.s", "mula.s", "madda.s", "msuba.s"):
                        fd, fs, ft = 0, a[0], a[1]
                    elif mn in ("sqrt.s",):
                        fd, fs, ft = a[0], 0, a[1]
                    else:
                        fd, fs, ft = a[0], a[1], 0
                else:
                    fd, fs, ft = a
                self.emit((17 << 26) | (16 << 21) | (F(ft) << 16) | (F(fs) << 11) | (F(fd) << 6) | FPU_S[mn])
            return fpu
        if mn in ("qmtc2", "qmfc2", "ctc2", "cfc2"):
            sub = {"qmfc2": 1, "cfc2": 2, "qmtc2": 5, "ctc2": 6}[mn]
            return lambda rt, vd: self.emit((18 << 26) | (sub << 21) | (R(rt) << 16) | (V(vd) << 11))
        base = mn.split(".")[0]
        if base in VU1 or base in VU2:
            dest = mn.split(".")[1] if "." in mn else "xyzw"
            dmask = sum(1 << (24 - "xyzw".index(ch)) for ch in dest)

            def vu(fd=0, fs=0, ft=0, base=base, dmask=dmask):
                if base in VU1:
                    w = (18 << 26) | (1 << 25) | dmask | (V(ft) << 16) | (V(fs) << 11) | (V(fd) << 6) | VU1[base]
                else:
                    idx = VU2[base]
                    w = (18 << 26) | (1 << 25) | dmask | (V(ft) << 16) | (V(fs) << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3)
                self.emit(w)
            return vu
        raise AttributeError(name)

    # ---------------------------------------------------------- pseudo
    def li(self, rt, value):
        value &= 0xFFFFFFFF
        if value < 0x8000 or value >= 0xFFFF8000:
            self.i(9, 0, rt, value & 0xFFFF)          # addiu
        elif value <= 0xFFFF:
            self.i(13, 0, rt, value)                  # ori
        else:
            self.i(15, 0, rt, value >> 16)            # lui
            if value & 0xFFFF:
                self.i(13, rt, rt, value & 0xFFFF)    # ori

    def la(self, rt, name):
        self.emit(lambda _=None: (15 << 26) | (R(rt) << 16) | (self.addr_of(name) >> 16))
        self.emit(lambda _=None: (13 << 26) | (R(rt) << 21) | (R(rt) << 16) | (self.addr_of(name) & 0xFFFF))

    def la_addiu(self, rt, name):
        """lui/addiu form (what gcc emits for function pointers)."""
        def hi(_=None):
            a = self.addr_of(name)
            return (15 << 26) | (R(rt) << 16) | (((a + 0x8000) >> 16) & 0xFFFF)

        def lo(_=None):
            return (9 << 26) | (R(rt) << 21) | (R(rt) << 16) | (self.addr_of(name) & 0xFFFF)
        self.emit(hi)
        self.emit(lo)

    def move(self, rd, rs):
        self.r(SPECIAL["daddu"], rs, "zero", rd)

    def b(self, target):
        self.beq("zero", "zero", target)

    def beqz(self, rs, t):
        self.beq(rs, "zero", t)

    def bnez(self, rs, t):
        self.bne(rs, "zero", t)

    # ---------------------------------------------------------- output
    def assemble(self):
        words = []
        for idx, w in enumerate(self.code):
            words.append(w() if callable(w) else w)
        data = bytearray(self.data)
        for off, name in self.data_relocs:
            struct.pack_into("<I", data, off, self.addr_of(name))
        return words, bytes(data)

    def write_elf(self, path, symbols=True):
        words, data = self.assemble()
        text = b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)
        text_addr = self.text_base
        data_addr = self.data_base()
        entry = self.labels.get("_start", text_addr)

        shstr = b"\0.text\0.data\0.symtab\0.strtab\0.shstrtab\0"
        names = {".text": 1, ".data": 7, ".symtab": 13, ".strtab": 21, ".shstrtab": 29}
        strtab = bytearray(b"\0")
        syms = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]
        if symbols:
            for fname in self.funcs:
                off = len(strtab)
                strtab += fname.encode() + b"\0"
                syms.append(struct.pack("<IIIBBH", off, self.labels[fname], 0, (1 << 4) | 2, 0, 1))
        symtab = b"".join(syms)

        ehsize, phsize, shsize = 52, 32, 40
        nph = 2 if data else 1
        off_text = 0x1000
        off_data = off_text + ((len(text) + 0xFFF) & ~0xFFF)
        off_sym = off_data + len(data)
        off_str = off_sym + len(symtab)
        off_shstr = off_str + len(strtab)
        off_sh = (off_shstr + len(shstr) + 3) & ~3

        out = bytearray(off_sh + 6 * shsize)
        struct.pack_into("<4sBBBB8sHHIIIIIHHHHHH", out, 0, b"\x7fELF", 1, 1, 1, 0, bytes(8),
                         2, 8, 1, entry, ehsize, off_sh, 0x20924001, ehsize, phsize, nph,
                         shsize, 6, 5)
        struct.pack_into("<IIIIIIII", out, ehsize, 1, off_text, text_addr, text_addr,
                         len(text), len(text), 5, 0x1000)
        if data:
            struct.pack_into("<IIIIIIII", out, ehsize + phsize, 1, off_data, data_addr,
                             data_addr, len(data), len(data) + 0x1000, 6, 0x1000)
        out[off_text:off_text + len(text)] = text
        out[off_data:off_data + len(data)] = data
        out[off_sym:off_sym + len(symtab)] = symtab
        out[off_str:off_str + len(strtab)] = strtab
        out[off_shstr:off_shstr + len(shstr)] = shstr
        sh = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (names[".text"], 1, 6, text_addr, off_text, len(text), 0, 0, 4, 0),
            (names[".data"], 1, 3, data_addr, off_data, len(data), 0, 0, 16, 0),
            (names[".symtab"], 2, 0, 0, off_sym, len(symtab), 4, 1, 4, 16),
            (names[".strtab"], 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0),
            (names[".shstrtab"], 3, 0, 0, off_shstr, len(shstr), 0, 0, 1, 0),
        ]
        for i, s in enumerate(sh):
            struct.pack_into("<IIIIIIIIII", out, off_sh + i * shsize, *s)
        Path(path).write_bytes(bytes(out))
        return path
