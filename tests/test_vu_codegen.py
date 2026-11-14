"""Execution tests of VU programs translated to C by ps2recomp.vu (S3-M1).

Each test assembles a small micro program, translates it, compiles it with gcc/clang
into build/vu_scratch (see vu_harness.py) and checks the final state.  Expected values
come from the microVU semantics cited in vu.py.  Run:  python tests/test_vu_codegen.py
(skipped when no gcc/clang is installed).  Never touches build/games or the game exe.
"""

import math
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import vu_harness as H  # noqa: E402
from vu_harness import vu  # noqa: E402
import programs_sys as PS  # noqa: E402

NOPU = vu.encode_upper("NOP")
NOPL = 0x8000033C
E = vu.E_BIT
U = vu.encode_upper
L = vu.encode_lower
f2u, u2f = vu.f2u, vu.u2f
FMAX = 0x7F7FFFFF


def end(prog):
    """Append the E-bit instruction + delay slot."""
    return prog + [(NOPL, NOPU | E), (NOPL, NOPU)]


def off(src, dst):
    """Branch immediate from the instruction at `src` to `dst` (bytes)."""
    return (dst - src - 8) // 8


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class FMAC(unittest.TestCase):
    def setUp(self):
        s = H.State()
        s.setf(1, 1.0, 2.0, 3.0, 4.0)
        s.setf(2, 0.5, -1.0, 2.0, 8.0)
        s.acc = [f2u(v) for v in (10.0, 20.0, 30.0, 40.0)]
        s.i, s.q = f2u(2.0), f2u(4.0)
        self.s = s

    def test_arith_variants(self):
        prog = end([
            (NOPL, U("ADD", fd=3, fs=1, ft=2)),
            (NOPL, U("SUBz", "xy", fd=4, fs=1, ft=2)),
            (NOPL, U("MULi", fd=5, fs=1)),
            (NOPL, U("MADDq", fd=6, fs=1)),
            (NOPL, U("MSUBw", fd=7, fs=1, ft=2)),
            (NOPL, U("MAX", fd=9, fs=1, ft=2)),
            (NOPL, U("MINIi", fd=10, fs=1)),
            (NOPL, U("OPMULA", "xyz", fs=1, ft=2)),
            (NOPL, U("OPMSUB", "xyz", fd=11, fs=2, ft=1)),
            (NOPL, U("ADD", fd=0, fs=1, ft=1)),            # vf0 is read-only
            (NOPL, U("MULA", fs=1, ft=2)),
            (NOPL, U("MADDAx", fs=1, ft=2)),
            (NOPL, U("SUB", "xw", fd=12, fs=1, ft=1)),     # x - x = +0 (doSafeSub)
        ])
        r = H.run(prog, self.s)
        self.assertEqual(r.getf(3), [1.5, 1.0, 5.0, 12.0])
        self.assertEqual(r.getf(4), [-1.0, 0.0, 0.0, 0.0])
        self.assertEqual(r.getf(5), [2.0, 4.0, 6.0, 8.0])
        self.assertEqual(r.getf(6), [14.0, 28.0, 42.0, 56.0])
        self.assertEqual(r.getf(7), [2.0, 4.0, 6.0, 8.0])
        self.assertEqual(r.getf(9), [1.0, 2.0, 3.0, 8.0])
        self.assertEqual(r.getf(10), [1.0, 2.0, 2.0, 2.0])
        self.assertEqual(r.getf(11)[:3], [7.0, -0.5, -2.0])  # cross product vf1 x vf2
        self.assertEqual(r.vf[0], [0, 0, 0, f2u(1.0)])
        # ACC = vf1*vf2 + vf1*vf2.x
        self.assertEqual([u2f(u) for u in r.acc], [0.5 + 0.5, -2.0 + 1.0, 6.0 + 1.5, 32.0 + 2.0])
        self.assertEqual(r.vf[12], [0, 0, 0, 0])

    def test_max_mini_bits(self):
        s = H.State()
        s.vf[1] = [f2u(-1.0), 0x80000000, f2u(-3.0), 0x7F800000]
        s.vf[2] = [f2u(-2.0), 0x00000000, f2u(1.0), 0xFF800000]
        prog = end([(NOPL, U("MAX", fd=3, fs=1, ft=2)), (NOPL, U("MINI", fd=4, fs=1, ft=2))])
        r = H.run(prog, s)
        self.assertEqual(r.vf[3], [f2u(-1.0), 0, f2u(1.0), 0x7F800000])
        self.assertEqual(r.vf[4], [f2u(-2.0), 0x80000000, f2u(-3.0), 0xFF800000])

    def test_convert_abs(self):
        s = H.State()
        s.setf(1, 1.9, -1.9, 3e9, -3e9)
        s.setf(2, 1.5, -0.25, 0.5, 2.0)
        s.vf[3] = [16, 0xFFFFFFFD, 32768, 4096]
        s.vf[4] = [0x7FC00000, 0xFFC00000, 0x7F800000, 0]
        prog = end([
            (NOPL, U("FTOI0", ft=5, fs=1)),
            (NOPL, U("FTOI4", ft=6, fs=2)),
            (NOPL, U("FTOI12", "x", ft=7, fs=2)),
            (NOPL, U("FTOI15", "w", ft=7, fs=2)),
            (NOPL, U("ITOF4", "x", ft=8, fs=3)),
            (NOPL, U("ITOF0", "y", ft=8, fs=3)),
            (NOPL, U("ITOF15", "z", ft=8, fs=3)),
            (NOPL, U("ITOF12", "w", ft=8, fs=3)),
            (NOPL, U("ABS", ft=9, fs=1)),
            (NOPL, U("FTOI0", ft=10, fs=4)),
        ])
        r = H.run(prog, s)
        self.assertEqual(r.vf[5], [1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000])
        self.assertEqual(r.vf[6], [24, 0xFFFFFFFC, 8, 32])
        self.assertEqual(r.vf[7], [6144, 0, 0, 65536])
        self.assertEqual(r.getf(8), [1.0, -3.0, 1.0, 1.0])
        self.assertEqual(r.getf(9)[:2], [u2f(f2u(1.9)), u2f(f2u(1.9))])
        self.assertEqual(r.vf[10], [0x7FFFFFFF, 0x80000000, 0x7FFFFFFF, 0])

    def test_round_toward_zero(self):
        """MXCSR chop mode: 1/3 = 0x3EAAAAAA (round-to-nearest gives ...AB)."""
        s = H.State()
        s.setf(1, 1.0, 3.0, 0, 0)
        s.setf(2, 1.0, 3.0, 0.1, 0.2)
        prog = end([
            (L("DIV", fs=1, fsf=0, ft=1, ftf=1), NOPU),
            (L("WAITQ", ""), U("ADDq", "x", fd=3, fs=0)),
            (NOPL, U("ADD", "x", fd=4, fs=2, ft=2)),
            (NOPL, U("ADDw", "y", fd=4, fs=2, ft=2)),       # 0.1f + 0.2f
        ])
        r = H.run(prog, s)
        self.assertEqual(r.vf[3][0], 0x3EAAAAAA)
        self.assertEqual(r.vf[4][1], 0x404CCCCC)            # 3.0 + 0.2: RZ (nearest gives ...CD)


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class Clamp(unittest.TestCase):
    def _state(self):
        s = H.State()
        s.vf[1] = [0x7F800000, 0x7FC00000, 0xFF800000, 0xFFFFFFFF]   # +Inf NaN -Inf -NaN
        s.vf[2] = [0x7F000000, 0x7F000000, 0xFF000000, 0x00000000]
        return s

    PROG = end([
        (NOPL, U("ADD", fd=3, fs=1, ft=0)),
        (NOPL, U("MULw", fd=4, fs=1, ft=0)),
        (NOPL, U("MUL", "xyz", fd=5, fs=2, ft=2)),
    ])

    def test_mode2_extra(self):
        """SH3 GameDB vuClampMode 2: every SSE op clamps operands and result."""
        r = H.run(self.PROG, self._state(), clamp_mode=2)
        self.assertEqual(r.vf[3], [FMAX, FMAX, 0xFF7FFFFF, FMAX])
        self.assertEqual(r.vf[4], [FMAX, FMAX, 0xFF7FFFFF, FMAX])
        self.assertEqual(r.vf[5][:3], [FMAX, FMAX, FMAX])

    def test_mode1_default(self):
        """PCSX2 default: only the per-opcode operand clamps (MUL: cFs|cFt), results raw."""
        r = H.run(self.PROG, self._state(), clamp_mode=1)
        self.assertEqual(r.vf[3][0], 0x7F800000)                     # ADD: no clamp
        self.assertEqual(r.vf[3][2], 0xFF800000)
        self.assertEqual(r.vf[4], [FMAX, FMAX, 0xFF7FFFFF, FMAX])    # MUL operands clamped
        # overflow under round-toward-zero gives FLT_MAX (IEEE), not Inf, even unclamped
        self.assertEqual(r.vf[5][:3], [FMAX, FMAX, FMAX])

    def test_ibit_clamped(self):
        prog = end([(0x7F800000, U("ADDi", "x", fd=3, fs=0) | vu.I_BIT),
                    (NOPL, U("ADDi", "x", fd=4, fs=0))])
        r = H.run(prog, H.State())
        self.assertEqual(r.i, FMAX)
        self.assertEqual(r.vf[4][0], FMAX)


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class Flags(unittest.TestCase):
    def test_mac_latency(self):
        """FMAND sees a MAC write 4 cycles later, not 3 (flag instances)."""
        s = H.State()
        s.vi[3] = 0xFFFF
        s.setf(1, -1.0, 0, 0, 0)
        prog = end([
            (NOPL, U("ADD", "xy", fd=2, fs=0, ft=1)),               # x = -1 (S), y = 0 (Z)
            (L("FMAND", "", it=4, is_=3), NOPU),
            (L("FMAND", "", it=5, is_=3), NOPU),
            (L("FMAND", "", it=6, is_=3), NOPU),
            (L("FMAND", "", it=7, is_=3), NOPU),
            (L("FSAND", "", it=8, imm=0xFFF), NOPU),
        ])
        r = H.run(prog, s)
        self.assertEqual(r.vi[4:7], [0, 0, 0])
        self.assertEqual(r.vi[7], 0x80 | 0x04)                      # Sx | Zy
        self.assertEqual(r.vi[8], 0x1 | 0x2 | 0x40 | 0x80)          # Z S ZS SS
        self.assertEqual(r.mac, 0x84)

    def test_status_sticky_and_fsset(self):
        s = H.State()
        s.setf(1, 1.0, 1.0, 1.0, 1.0)
        prog = end([
            (NOPL, U("ADD", "x", fd=2, fs=0, ft=0)),                # zero -> Z, ZS
            (NOPL, U("ADD", "x", fd=2, fs=1, ft=1)),                # nonzero -> Z cleared
        ])
        r = H.run(prog, s)
        self.assertEqual(r.status, 0x40)
        prog = end([(L("FSSET", "", imm=0x800), NOPU)])
        s = H.State()
        s.status = 0x0C3                                            # Z S ZS SS
        r = H.run(prog, s)
        self.assertEqual(r.status, 0x803)
        prog = end([(L("FSEQ", "", it=1, imm=0x0C3), NOPU), (L("FSOR", "", it=2, imm=0x100), NOPU)])
        s = H.State()
        s.status = 0x0C3
        r = H.run(prog, s)
        self.assertEqual((r.vi[1], r.vi[2]), (1, 0x1C3))

    def test_clip_flags(self):
        s = H.State()
        s.setf(1, 2.0, -2.0, 0.5, 0)
        s.setf(2, 0, 0, 0, -1.0)
        s.clip = 0x3F
        prog = end([
            (NOPL, U("CLIP", "xyz", fs=1, ft=2)),
            (L("FCGET", "", it=2), NOPU),                            # old value (latency)
            (NOPL, NOPU), (NOPL, NOPU),
            (L("FCAND", imm=0x9), NOPU),
            (L("FCGET", "", it=3), NOPU),
            (L("FCEQ", imm=0xFC9), NOPU),
        ])
        r = H.run(prog, s)
        self.assertEqual(r.vi[2], 0x3F)
        self.assertEqual(r.vi[3], 0xFC9)
        self.assertEqual(r.vi[1], 1)                                 # FCEQ
        self.assertEqual(r.clip, 0xFC9)
        prog = end([(L("FCSET", imm=0x123456), NOPU), (NOPL, NOPU), (NOPL, NOPU), (NOPL, NOPU),
                    (L("FCOR", imm=0xEDCBA9), NOPU)])
        r = H.run(prog, H.State())
        self.assertEqual((r.clip, r.vi[1]), (0x123456, 1))


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class QP(unittest.TestCase):
    def test_div_latency(self):
        s = H.State()
        s.setf(1, 6.0, 0, 0, 0)
        s.setf(2, 3.0, 0, 0, 0)
        s.setf(4, 1.0, 1.0, 1.0, 1.0)
        s.q = f2u(1.0)
        prog = end([
            (L("DIV", fs=1, fsf=0, ft=2, ftf=0), NOPU),             # cycle 1, ready 8
            (NOPL, U("MULq", "x", fd=3, fs=4)),                     # cycle 2: old Q
            (NOPL, NOPU), (NOPL, NOPU), (NOPL, NOPU), (NOPL, NOPU),
            (NOPL, U("MULq", "y", fd=3, fs=4)),                     # cycle 7: old
            (NOPL, U("MULq", "z", fd=3, fs=4)),                     # cycle 8: new
        ])
        r = H.run(prog, s)
        self.assertEqual(r.getf(3)[:3], [1.0, 1.0, 2.0])
        self.assertEqual(u2f(r.q), 2.0)

    def test_waitq_stall(self):
        s = H.State()
        s.setf(1, 6.0, 0, 0, 0)
        s.setf(2, 3.0, 0, 0, 0)
        prog = end([(L("RSQRT", fs=1, fsf=0, ft=2, ftf=0), NOPU),
                    (L("WAITQ", ""), U("ADDq", "x", fd=3, fs=0))])
        r = H.run(prog, s)
        self.assertAlmostEqual(r.getf(3)[0], 6.0 / math.sqrt(3.0), places=5)
        self.assertEqual(r.cycles, 1 + 13 + 2)                      # RSQRT 13 cycles

    def test_fdiv_flags(self):
        s = H.State()
        s.setf(1, 5.0, 0.0, -4.0, 0)
        prog = end([(L("DIV", fs=1, fsf=0, ft=0, ftf=0), NOPU),    # 5/0 -> D, +max
                    (L("WAITQ", ""), U("ADDq", "x", fd=2, fs=0))])
        r = H.run(prog, s)
        self.assertEqual(r.vf[2][0], FMAX)
        self.assertEqual(r.status & 0xC30, 0x820)
        prog = end([(L("DIV", fs=1, fsf=1, ft=0, ftf=0), NOPU)])  # 0/0 -> I
        r = H.run(prog, s)
        self.assertEqual(r.status & 0xC30, 0x410)
        prog = end([(L("SQRT", ft=1, ftf=2), NOPU)])               # sqrt(-4) = 2, I
        r = H.run(prog, s)
        self.assertEqual((u2f(r.q), r.status & 0x30), (2.0, 0x10))
        s.status = 0x830
        prog = end([(L("DIV", fs=1, fsf=0, ft=1, ftf=0), NOPU)])  # ok: I/D cleared, DS kept
        r = H.run(prog, s)
        self.assertEqual((u2f(r.q), r.status), (1.0, 0x800))

    def test_efu(self):
        s = H.State()
        s.setf(1, 1.0, 2.0, 2.0, 4.0)
        s.setf(2, 3.0, 4.0, 0.0, 0.0)
        s.setf(3, 0.5, 1.0, -9.0, 4.0)
        s.setf(4, 2.0, 1.0, 0.0, 0.0)
        s.p = f2u(-1.0)
        mfp = lambda fd, d: (L("MFP", d, ft=fd), NOPU)
        prog = end([
            (L("ESADD", "xyz", fs=1), NOPU),
            mfp(5, "x"),                                            # too early: old P
            (L("WAITP", ""), NOPU), mfp(5, "y"),                    # 9
            (L("ERCPR", fs=1, fsf=3), NOPU), (L("WAITP", ""), NOPU), mfp(5, "z"),
            (L("ESUM", fs=1), NOPU), (L("WAITP", ""), NOPU), mfp(5, "w"),
            (L("ELENG", "xyz", fs=2), NOPU), (L("WAITP", ""), NOPU), mfp(6, "x"),
            (L("ERLENG", "xyz", fs=2), NOPU), (L("WAITP", ""), NOPU), mfp(6, "y"),
            (L("ESQRT", fs=3, fsf=2), NOPU), (L("WAITP", ""), NOPU), mfp(6, "z"),
            (L("ERSQRT", fs=3, fsf=3), NOPU), (L("WAITP", ""), NOPU), mfp(6, "w"),
            (L("ESIN", fs=3, fsf=0), NOPU), (L("WAITP", ""), NOPU), mfp(7, "x"),
            (L("EATAN", fs=3, fsf=1), NOPU), (L("WAITP", ""), NOPU), mfp(7, "y"),
            (L("EEXP", fs=3, fsf=1), NOPU), (L("WAITP", ""), NOPU), mfp(7, "z"),
            (L("EATANxy", fs=4), NOPU), (L("WAITP", ""), NOPU), mfp(7, "w"),
            (L("ERSADD", "xyz", fs=1), NOPU), (L("WAITP", ""), NOPU), mfp(8, "x"),
            (L("EATANxz", fs=4), NOPU), (L("WAITP", ""), NOPU), mfp(8, "y"),
        ])
        r = H.run(prog, s)
        self.assertEqual(r.getf(5), [-1.0, 9.0, 0.25, 9.0])
        v6 = r.getf(6)
        self.assertEqual(v6[0], 5.0)
        self.assertAlmostEqual(v6[1], 0.2, places=6)
        self.assertEqual(v6[2:], [3.0, 0.5])
        v7 = r.getf(7)
        self.assertAlmostEqual(v7[0], math.sin(0.5), places=5)
        self.assertAlmostEqual(v7[1], math.pi / 4, places=5)
        self.assertAlmostEqual(v7[2], math.exp(-1.0), places=4)
        self.assertAlmostEqual(v7[3], math.atan(0.5), places=5)
        v8 = r.getf(8)
        self.assertAlmostEqual(v8[0], 1.0 / 9.0, places=6)
        self.assertAlmostEqual(v8[1], math.atan(0.0) + 0.0, places=5)  # z = 0: atan(0)


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class IntegerAndMemory(unittest.TestCase):
    def test_ialu(self):
        s = H.State()
        s.vi[1], s.vi[2] = 0x00F0, 0x0F0F
        prog = end([
            (L("IADD", "", id_=3, is_=1, it=2), NOPU),
            (L("ISUB", "", id_=4, is_=1, it=2), NOPU),
            (L("IAND", "", id_=5, is_=1, it=2), NOPU),
            (L("IOR", "", id_=6, is_=1, it=2), NOPU),
            (L("IADDI", "", it=7, is_=1, imm=-16), NOPU),
            (L("IADDIU", "", it=8, is_=0, imm=0x7FFF), NOPU),
            (L("ISUBIU", "", it=9, is_=1, imm=0x100), NOPU),
            (L("IADDIU", "", it=0, is_=1, imm=5), NOPU),             # vi0 stays 0
        ])
        r = H.run(prog, s)
        self.assertEqual(r.vi[3:10], [0x0FFF, 0xF1E1, 0x0000, 0x0FFF, 0x00E0, 0x7FFF, 0xFFF0])
        self.assertEqual(r.vi[0], 0)

    def test_loads_stores(self):
        s = H.State()
        for q in range(8):
            s.mem[q * 16:q * 16 + 16] = struct.pack("<4I", *(q * 16 + k for k in range(4)))
        s.vi[1], s.vi[2], s.vi[3] = 2, 5, 0x3FF
        s.setf(9, 1.0, 2.0, 3.0, 4.0)
        prog = end([
            (L("LQ", "xz", ft=1, is_=1, imm=-1), NOPU),             # qword 1
            (L("LQI", ft=2, is_=1), NOPU),                          # qword 2, vi1 = 3
            (L("LQD", ft=3, is_=2), NOPU),                          # vi2 = 4, qword 4
            (L("SQ", "yw", fs=9, it=3, imm=2), NOPU),               # 0x3FF + 2 wraps to 1
            (L("SQI", fs=9, it=2), NOPU),                           # qword 4, vi2 = 5
            (L("SQD", "x", fs=9, it=1), NOPU),                      # vi1 = 2, qword 2
            (L("ILW", "z", it=4, is_=0, imm=6), NOPU),
            (L("ISW", "xw", it=3, is_=0, imm=7), NOPU),
            (L("ILWR", "w", it=5, is_=1), NOPU),
            (L("ISWR", "y", it=1, is_=1), NOPU),
        ])
        r = H.run(prog, s)
        self.assertEqual([r.vf[1][0], r.vf[1][2]], [16, 18])
        self.assertEqual(r.vf[2], [32, 33, 34, 35])
        self.assertEqual(r.vf[3], [64, 65, 66, 67])
        self.assertEqual(struct.unpack_from("<4I", r.mem, 16), (16, f2u(2.0), 18, f2u(4.0)))
        self.assertEqual(struct.unpack_from("<4f", r.mem, 64), (1.0, 2.0, 3.0, 4.0))
        self.assertEqual(struct.unpack_from("<4I", r.mem, 32), (f2u(1.0), 2, 34, 35))
        self.assertEqual((r.vi[1], r.vi[2]), (2, 5))
        self.assertEqual(r.vi[4], 98)
        self.assertEqual(struct.unpack_from("<4I", r.mem, 112), (0x3FF, 113, 114, 0x3FF))
        self.assertEqual(r.vi[5], 35)
        self.assertEqual(struct.unpack_from("<I", r.mem, 36)[0], 2)

    def test_moves(self):
        s = H.State()
        s.setf(1, 1.0, 2.0, 3.0, 4.0)
        s.vi[2] = 0xFFFE
        prog = end([
            (L("MOVE", "xw", ft=3, fs=1), NOPU),
            (L("MR32", ft=4, fs=1), NOPU),
            (L("MFIR", "yz", ft=5, is_=2), NOPU),
            (L("MTIR", it=6, fs=1, fsf=3), NOPU),
        ])
        r = H.run(prog, s)
        self.assertEqual(r.getf(3), [1.0, 0.0, 0.0, 4.0])
        self.assertEqual(r.getf(4), [2.0, 3.0, 4.0, 1.0])
        self.assertEqual(r.vf[5], [0, 0xFFFFFFFE, 0xFFFFFFFE, 0])
        self.assertEqual(r.vi[6], f2u(4.0) & 0xFFFF)

    def test_random_and_top(self):
        s = H.State()
        s.setf(1, 0, 0, 0, 0)
        s.vf[1][0] = 0x12345678
        s.top, s.itop = 0x1234, 0x7FF
        prog = end([
            (L("RINIT", fs=1, fsf=0), NOPU),
            (L("RGET", "x", ft=2), NOPU),
            (L("RNEXT", "y", ft=2), NOPU),
            (L("RXOR", fs=1, fsf=0), NOPU),
            (L("RGET", "z", ft=2), NOPU),
            (L("XTOP", "", it=3), NOPU),
            (L("XITOP", "", it=4), NOPU),
        ])
        r = H.run(prog, s)
        r0 = 0x3F800000 | (0x12345678 & 0x7FFFFF)
        bit = ((r0 >> 4) ^ (r0 >> 22)) & 1
        r1 = 0x3F800000 | (((r0 << 1) ^ bit) & 0x7FFFFF)
        self.assertEqual(r.vf[2][:3], [r0, r1, r1 ^ (0x12345678 & 0x7FFFFF)])
        self.assertEqual((r.vi[3], r.vi[4]), (0x1234, 0x3FF))

    def test_pair_semantics(self):
        """Both halves read before either writes; upper wins a VF write collision."""
        s = H.State()
        s.setf(1, 1.0, 1.0, 1.0, 1.0)
        s.setf(2, 5.0, 6.0, 7.0, 8.0)
        s.setf(3, 9.0, 9.0, 9.0, 9.0)
        prog = end([
            (L("MOVE", ft=2, fs=3), U("ADD", "x", fd=2, fs=1, ft=1)),   # lower dropped
            (L("SQ", fs=4, it=0, imm=0), U("ADD", fd=4, fs=1, ft=1)),   # SQ sees old vf4
            (L("MOVE", ft=5, fs=6), U("ADD", fd=6, fs=1, ft=1)),        # MOVE sees old vf6
        ])
        r = H.run(prog, s)
        self.assertEqual(r.getf(2), [2.0, 6.0, 7.0, 8.0])
        self.assertEqual(struct.unpack_from("<4I", r.mem, 0), (0, 0, 0, 0))
        self.assertEqual(r.vf[5], [0, 0, 0, 0])
        self.assertEqual(r.getf(6), [2.0, 2.0, 2.0, 2.0])


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class Control(unittest.TestCase):
    def test_loop_delay_slot(self):
        s = H.State()
        s.vi[1] = 5
        prog = [
            (L("IADDI", "", it=1, is_=1, imm=-1), NOPU),            # 00
            (NOPL, NOPU),                                           # 08
            (L("IBNE", "", it=0, is_=1, imm=off(0x10, 0)), NOPU),   # 10
            (L("IADDIU", "", it=2, is_=2, imm=1), NOPU),            # 18 delay slot
        ]
        r = H.run(end(prog), s)
        self.assertEqual((r.vi[1], r.vi[2], r.rc, r.tpc), (0, 5, 0, 0x30))

    def test_bal_jr(self):
        prog = [
            (L("BAL", "", it=15, imm=off(0x00, 0x30)), NOPU),        # 00
            (L("IADDIU", "", it=1, is_=0, imm=1), NOPU),             # 08 delay
            (L("IADDIU", "", it=3, is_=0, imm=3), NOPU),             # 10 return point
            (NOPL, NOPU | E), (NOPL, NOPU),                          # 18, 20
            (NOPL, NOPU),                                            # 28
            (L("IADDIU", "", it=2, is_=0, imm=2), NOPU),             # 30
            (L("JR", "", is_=15), NOPU),                             # 38
            (NOPL, NOPU),                                            # 40
        ]
        r = H.run(prog, H.State())
        self.assertEqual((r.vi[1], r.vi[2], r.vi[3], r.vi[15]), (1, 2, 3, 2))
        self.assertEqual((r.rc, r.tpc), (0, 0x28))

    def test_jr_out_of_program(self):
        s = H.State()
        s.vi[1] = 0x100
        r = H.run([(L("JR", "", is_=1), NOPU), (NOPL, NOPU)], s)
        self.assertEqual((r.rc, r.tpc), (2, 0x800))                  # VU_EXIT_JUMP

    def test_branch_vi_delay(self):
        """analyzeBranchVI: IBNE right after VI writes uses the value from before them."""
        s = H.State()
        prog = [
            (L("IADDIU", "", it=1, is_=0, imm=5), NOPU),             # 00
            (L("IADDI", "", it=1, is_=1, imm=-1), NOPU),             # 08
            (L("IBNE", "", it=0, is_=1, imm=off(0x10, 0x30)), NOPU),  # 10 sees vi1 = 0
            (NOPL, NOPU),                                            # 18
            (L("IADDIU", "", it=2, is_=0, imm=1), NOPU),             # 20 not-taken path
            (NOPL, NOPU | E), (NOPL, NOPU),                          # 28 30
            (NOPL, NOPU | E), (NOPL, NOPU),                          # 38 40
        ]
        r = H.run(prog, s)
        self.assertEqual((r.vi[1], r.vi[2]), (4, 1))

    def test_tbit(self):
        prog = end([
            (L("IADDIU", "", it=1, is_=0, imm=7), NOPU | vu.T_BIT),
            (L("IADDIU", "", it=2, is_=0, imm=9), NOPU),
        ])
        s = H.State()
        s.fbrst = 0x800
        r = H.run(prog, s)
        self.assertEqual((r.rc, r.tpc, r.vi[1], r.vi[2]), (1, 8, 7, 0))
        s = H.State()
        s.fbrst = 0x800
        r = H.run(prog, s, pc=8)                                     # resume
        self.assertEqual((r.rc, r.vi[2]), (0, 9))
        r = H.run(prog, H.State())                                   # TE clear: no stop
        self.assertEqual((r.rc, r.vi[2]), (0, 9))

    def test_ibit_order(self):
        """I is set after the upper op of the same instruction (doIbit)."""
        s = H.State()
        s.i = f2u(2.0)
        prog = end([(f2u(5.0), U("ADDi", "x", fd=1, fs=0) | vu.I_BIT),
                    (NOPL, U("ADDi", "y", fd=1, fs=0))])
        r = H.run(prog, s)
        self.assertEqual(r.getf(1)[:2], [2.0, 5.0])


