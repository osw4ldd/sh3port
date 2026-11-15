"""VU0 macro-mode MAC/STATUS/FDIV flags and CTC2 vi16 (task S0-B4).

Compiles the runtime's VU0 macro helpers (runtime/include/ps2_vu0.h + ps2rt.h) and
the vu0_cfc2/vu0_ctc2 functions cut out of runtime/src/core/dispatch.c with gcc into
build/vu_scratch, runs a small C driver and checks the flags against PCSX2 microVU
macro mode (see ps2_vu0.h comments for the citations).  No game exe, no MSVC build.

Override the files under test (e.g. a patch prepared in a scratch folder):
    VU0_HEADER=path/to/ps2_vu0.h  VU0_DISPATCH=path/to/dispatch.c
Run:  python tests/test_vu0_flags.py
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from vu_harness import find_cc  # noqa: E402

INC = ROOT / "runtime" / "include"
HEADER = Path(os.environ.get("VU0_HEADER", INC / "ps2_vu0.h"))
DISPATCH = Path(os.environ.get("VU0_DISPATCH", ROOT / "runtime" / "src" / "core" / "dispatch.c"))
SCRATCH = Path(os.environ.get("VU_TEST_TMP", ROOT / "build" / "vu_scratch")) / "vu0_flags"


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def W(dest="xyzw", fd=0, fs=0, ft=0, fsf=None, ftf=None):
    """COP2 macro word with the fields the helpers read (opcode bits do not matter)."""
    d = 0
    for ch in dest:
        d |= 8 >> "xyzw".index(ch)
    if fsf is not None or ftf is not None:      # FDIV: bits 22..21 fsf, 24..23 ftf
        d = ((ftf or 0) << 2) | (fsf or 0)
    return (18 << 26) | (1 << 25) | (d << 21) | (ft << 16) | (fs << 11) | (fd << 6)


DRIVER = r"""
#include <stdio.h>
#include "ps2rt.h"
PS2_IMPORT uint8_t *ps2_vu0_mem;
uint8_t *ps2_vu0_mem;
void ps2_warn_once(const char *key, const char *fmt, ...) { (void)key; (void)fmt; }
void vu0_callms(R5900 *c, uint32_t addr) { (void)c; (void)addr; }
uint32_t vu0_cfc2(R5900 *c, int reg);
void vu0_ctc2(R5900 *c, int reg, uint32_t v);
static R5900 C;
static void setf(int r, float x, float y, float z, float w)
{ C.vf[r].F[0] = x; C.vf[r].F[1] = y; C.vf[r].F[2] = z; C.vf[r].F[3] = w; }
#define OUT(tag) printf("%s %08X %08X %08X %08X %08X %08X\n", tag, vu0_cfc2(&C, 16), vu0_cfc2(&C, 17), \
    C.vf[3].UW[0], C.vf[3].UW[1], C.vf[3].UW[2], C.vf[3].UW[3])
