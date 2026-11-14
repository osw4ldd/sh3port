"""Pure-Python tests of the VU micro-code decoder/encoder and program discovery
(ps2recomp.vu, S3-M1).  Run:  python tests/test_vu_decode.py
No compiler, game exe or PCSX2 needed."""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "recompiler"))

from ps2recomp import vu  # noqa: E402
import programs_sys as P  # noqa: E402

NOPU = vu.encode_upper("NOP")
NOPL = 0x8000033C


class DecodeTables(unittest.TestCase):
    def test_upper_roundtrip_all(self):
        self.assertEqual(len(vu.UPPER_NAMES), 95)   # 48 main + 47 FD-table entries
        for n in vu.UPPER_NAMES:
            w = vu.encode_upper(n, "xz", fd=3, fs=7, ft=11)
            op = vu.decode_upper(w)
            self.assertEqual(op.name, n, f"{n}: {w:08X}")
            self.assertEqual(op.dest, 0b1010)
            self.assertEqual((op.fs, op.ft), (7, 11))

    def test_lower_roundtrip_all(self):
        for n in vu.LOWER_NAMES:
            w = vu.encode_lower(n, "y", ft=5, fs=9)
            self.assertEqual(vu.decode_lower(w).name, n, f"{n}: {w:08X}")

    def test_upper_table_is_complete(self):
        """Exhaustive scan of the upper encoding space: only the 95 mnemonics of
        microVU_Tables.inl decode, everything else is '???'."""
        found = set()
        for low in range(64):
            for fd in range(32):
                op = vu.decode_upper(low | (fd << 6))
                if op.known:
                    found.add(op.name)
        self.assertEqual(found, set(vu.UPPER_NAMES))

    def test_lower_table_is_complete(self):
        found = set()
        for top in range(128):
            for low in range(64):
                for mid in range(32):
                    op = vu.decode_lower((top << 25) | (mid << 6) | low)
                    if op.known:
                        found.add(op.name)
        self.assertEqual(found, set(vu.LOWER_NAMES))
        self.assertEqual(len(found), 69)

    def test_known_words_from_vu1_test(self):
        """The hand-assembled words in programs_sys.build_vu1_test / build_vu0_test."""
        self.assertEqual(vu.decode_upper(P.VU_NOP_UPPER).name, "NOP")
        self.assertEqual(vu.decode_lower(P.VU_NOP_LOWER).name, "MOVE")   # MOVE vf0,vf0 = NOP
        a = vu.decode_upper(P.vu_upper_vadd(3, 1, 2))
        self.assertEqual((a.name, a.fd, a.fs, a.ft, a.dest), ("ADD", 3, 1, 2, 15))
        f = vu.decode_upper(P.vu_upper_ftoi4(4, 1))
        self.assertEqual((f.name, f.ft, f.fs), ("FTOI4", 4, 1))
        lq = vu.decode_lower(P.vu_lower_lq(1, 3, 0))
        self.assertEqual((lq.name, lq.ft, lq.imm11, lq.is_), ("LQ", 1, 3, 0))
        sq = vu.decode_lower(P.vu_lower_sq(5, 4, 0))
        self.assertEqual((sq.name, sq.fs, sq.imm11, sq.it), ("SQ", 5, 4, 0))
        k = vu.decode_lower(P.vu_lower_xgkick(7))
        self.assertEqual((k.name, k.is_), ("XGKICK", 7))
        ia = vu.decode_lower(P.vu_lower_iaddiu(1, 0, 0x7ABC))
        self.assertEqual((ia.name, ia.it, ia.is_, ia.imm15), ("IADDIU", 1, 0, 0x7ABC))
        # encoder agrees with the hand encodings
        self.assertEqual(vu.encode_upper("ADD", fd=3, fs=1, ft=2), P.vu_upper_vadd(3, 1, 2))
        self.assertEqual(vu.encode_upper("FTOI4", ft=4, fs=1), P.vu_upper_ftoi4(4, 1))
        self.assertEqual(vu.encode_lower("LQ", ft=1, is_=0, imm=3), P.vu_lower_lq(1, 3, 0))
        self.assertEqual(vu.encode_lower("SQ", fs=5, it=0, imm=4), P.vu_lower_sq(5, 4, 0))
        self.assertEqual(vu.encode_lower("XGKICK", "", is_=7), P.vu_lower_xgkick(7))
        self.assertEqual(vu.encode_lower("IADDIU", "", it=1, is_=0, imm=0x7ABC),
                         P.vu_lower_iaddiu(1, 0, 0x7ABC))

    def test_immediates(self):
        w = vu.encode_lower("IADDI", "", it=2, is_=3, imm=-5)
        self.assertEqual(vu.decode_lower(w).imm5, -5)
        w = vu.encode_lower("IBNE", "", it=1, is_=2, imm=-3)
        op = vu.decode_lower(w)
        self.assertEqual((op.name, op.imm11, op.it, op.is_), ("IBNE", -3, 1, 2))
        w = vu.encode_lower("FSAND", "", it=4, imm=0xFC3)
        self.assertEqual(vu.decode_lower(w).imm12, 0xFC3)
        w = vu.encode_lower("FCSET", imm=0xABCDEF)
        self.assertEqual(vu.decode_lower(w).imm24, 0xABCDEF)
        w = vu.encode_lower("DIV", fs=1, fsf=2, ft=3, ftf=1)
        op = vu.decode_lower(w)
        self.assertEqual((op.fs, op.fsf, op.ft, op.ftf), (1, 2, 3, 1))

    def test_bits_and_ibit(self):
        ins = vu.decode(vu.f2u(2.5), NOPU | vu.I_BIT | vu.E_BIT)
        self.assertIsNone(ins.lower)
        self.assertTrue(ins.ibit and ins.ebit and not ins.tbit)
        self.assertIn("LOI 2.5", vu.disasm(ins))

    def test_disasm(self):
        ins = vu.decode(vu.encode_lower("LQI", "xyz", ft=4, is_=2),
                        vu.encode_upper("MADDAw", "xyzw", fs=1, ft=2))
        self.assertEqual(vu.disasm(ins), "MADDAw.xyzw ACC, vf01, vf02w | LQI.xyz vf04, (vi02++)")