@unittest.skipUnless(H.find_cc(), "gcc/clang not found")
class XGKick(unittest.TestCase):
    def test_kick_after_next_instruction(self):
        s = H.State()
        s.setf(1, 1.0, 2.0, 3.0, 4.0)
        s.setf(2, 5.0, 6.0, 7.0, 8.0)
        s.vi[4] = 0x402                                              # & 0x3FF -> qword 2
        prog = end([
            (L("XGKICK", "", is_=4), NOPU),
            (L("SQ", fs=1, it=0, imm=2), NOPU),                      # included
            (L("SQ", fs=2, it=0, imm=3), NOPU),                      # after the transfer
        ])
        r = H.run(prog, s)
        self.assertEqual(len(r.kicks), 1)
        addr, data = r.kicks[0]
        self.assertEqual(addr, 0x20)
        self.assertEqual(struct.unpack_from("<4f", data, 0), (1.0, 2.0, 3.0, 4.0))
        self.assertEqual(struct.unpack_from("<4I", data, 16), (0, 0, 0, 0))

    def test_build_vu1_test_packet(self):
        """The micro program of programs_sys.build_vu1_test, with the VIF1 UNPACK data
        it uploads: one XGKICK of the sprite packet (FTOI4 vertices)."""
        NU, NLw = PS.VU_NOP_UPPER, PS.VU_NOP_LOWER
        prog = [
            (PS.vu_lower_lq(1, 3, 0), NU), (PS.vu_lower_lq(2, 5, 0), NU),
            (NLw, NU), (NLw, NU), (NLw, NU),
            (NLw, PS.vu_upper_vadd(3, 1, 2)), (NLw, PS.vu_upper_ftoi4(4, 1)),
            (NLw, PS.vu_upper_ftoi4(5, 3)),
            (NLw, NU), (NLw, NU), (NLw, NU),
            (PS.vu_lower_sq(4, 3, 0), NU), (PS.vu_lower_sq(5, 4, 0), NU),
            (PS.vu_lower_xgkick(0), NU), (NLw, NU | E), (NLw, NU)]
        r = H.run(prog, vu1_test_state())
        self.assertEqual((r.rc, r.tpc, len(r.kicks)), (0, 0x80, 1))
        self.assertEqual(r.kicks[0][1][:0x50], expected_vu1_test_packet())