int main(void)
{
    setf(0, 0, 0, 0, 1);
    setf(1, 1, 2, 3, 4);
    setf(2, 2, 2, 1, 4);
    setf(5, -3, 0, 0, 0);
    VU0_VSUB(&C, @SUB@);            OUT("sub");      /* vf3 = (-1, 0, 2, 0) */
    VU0_VADD(&C, @ADDX@);           OUT("addx");     /* only x written, 2 */
    VU0_VMAX(&C, @MAX@);            OUT("max");      /* no flag change */
    VU0_VSUB(&C, @SUB0@);           OUT("subvf0");   /* vf0 dest: flags still set */
    vu0_ctc2(&C, 16, 0);            OUT("ctc0");     /* sticky cleared, Z/S kept */
    vu0_ctc2(&C, 16, 0xFFF);        OUT("ctcfff");   /* only sticky bits written */
    vu0_ctc2(&C, 16, 0);
    VU0_VADD(&C, @ADD@);            OUT("addpos");   /* all positive: Z/S 0 */
    VU0_VMULA(&C, @MULA@);          OUT("mula");     /* ACC = vf1*vf5 (x neg) */
    VU0_VOPMULA(&C, @OPMULA@);      OUT("opmula");   /* (2*1, 3*2, 1*2) */
    VU0_VOPMSUB(&C, @OPMSUB@);      OUT("opmsub");   /* cross vf1 x vf2 into vf3 */
    vu0_ctc2(&C, 16, 0);
    VU0_VDIV(&C, @DIV0@);           OUT("div0");     /* 1 / 0: D|DS */
    VU0_VDIV(&C, @DIV@);            OUT("div");      /* 1 / 2: I/D cleared, DS kept */
    VU0_VDIV(&C, @DIV00@);          OUT("div00");    /* 0 / 0: I|IS */
    VU0_VSQRT(&C, @SQRTN@);         OUT("sqrtneg");  /* sqrt(-3): I */
    VU0_VSQRT(&C, @SQRT@);          OUT("sqrt");     /* sqrt(4): clears I */
    VU0_VRSQRT(&C, @RSQRT0@);       OUT("rsqrt0");   /* 1/sqrt(0): D */
    { float q = C.vq; unsigned u; memcpy(&u, &q, 4); printf("q %08X\n", u); }
    return 0;
}
"""


def driver_source():
    subs = {
        "SUB": W("xyzw", fd=3, fs=1, ft=2),
        "ADDX": W("x", fd=4, fs=1, ft=1),
        "MAX": W("xyzw", fd=6, fs=5, ft=1),
        "SUB0": W("xyzw", fd=0, fs=1, ft=2),
        "ADD": W("xyzw", fd=3, fs=1, ft=2),
        "MULA": W("xyzw", fs=1, ft=5),
        "OPMULA": W("xyz", fs=1, ft=2),
        "OPMSUB": W("xyz", fd=3, fs=2, ft=1),
        "DIV0": W(fs=1, ft=0, fsf=0, ftf=0),
        "DIV": W(fs=1, ft=2, fsf=0, ftf=1),
        "DIV00": W(fs=0, ft=0, fsf=0, ftf=0),
        "SQRTN": W(ft=5, ftf=0),
        "SQRT": W(ft=1, ftf=3),
        "RSQRT0": W(fs=1, ft=0, fsf=0, ftf=0),
    }
    s = DRIVER
    for k, v in subs.items():
        s = s.replace(f"@{k}@", f"0x{v:08X}u")
    return s


def cut_cfc2_ctc2(path):
    text = Path(path).read_text()
    out = []
    for fn in ("vu0_cfc2", "vu0_ctc2"):
        m = re.search(r"^[a-z0-9_ ]+\b%s\(R5900 \*c.*?^}\n" % fn, text, re.S | re.M)
        if not m:
            raise RuntimeError(f"{fn} not found in {path}")
        out.append(m.group(0))
    return '#include "ps2rt.h"\nvoid ps2_warn_once(const char *key, const char *fmt, ...);\n' + "\n".join(out)


def run_driver():
    cc = find_cc()
    d = SCRATCH
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    for h in INC.glob("*.h"):
        shutil.copy(h, d / h.name)
    shutil.copy(HEADER, d / "ps2_vu0.h")
    (d / "main.c").write_text(driver_source())
    (d / "vu0_regs.c").write_text(cut_cfc2_ctc2(DISPATCH))
    exe = d / "t.exe"
    p = subprocess.run([cc, "-std=gnu11", "-O1", "-w", "-I", str(d), str(d / "main.c"),
                        str(d / "vu0_regs.c"), "-o", str(exe), "-lm"], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError("compile failed:\n" + p.stderr[:3000])
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    res = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        res[parts[0]] = [int(x, 16) for x in parts[1:]]
    return res


@unittest.skipUnless(find_cc(), "gcc/clang not found")
class VU0Flags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.r = run_driver()

    def st(self, tag):
        return self.r[tag][0], self.r[tag][1]

    def test_vsub_mac_status(self):
        st, mac = self.st("sub")
        self.assertEqual(self.r["sub"][2:], [f2u(-1.0), 0, f2u(2.0), 0])
        self.assertEqual(mac, 0x80 | 0x04 | 0x01)        # Sx, Zy, Zw
        self.assertEqual(st, 0x1 | 0x2 | 0x40 | 0x80)    # Z S ZS SS

    def test_unwritten_fields_cleared_sticky_kept(self):
        st, mac = self.st("addx")
        self.assertEqual(mac, 0)
        self.assertEqual(st, 0xC0)

    def test_max_no_flags(self):
        self.assertEqual(self.st("max"), self.st("addx"))

    def test_vf0_dest_sets_flags(self):
        self.assertEqual(self.st("subvf0"), (0xC3, 0x85))

    def test_ctc2_status_sticky_only(self):
        self.assertEqual(self.st("ctc0")[0], 0x03)
        self.assertEqual(self.st("ctcfff")[0], 0xFC3)

    def test_positive_add(self):
        self.assertEqual(self.st("addpos"), (0, 0))

    def test_acc_and_opmul(self):
        self.assertEqual(self.st("mula"), (0xC3, 0x80 | 0x07))   # x neg, y z w zero
        self.assertEqual(self.st("opmula")[1], 0)
        st, mac = self.st("opmsub")
        # ACC (2,6,2) - vf2.yzx*vf1.zxy (6,1,4) = (-4, 5, -2)
        self.assertEqual(self.r["opmsub"][2:5], [f2u(-4.0), f2u(5.0), f2u(-2.0)])
        self.assertEqual(mac, 0x80 | 0x20)
        self.assertEqual(st & 0xF, 0x2)

    def test_fdiv_flags_set_and_clear(self):
        self.assertEqual(self.st("div0")[0] & 0xC30, 0x820)
        self.assertEqual(self.st("div")[0] & 0xC30, 0x800)   # D cleared, DS sticky
        self.assertEqual(self.st("div00")[0] & 0xC30, 0xC10)
        self.assertEqual(self.st("sqrtneg")[0] & 0x30, 0x10)
        self.assertEqual(self.st("sqrt")[0] & 0x30, 0x00)
        self.assertEqual(self.st("rsqrt0")[0] & 0x30, 0x20)
        self.assertEqual(self.r["q"][0], 0x7F7FFFFF)


if __name__ == "__main__":
    unittest.main(verbosity=2)