class Discovery(unittest.TestCase):
    def test_linear_ebit(self):
        code = vu.assemble([(NOPL, NOPU), (NOPL, NOPU | vu.E_BIT), (NOPL, NOPU), (NOPL, NOPU)])
        p = vu.discover(code, 0)
        self.assertEqual(sorted(p.insns), [0, 8, 16])
        self.assertEqual(p.delay, {16: 8})
        self.assertEqual(p.ranges(), [(0, 24)])

    def test_loop_and_labels(self):
        L = vu.encode_lower
        prog = [
            (L("IADDIU", "", it=1, is_=0, imm=3), NOPU),          # 00
            (L("IADDI", "", it=1, is_=1, imm=-1), NOPU),          # 08 loop
            (NOPL, NOPU),                                         # 10
            (L("IBNE", "", it=0, is_=1, imm=-3), NOPU),           # 18 -> 08
            (NOPL, NOPU),                                         # 20 delay
            (NOPL, NOPU | vu.E_BIT),                              # 28
            (NOPL, NOPU),                                         # 30
        ]
        p = vu.discover(vu.assemble(prog), 0)
        self.assertIn(8, p.labels)
        self.assertIn(0x28, p.labels)                             # not-taken continuation
        self.assertEqual(p.delay, {0x20: 0x18, 0x30: 0x28})
        self.assertEqual(len(p.insns), 7)

    def test_branch_in_delay_slot_unsupported(self):
        L = vu.encode_lower
        prog = [(L("B", "", imm=2), NOPU), (L("B", "", imm=2), NOPU)] + [(NOPL, NOPU | vu.E_BIT)] * 4
        with self.assertRaises(vu.VUUnsupported):
            vu.discover(vu.assemble(prog), 0)

    def test_unknown_op_unsupported(self):
        with self.assertRaises(vu.VUUnsupported):
            vu.discover(vu.assemble([(NOPL, 0x30), (NOPL, NOPU | vu.E_BIT)]), 0)

    def test_hash_depends_on_covered_code_only(self):
        a = bytearray(vu.assemble([(NOPL, NOPU | vu.E_BIT), (NOPL, NOPU)]))
        h1 = vu.discover(bytes(a), 0).hash()
        a[0x100] = 0x55                                           # outside the program
        self.assertEqual(vu.discover(bytes(a), 0).hash(), h1)
        a[0] ^= 1                                                 # inside
        self.assertNotEqual(vu.discover(bytes(a), 0).hash(), h1)

    def test_vu0_image(self):
        code = vu.assemble([(NOPL, NOPU | vu.E_BIT), (NOPL, NOPU)], size=0x1000)
        prog, src = vu.translate(code, 0, vu=0)
        self.assertTrue(prog.name.startswith("vu0_"))
        self.assertIn("VU_EXIT_EBIT", src)


class Stalls(unittest.TestCase):
    def test_fmac_stall(self):
        """VF read 1 instruction after an FMAC write stalls 3 cycles (analyzeReg1)."""
        p = vu._Pipe()
        a = vu.decode(NOPL, vu.encode_upper("ADD", fd=3, fs=1, ft=2))
        b = vu.decode(NOPL, vu.encode_upper("ADD", fd=4, fs=3, ft=3))
        self.assertEqual(p.step(a, 1), 0)
        self.assertEqual(p.step(b, 1), 3)

    def test_ilw_stall(self):
        p = vu._Pipe()
        a = vu.decode(vu.encode_lower("ILW", "x", it=2, is_=0, imm=0), NOPU)
        b = vu.decode(vu.encode_lower("IADD", "", id_=3, is_=2, it=2), NOPU)
        self.assertEqual(p.step(a, 1), 0)
        self.assertEqual(p.step(b, 1), 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