def vu1_test_state():
    """VU1 data memory as build_vu1_test's UNPACK V4-32 x6 @0 leaves it."""
    from mips_asm import Asm  # noqa: F401  (keeps programs_sys importable)
    s = H.State()
    tag_lo, tag_hi = PS.gif_tag(1, 1, 0, 4, 0x5510)
    words = [tag_lo & 0xFFFFFFFF, tag_lo >> 32, tag_hi & 0xFFFFFFFF, tag_hi >> 32,
             6, 0, 0, 0, 0, 255, 0, 0x80,
             f2u(300.0), f2u(250.0), 0, 0, 0, 0, 0, 0, f2u(100.0), f2u(80.0), 0, 0]
    s.mem[:96] = struct.pack("<24I", *words)
    return s


def expected_vu1_test_packet():
    tag_lo, tag_hi = PS.gif_tag(1, 1, 0, 4, 0x5510)
    words = [tag_lo & 0xFFFFFFFF, tag_lo >> 32, tag_hi & 0xFFFFFFFF, tag_hi >> 32,
             6, 0, 0, 0, 0, 255, 0, 0x80,
             300 * 16, 250 * 16, 0, 0, 400 * 16, 330 * 16, 0, 0]
    return struct.pack("<20I", *words)


if __name__ == "__main__":
    unittest.main(verbosity=2)
