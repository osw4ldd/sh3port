"""Self-checking PS2 test programs, assembled with mips_asm.

Each program prints "FAIL <name>" for every failed check, "ALL PASS" when
everything passed, and exits with the number of failures.
"""

import struct

from mips_asm import Asm

SYS_EXIT = 0x04
SYS_PRINT = 0x75


class Checker:
    def __init__(self, a):
        self.a = a
        self.n = 0
        self.names = []

    def _fail(self, name):
        a = self.a
        msg = f"m_fail_{self.n}"
        self.n += 1
        a.la("a0", msg)
        a.jal("t_fail")
        a.nop()
        a.dlabel(msg)
        a.asciiz(f"FAIL {name}\n")
        self.names.append(name)

    def eq(self, reg, value, name):
        """64-bit compare of reg with a sign-extended 32-bit constant."""
        a = self.a
        ok = f"ok_{self.n}"
        a.li("t9", value)
        a.beq(reg, "t9", ok)
        a.nop()
        self._fail(name)
        a.label(ok)

    def eq64(self, reg, value, name):
        a = self.a
        a.la("t8", "scratch")
        a.sd(reg, 0, "t8")
        a.lw("t7", 0, "t8")
        self.eq("t7", value & 0xFFFFFFFF, name + ".lo")
        a.la("t8", "scratch")
        a.lw("t7", 4, "t8")
        self.eq("t7", (value >> 32) & 0xFFFFFFFF, name + ".hi")

    def eq128(self, reg, words, name):
        a = self.a
        a.la("t8", "scratch")
        a.sq(reg, 0, "t8")
        for i, w in enumerate(words):
            a.la("t8", "scratch")
            a.lw("t7", 4 * i, "t8")
            self.eq("t7", w, f"{name}.w{i}")

    def eqf(self, freg, value, name):
        """Compare with value rounded toward zero to float32 (PS2 FPU rounding)."""
        a = self.a
        a.mfc1("t6", freg)
        self.eq("t6", f32_chop(value), name)


def prologue(a):
    a.func("_start")
    a.li("sp", 0x01F00000)
    a.li("s7", 0)


def epilogue(a):
    a.bnez("s7", "done_fail")
    a.nop()
    a.la("a0", "m_allpass")
    a.li("v1", SYS_PRINT)
    a.syscall()
    a.label("done_fail")
    a.move("a0", "s7")
    a.li("v1", SYS_EXIT)
    a.syscall()
    a.nop()
    # t_fail(a0 = message): print and count
    a.func("t_fail")
    a.li("v1", SYS_PRINT)
    a.syscall()
    a.jr("ra")
    a.addiu("s7", "s7", 1)
    a.dlabel("m_allpass")
    a.asciiz("ALL PASS\n")
    a.dalign(16)
    a.dlabel("scratch")
    a.space(64)


