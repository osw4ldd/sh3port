"""Codegen-level check for S0-B1 (fake return address, SH3 quicksorts 0x139CC0/0x139EB0).

Assembles programs_sys.build_fake_ra_test, runs the analysis + C generation in memory
(nothing is built or executed) and checks how `jr ra` is lowered:
  * fsort writes ra with lui/addiu (not a call)  -> `jr ra` dispatches on ra:
    goto L_<fsort_pop> for the in-function address, return for entry_ra, else ps2_jump;
  * t_fail / functions that only write ra by calls or restore it from sp keep `return;`.
Before the fix every `jr ra` was `return;` and the first check fails.
Run:  python tests/test_codegen_fake_ra.py
The executable version is `python tests/run_tests.py fake_ra` (verifier, run token).
"""

import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "recompiler"))

import programs_sys  # noqa: E402
from ps2recomp.elf import load_elf  # noqa: E402
from ps2recomp.analysis import Program  # noqa: E402
from ps2recomp.codegen import FunctionEmitter, fname, lbl  # noqa: E402


def generate():
    d = Path(os.environ.get("VU_TEST_TMP", ROOT / "build" / "vu_scratch"))
    d.mkdir(parents=True, exist_ok=True)
    elf_path = d / "fake_ra.elf"
    programs_sys.build_fake_ra_test(str(elf_path))
    elf = load_elf(str(elf_path))
    prog = Program(elf, log=lambda *a: None).analyze()
    funcs = dict(sorted(prog.functions.items()))
    code = {a: "\n".join(FunctionEmitter(prog, f, funcs, {"branch_toggles": {}}).emit())
            for a, f in funcs.items()}
    return prog, funcs, code


class FakeRa(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.prog, cls.funcs, cls.code = generate()

    def _func_with(self, pattern):
        for a, src in self.code.items():
            if re.search(pattern, src):
                return a, src
        self.fail(f"no function matches {pattern!r}")

    def test_fsort_jr_ra_dispatches(self):
        a, src = self._func_with(r"/\* fsort \*/")
        f = self.funcs[a]
        jr_ra = [x for x in f.insns if self.prog.insn(x).name == "jr" and self.prog.insn(x).rs == 31]
        self.assertEqual(len(jr_ra), 2)
        pop = None
        for x in sorted(f.insns):
            ins = self.prog.insn(x)
            if ins.name == "addiu" and ins.rt == 31:
                v = self.prog._resolve_const(31, x + 4)
                pop = v
        self.assertIsNotNone(pop)
        self.assertIn(pop, f.insns)
        self.assertIn("const uint32_t entry_ra = c->r[31].UW[0];", src)
        # once per jr ra (+ once in the secondary-entry switch, fsort_pop is also a
        # dispatcher target because its address is built with lui/addiu)
        body = src.split(f"{lbl(f.entry)}:", 1)[1]
        self.assertEqual(body.count(f"case 0x{pop:X}u: goto {lbl(pop)};"), 2)
        self.assertEqual(src.count("if (t == entry_ra) return;"), 2)
        # no blind return left for jr ra
        for x in jr_ra:
            block = src.split(f"{lbl(x)}:", 1)[1].split("\nL_", 1)[0]
            self.assertNotRegex(block, r"^\s*return;\s*$", f"jr ra at {x:08X} still a plain return")

    def test_plain_functions_keep_return(self):
        # t_fail ends with "jr ra; addiu s7,s7,1" and never writes ra: plain return
        a, src = self._func_with(r"c->r\[23\]\.SD\[0\] = \(int32_t\)\(c->r\[23\]\.UW\[0\] \+ 0x1u\);\n    return;")
        self.assertNotIn("entry_ra", src)


if __name__ == "__main__":
    unittest.main(verbosity=2)