def build_cpu_test(path, symbols=True):
    a = Asm()
    k = Checker(a)
    prologue(a)

    # --- 32-bit arithmetic and sign extension
    a.li("t0", 0x7FFFFFFF)
    a.addiu("t1", "t0", 1)
    k.eq64("t1", 0xFFFFFFFF80000000, "addiu_overflow_sext")
    a.li("t0", 5)
    a.li("t1", 7)
    a.subu("t2", "t0", "t1")
    k.eq("t2", -2, "subu")
    a.addu("t2", "t0", "t1")
    k.eq("t2", 12, "addu")

    # --- 64-bit
    a.li("t0", 1)
    a.dsll32("t1", "t0", 0)
    a.daddiu("t1", "t1", -1)
    k.eq64("t1", 0x00000000FFFFFFFF, "dsll32_daddiu")
    a.li("t0", 0x12345678)
    a.dsll32("t1", "t0", 0)
    a.dsra32("t2", "t1", 4)
    k.eq64("t2", 0x01234567, "dsra32")
    a.li("t0", -1)
    a.dsrl32("t2", "t0", 0)
    k.eq64("t2", 0xFFFFFFFF, "dsrl32")
    a.li("t3", 36)
    a.dsrav("t4", "t1", "t3")
    k.eq64("t4", 0x01234567, "dsrav")
    a.daddu("t5", "t1", "t1")
    k.eq64("t5", 0x2468ACF000000000, "daddu")

    # --- shifts
    a.li("t0", 0x80000001)
    a.sll("t1", "t0", 1)
    k.eq("t1", 2, "sll")
    a.srl("t1", "t0", 1)
    k.eq("t1", 0x40000000, "srl")
    a.sra("t1", "t0", 1)
    k.eq("t1", 0xC0000000, "sra")
    a.li("t2", 33)                       # shift amount uses low 5 bits
    a.sllv("t1", "t0", "t2")
    k.eq("t1", 2, "sllv_mask")
    a.srav("t1", "t0", "t2")
    k.eq("t1", 0xC0000000, "srav")

    # --- comparisons
    a.li("t0", -5)
    a.li("t1", 3)
    a.slt("t2", "t0", "t1")
    k.eq("t2", 1, "slt")
    a.sltu("t2", "t0", "t1")
    k.eq("t2", 0, "sltu")
    a.slti("t2", "t0", -4)
    k.eq("t2", 1, "slti")
    a.sltiu("t2", "t1", -1)              # 3 < 0xFFFF...FFFF
    k.eq("t2", 1, "sltiu_sext")

    # --- logic
    a.li("t0", 0xF0F0F0F0)
    a.li("t1", 0x0FF00FF0)
    getattr(a, "and_")("t2", "t0", "t1")
    k.eq("t2", 0x00F000F0, "and")
    getattr(a, "or_")("t2", "t0", "t1")
    k.eq("t2", 0xFFF0FFF0, "or")
    a.xor("t2", "t0", "t1")
    k.eq("t2", 0xFF00FF00, "xor")
    a.nor("t2", "zero", "zero")
    k.eq("t2", -1, "nor")
    a.andi("t2", "t0", 0xFFFF)
    k.eq("t2", 0xF0F0, "andi_zext")
    a.xori("t2", "t1", 0x0FF0)
    k.eq("t2", 0x0FF00000, "xori")
    a.lui("t2", 0x8000)
    k.eq64("t2", 0xFFFFFFFF80000000, "lui_sext")

    # --- multiply / divide
    a.li("t0", -3)
    a.li("t1", 100000)
    a.mult("t2", "t0", "t1")             # R5900 3-operand form
    k.eq("t2", -300000, "mult_rd")
    a.mfhi("t3")
    k.eq("t3", -1, "mult_hi")
    a.li("t0", 0x80000000)
    a.li("t1", 4)
    a.multu("t0", "t1")
    a.mfhi("t3")
    a.mflo("t4")
    k.eq("t3", 2, "multu_hi")
    k.eq("t4", 0, "multu_lo")
    a.li("t0", -17)
    a.li("t1", 5)
    a.div("t0", "t1")
    a.mflo("t2")
    a.mfhi("t3")
    k.eq("t2", -3, "div_q")
    k.eq("t3", -2, "div_r")
    a.li("t0", 17)
    a.divu("t0", "zero")                 # divide by zero: LO = -1, HI = rs
    a.mflo("t2")
    a.mfhi("t3")
    k.eq("t2", -1, "divu_zero_lo")
    k.eq("t3", 17, "divu_zero_hi")
    a.li("t0", 10)
    a.li("t1", 20)
    a.mtlo("zero")
    a.mthi("zero")
    a.madd("t2", "t0", "t1")
    a.madd("t2", "t0", "t1")
    k.eq("t2", 400, "madd")
    a.mult1("t2", "t0", "t1")
    a.mflo1("t3")
    k.eq("t3", 200, "mult1")
    a.li("t0", 100)
    a.li("t1", 7)
    a.div1("zero", "t0", "t1")
    a.mfhi1("t3")
    k.eq("t3", 2, "div1_rem")

    # --- conditional moves
    a.li("t0", 11)
    a.li("t1", 22)
    a.movz("t0", "t1", "zero")
    k.eq("t0", 22, "movz")
    a.li("t2", 1)
    a.movn("t0", "zero", "t2")
    k.eq("t0", 0, "movn")

    # --- memory
    a.la("s0", "memtest")
    a.li("t0", 0x11223344)
    a.sw("t0", 0, "s0")
    a.lb("t1", 3, "s0")
    k.eq("t1", 0x11, "lb")
    a.li("t0", 0xFF)
    a.sb("t0", 4, "s0")
    a.lb("t1", 4, "s0")
    k.eq("t1", -1, "lb_sext")
    a.lbu("t1", 4, "s0")
    k.eq("t1", 0xFF, "lbu")
    a.li("t0", 0x8001)
    a.sh("t0", 6, "s0")
    a.lh("t1", 6, "s0")
    k.eq("t1", 0xFFFF8001, "lh")
    a.lhu("t1", 6, "s0")
    k.eq("t1", 0x8001, "lhu")
    a.li("t0", 0x89ABCDEF)
    a.sw("t0", 8, "s0")
    a.lwu("t1", 8, "s0")
    k.eq64("t1", 0x89ABCDEF, "lwu")
    a.ld("t1", 0, "s0")
    k.eq64("t1", 0x800100FF11223344, "ld")
    # unaligned word load at offset 1 (bytes 33 22 11 FF)
    a.lwr("t2", 1, "s0")
    a.lwl("t2", 4, "s0")
    k.eq("t2", 0xFF112233, "lwl_lwr")
    # unaligned word store at offset 9
    a.li("t0", 0xA1B2C3D4)
    a.swr("t0", 9, "s0")
    a.swl("t0", 12, "s0")
    a.lbu("t1", 9, "s0")
    k.eq("t1", 0xD4, "swr_b0")
    a.lbu("t1", 12, "s0")
    k.eq("t1", 0xA1, "swl_b3")
    # unaligned doubleword load at offset 2
    a.ldr("t3", 2, "s0")
    a.ldl("t3", 9, "s0")
    a.sd("t3", 32, "s0")
    a.lw("t5", 32, "s0")
    k.eq("t5", 0x00FF1122, "ldl_ldr_lo")
    a.lw("t5", 36, "s0")
    k.eq("t5", 0xD4EF8001, "ldl_ldr_hi")
    a.li("t0", 0x5555AAAA)
    a.dsll32("t0", "t0", 0)
    a.ori("t0", "t0", 0x1234)
    a.sdr("t0", 17, "s0")
    a.sdl("t0", 24, "s0")
    a.lbu("t1", 17, "s0")
    k.eq("t1", 0x34, "sdr_b0")
    a.lbu("t1", 24, "s0")
    k.eq("t1", 0x55, "sdl_b7")
    # lq / sq ignore the low 4 address bits
    a.la("s1", "qdata")
    a.lq("t0", 5, "s1")
    k.eq128("t0", [0x11111111, 0x22222222, 0x33333333, 0x44444444], "lq")

    # --- branches and delay slots
    a.li("t0", 0)
    a.beq("zero", "zero", "br1")
    a.addiu("t0", "t0", 1)               # delay slot executes
    a.addiu("t0", "t0", 100)             # skipped
    a.label("br1")
    k.eq("t0", 1, "beq_delay_slot")
    a.li("t0", 0)
    a.li("t1", 1)
    a.beql("t1", "zero", "br2")
    a.addiu("t0", "t0", 1)               # nullified (not taken)
    a.label("br2")
    k.eq("t0", 0, "beql_nullify")
    a.bnel("t1", "zero", "br3")
    a.addiu("t0", "t0", 5)               # executed (taken)
    a.addiu("t0", "t0", 100)
    a.label("br3")
    k.eq("t0", 5, "bnel_taken")
    a.li("t1", -1)
    a.bgez("t1", "br_bad")
    a.nop()
    a.bltz("t1", "br4")
    a.nop()
    a.label("br_bad")
    a.li("t0", 999)
    a.label("br4")
    k.eq("t0", 5, "bltz_bgez")
    a.li("t1", 0)
    a.bgtz("t1", "br_bad2")
    a.nop()
    a.blez("t1", "br5")
    a.nop()
    a.label("br_bad2")
    a.li("t0", 888)
    a.label("br5")
    k.eq("t0", 5, "blez_bgtz")

    # --- loop with backward branch: sum 1..100
    a.li("t0", 0)
    a.li("t1", 100)
    a.label("loop1")
    a.addu("t0", "t0", "t1")
    a.addiu("t1", "t1", -1)
    a.bnez("t1", "loop1")
    a.nop()
    k.eq("t0", 5050, "loop_sum")

    # --- calls: recursive factorial(10)
    a.jal("fact")
    a.li("a0", 10)
    k.eq("v0", 3628800, "recursive_fact")

    # --- switch through a gcc-style jump table
    for val, expect in ((0, 10), (2, 30), (3, 40), (7, -1)):
        a.jal("switchfn")
        a.li("a0", val)
        k.eq("v0", expect, f"jumptable_{val}")

    # --- indirect calls: data pointer table and lui/addiu constant
    a.la("t0", "fptrs")
    a.lw("t1", 0, "t0")
    a.jalr("ra", "t1")
    a.li("a0", 6)
    k.eq("v0", 12, "jalr_data_ptr")
    a.la_addiu("t1", "triple")
    a.jalr("ra", "t1")
    a.li("a0", 6)
    k.eq("v0", 18, "jalr_code_const")
    # pointer into the middle of a function (secondary entry)
    a.la("t0", "fptrs")
    a.lw("t1", 4, "t0")
    a.jalr("ra", "t1")
    a.li("a0", 6)
    k.eq("v0", 106, "secondary_entry")

    # --- tail call through j
    a.jal("tailer")
    a.li("a0", 1)
    k.eq("v0", 2 * 3, "tail_call")

    # --- MMI
    a.la("s1", "qdata")
    a.lq("t0", 0, "s1")                  # 11111111 22222222 33333333 44444444
    a.lq("t1", 16, "s1")                 # 1 2 3 4 (words)
    a.paddw("t2", "t0", "t1")
    k.eq128("t2", [0x11111112, 0x22222224, 0x33333336, 0x44444448], "paddw")
    a.psubw("t2", "t0", "t1")
    k.eq128("t2", [0x11111110, 0x22222220, 0x33333330, 0x44444440], "psubw")
    a.pextlw("t2", "t0", "t1")
    k.eq128("t2", [1, 0x11111111, 2, 0x22222222], "pextlw")
    a.pcpyld("t2", "t0", "t1")
    k.eq128("t2", [1, 2, 0x11111111, 0x22222222], "pcpyld")
    a.pcpyud("t2", "t0", "t1")
    k.eq128("t2", [0x33333333, 0x44444444, 3, 4], "pcpyud")
    a.ppacw("t2", "t0", "t1")
    k.eq128("t2", [1, 3, 0x11111111, 0x33333333], "ppacw")
    a.pcgtw("t2", "t0", "t1")
    k.eq128("t2", [-1 & 0xFFFFFFFF] * 4, "pcgtw")
    a.mtsab("zero", 4)
    a.qfsrv("t2", "t0", "t1")            # (t0:t1) >> 32
    k.eq128("t2", [2, 3, 4, 0x11111111], "qfsrv")
    a.pmultw("t2", "t1", "t1")           # products of words 0 and 2: 1, 9
    a.pmfhl_lw("t3", "zero")
    k.eq128("t3", [1, 0, 9, 0], "pmultw_pmfhl")
    a.lq("t4", 32, "s1")                 # halfwords 1..8
    a.paddh("t2", "t4", "t4")
    k.eq128("t2", [0x00040002, 0x00080006, 0x000C000A, 0x0010000E], "paddh")
    a.pextlh("t2", "t4", "zero")
    k.eq128("t2", [0x00010000, 0x00020000, 0x00030000, 0x00040000], "pextlh")
    a.pinth("t2", "t4", "zero")
    k.eq128("t2", [0x00050000, 0x00060000, 0x00070000, 0x00080000], "pinth")
    a.li("t5", 0x0000FFFF)
    a.plzcw("t2", "t5")
    a.sll("t2", "t2", 0)
    k.eq("t2", 15, "plzcw")
    getattr(a, "pand")("t2", "t0", "t1")
    k.eq128("t2", [1, 2, 3, 4], "pand")
    a.pnor("t2", "zero", "zero")
    k.eq128("t2", [0xFFFFFFFF] * 4, "pnor")

    # --- FPU
    a.li("t0", 7)
    a.mtc1("t0", "f1")
    a.cvt_s_w("f1", "f1")                # 7.0
    a.la("t1", "fconst")
    a.lwc1("f2", 0, "t1")                # 2.5
    a.add_s("f3", "f1", "f2")
    k.eqf("f3", 9.5, "add_s")
    a.mul_s("f4", "f1", "f2")
    k.eqf("f4", 17.5, "mul_s")
    a.div_s("f5", "f1", "f2")
    k.eqf("f5", 2.8, "div_s")
    a.sqrt_s("f6", "f2")
    k.eqf("f6", 2.5 ** 0.5, "sqrt_s")
    a.cvt_w_s("f7", "f4")                # trunc(17.5) = 17
    a.mfc1("t2", "f7")
    k.eq("t2", 17, "cvt_w_s")
    a.neg_s("f8", "f2")
    k.eqf("f8", -2.5, "neg_s")
    a.abs_s("f8", "f8")
    k.eqf("f8", 2.5, "abs_s")
    a.mtc1("zero", "f9")
    a.div_s("f10", "f1", "f9")           # x/0 -> +max on PS2
    a.mfc1("t2", "f10")
    k.eq("t2", 0x7F7FFFFF, "div_zero_clamp")
    a.li("t3", 0)
    a.c_lt_s("f2", "f1")                 # 2.5 < 7
    a.bc1t("fp1")
    a.nop()
    a.li("t3", 1)
    a.label("fp1")
    k.eq("t3", 0, "c_lt_bc1t")
    a.c_eq_s("f2", "f1")
    a.bc1f("fp2")
    a.nop()
    a.li("t3", 2)
    a.label("fp2")
    k.eq("t3", 0, "c_eq_bc1f")
    a.mula_s("f1", "f2")                 # ACC = 17.5
    a.madd_s("f11", "f1", "f1")          # 17.5 + 49
    k.eqf("f11", 66.5, "mula_madd")

    # --- VU0 macro mode
    a.la("s1", "vdata")
    a.lqc2("vf1", 0, "s1")               # 1 2 3 4
    a.lqc2("vf2", 16, "s1")              # 10 20 30 40
    a.vadd_xyzw("vf3", "vf1", "vf2")
    a.qmfc2("t0", "vf3")
    k.eq128("t0", [f2u(11), f2u(22), f2u(33), f2u(44)], "vadd")
    a.vmulx_xyz("vf4", "vf2", "vf1")     # vf2.xyz * vf1.x ; w untouched (0)
    a.qmfc2("t0", "vf4")
    k.eq128("t0", [f2u(10), f2u(20), f2u(30), 0], "vmulx_mask")
    a.vadd_w("vf5", "vf0", "vf0")        # vf0.w = 1 -> 2
    a.qmfc2("t0", "vf5")
    k.eq128("t0", [0, 0, 0, f2u(2)], "vf0_const")
    a.la("s1", "qdata")
    a.lq("t1", 16, "s1")
    a.qmtc2("t1", "vf6")
    a.qmfc2("t2", "vf6")
    k.eq128("t2", [1, 2, 3, 4], "qmtc2_roundtrip")

    epilogue(a)

    # ----------------------------------------------------------- helpers
    a.func("fact")
    a.addiu("sp", "sp", -16)
    a.sw("ra", 0, "sp")
    a.sw("s0", 4, "sp")
    a.move("s0", "a0")
    a.li("t0", 1)
    a.slt("t1", "t0", "a0")
    a.beqz("t1", "fact_base")
    a.li("v0", 1)
    a.jal("fact")
    a.addiu("a0", "a0", -1)
    a.mult("v0", "v0", "s0")
    a.label("fact_base")
    a.lw("ra", 0, "sp")
    a.lw("s0", 4, "sp")
    a.jr("ra")
    a.addiu("sp", "sp", 16)

    a.func("switchfn")
    a.sltiu("at", "a0", 4)
    a.beqz("at", "sw_default")
    a.sll("v0", "a0", 2)
    a.emit(lambda _=None: (15 << 26) | (1 << 16) | (((a.addr_of("jtab") + 0x8000) >> 16) & 0xFFFF))
    a.addu("at", "at", "v0")
    a.emit(lambda _=None: (35 << 26) | (1 << 21) | (2 << 16) | (a.addr_of("jtab") & 0xFFFF))
    a.jr("v0")
    a.nop()
    for i in range(4):
        a.label(f"case{i}")
        a.jr("ra")
        a.li("v0", 10 * (i + 1))
    a.label("sw_default")
    a.jr("ra")
    a.li("v0", -1)

    a.func("double")
    a.jr("ra")
    a.addu("v0", "a0", "a0")

    a.func("triple")
    a.addu("v0", "a0", "a0")
    a.jr("ra")
    a.addu("v0", "v0", "a0")

    a.func("withmid")
    a.addiu("a0", "a0", 1000)
    a.label("withmid_inner")
    a.jr("ra")
    a.addiu("v0", "a0", 100)

    a.func("tailer")
    a.addiu("a0", "a0", 2)
    a.j("triple_tail")
    a.nop()
    a.func("triple_tail")
    a.addu("v0", "a0", "a0")
    a.jr("ra")
    a.addu("v0", "v0", "zero")

    # ----------------------------------------------------------- data
    a.dalign(16)
    a.dlabel("memtest")
    a.space(64)
    a.dalign(16)
    a.dlabel("qdata")
    a.word(0x11111111, 0x22222222, 0x33333333, 0x44444444)
    a.word(1, 2, 3, 4)
    a.word(0x00020001, 0x00040003, 0x00060005, 0x00080007)
    a.dlabel("jtab")
    a.word("case0", "case1", "case2", "case3")
    a.dlabel("fptrs")
    a.word("double", "withmid_inner")
    a.dlabel("fconst")
    a.floats(2.5)
    a.dalign(16)
    a.dlabel("vdata")
    a.floats(1, 2, 3, 4, 10, 20, 30, 40)
    a.write_elf(path, symbols=symbols)
    return k.names


def f32_chop(x):
    """float32 bits of x rounded toward zero."""
    bits = struct.unpack("<I", struct.pack("<f", x))[0]
    if abs(struct.unpack("<f", struct.pack("<I", bits))[0]) > abs(x):
        bits -= 1
    return bits


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def rd_u32(b, off):
    return struct.unpack_from("<I", b, off)[0]
