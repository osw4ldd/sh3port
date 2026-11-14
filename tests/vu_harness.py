"""Host harness for VU programs translated by ps2recomp.vu (S3-M1 tests).

Builds the generated C with gcc (or clang) into a scratch folder (default
build/vu_scratch, override with VU_TEST_TMP) together with the reference VUState header
from vu.py, runs it on an initial state and returns the final state.  No game exe,
no PCSX2, no MSVC build is involved.
"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "recompiler"))

from ps2recomp import vu  # noqa: E402

SCRATCH = Path(os.environ.get("VU_TEST_TMP", ROOT / "build" / "vu_scratch"))

MAIN_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "ps2_vu1.h"

extern const vu_prog_entry vu_prog_table[];
static uint8_t mem[0x4000];
static FILE *kf;

static void kick(VUState *st, uint32_t addr)
{
    /* log the address and the 1 KiB that follows (wrapping) */
    fwrite(&addr, 4, 1, kf);
    for (uint32_t i = 0; i < 1024; i++)
        fputc(st->mem[(addr + i) & 0x3FFF], kf);
}

int main(int argc, char **argv)
{
    VUState st;
    uint32_t pc, n;
    FILE *f = fopen(argv[1], "rb");
    memset(&st, 0, sizeof(st));
    n = (uint32_t)fread(st.vf, 16, 32, f);
    n += (uint32_t)fread(&st.acc, 16, 1, f);
    n += (uint32_t)fread(st.vi, 2, 16, f);
    n += (uint32_t)fread(&st.status, 4, 1, f);
    n += (uint32_t)fread(&st.mac, 4, 1, f);
    n += (uint32_t)fread(&st.clip, 4, 1, f);
    n += (uint32_t)fread(&st.r, 4, 1, f);
    n += (uint32_t)fread(&st.i, 4, 1, f);
    n += (uint32_t)fread(&st.q, 4, 1, f);
    n += (uint32_t)fread(&st.p, 4, 1, f);
    n += (uint32_t)fread(&st.fbrst, 4, 1, f);
    n += (uint32_t)fread(&st.vif_top, 4, 1, f);
    n += (uint32_t)fread(&st.vif_itop, 4, 1, f);
    n += (uint32_t)fread(&pc, 4, 1, f);
    n += (uint32_t)fread(mem, 1, sizeof(mem), f);
    fclose(f);
    (void)n;
    st.mem = mem;
    st.xgkick = kick;
    kf = fopen(argv[3], "wb");
    _mm_setcsr(0xFFC0);                 /* FTZ | DAZ | round toward zero */
    int rc = vu_prog_table[0].fn(&st, pc);
    _mm_setcsr(0x1F80);
    fclose(kf);
    f = fopen(argv[2], "wb");
    fwrite(&rc, 4, 1, f);
    fwrite(&st.tpc, 4, 1, f);
    uint32_t cyc = (uint32_t)st.cycles;
    fwrite(&cyc, 4, 1, f);
    fwrite(st.vf, 16, 32, f);
    fwrite(&st.acc, 16, 1, f);
    fwrite(st.vi, 2, 16, f);
    fwrite(&st.status, 4, 1, f);
    fwrite(&st.mac, 4, 1, f);
    fwrite(&st.clip, 4, 1, f);
    fwrite(&st.r, 4, 1, f);
    fwrite(&st.i, 4, 1, f);
    fwrite(&st.q, 4, 1, f);
    fwrite(&st.p, 4, 1, f);
    fwrite(mem, 1, sizeof(mem), f);
    fclose(f);
    return 0;
}
"""


def find_cc():
    for cc in ("gcc", "clang", r"C:\msys64\ucrt64\bin\gcc.exe", r"C:\Program Files\LLVM\bin\clang.exe"):
        p = shutil.which(cc) or (cc if os.path.isfile(cc) else None)
        if p:
            return p
    return None


class State:
    """Mutable VU state for tests.  vf[r] = [x, y, z, w] as u32 bits."""

    def __init__(self):
        self.vf = [[0, 0, 0, 0] for _ in range(32)]
        self.vf[0] = [0, 0, 0, vu.f2u(1.0)]
        self.acc = [0, 0, 0, 0]
        self.vi = [0] * 16
        self.status = self.mac = self.clip = 0
        self.r = 0x3F800000
        self.i = self.q = self.p = 0
        self.fbrst = 0
        self.top = self.itop = 0
        self.mem = bytearray(0x4000)
        # outputs
        self.rc = self.tpc = self.cycles = None
        self.kicks = []

    def setf(self, r, *vals):
        self.vf[r] = [vu.f2u(v) for v in vals]

    def getf(self, r):
        return [vu.u2f(u) for u in self.vf[r]]

    def pack(self, pc):
        b = bytearray()
        for r in self.vf:
            b += struct.pack("<4I", *r)
        b += struct.pack("<4I", *self.acc)
        b += struct.pack("<16H", *self.vi)
        b += struct.pack("<11I", self.status, self.mac, self.clip, self.r, self.i, self.q,
                         self.p, self.fbrst, self.top, self.itop, pc)
        b += self.mem
        return bytes(b)

    def unpack(self, b):
        o = 0
        self.rc, self.tpc, self.cycles = struct.unpack_from("<3I", b, o)
        o += 12
        for r in range(32):
            self.vf[r] = list(struct.unpack_from("<4I", b, o))
            o += 16
        self.acc = list(struct.unpack_from("<4I", b, o))
        o += 16
        self.vi = list(struct.unpack_from("<16H", b, o))
        o += 32
        (self.status, self.mac, self.clip, self.r, self.i, self.q,
         self.p) = struct.unpack_from("<7I", b, o)
        o += 28
        self.mem = bytearray(b[o:o + 0x4000])


def build(pairs_or_code, entry=0, clamp_mode=2, vu_index=1, extra_entries=()):
    """Translate and compile; returns (exe path, VUProgram, C source)."""
    cc = find_cc()
    if cc is None:
        raise RuntimeError("no C compiler (gcc/clang) found")
    if isinstance(pairs_or_code, (bytes, bytearray)):
        code = bytes(pairs_or_code)
    else:
        code = vu.assemble(pairs_or_code, 0x4000 if vu_index == 1 else 0x1000)
    prog, src = vu.translate(code, entry, vu_index, clamp_mode, extra_entries)
    files = {
        "ps2_vu1.h": vu.REFERENCE_STATE_HEADER,
        "ps2_vu_ops.h": vu.emit_ops_header(clamp_mode),
        "prog.c": vu.emit_program_set([(prog, src)], clamp_mode),
        "main.c": MAIN_C,
    }
    key = hashlib.sha1("".join(files[k] for k in sorted(files)).encode()).hexdigest()[:16]
    d = SCRATCH / key
    exe = d / "t.exe"
    if not exe.exists():
        d.mkdir(parents=True, exist_ok=True)
        for k, v in files.items():
            (d / k).write_text(v)
        cmd = [cc, "-std=c11", "-O2", "-Wall", "-Wno-unused-label", "-Wno-unused-variable",
               "-ffp-contract=off", "-frounding-math", "-I", str(d),
               str(d / "prog.c"), str(d / "main.c"), "-o", str(exe), "-lm"]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError("compile failed:\n" + p.stderr[:4000])
        (d / "cc.log").write_text(p.stderr)
    return exe, prog, src


def run(pairs_or_code, state=None, entry=0, pc=None, clamp_mode=2, vu_index=1,
        extra_entries=()):
    """Run a program natively; returns the final State (with rc/tpc/cycles/kicks)."""
    exe, prog, src = build(pairs_or_code, entry, clamp_mode, vu_index, extra_entries)
    import copy
    st = copy.deepcopy(state) if state is not None else State()
    d = exe.parent
    tag = hashlib.sha1(st.pack(entry if pc is None else pc)).hexdigest()[:12]
    fin, fout, fk = d / f"in_{tag}.bin", d / f"out_{tag}.bin", d / f"kick_{tag}.bin"
    fin.write_bytes(st.pack(entry if pc is None else pc))
    p = subprocess.run([str(exe), str(fin), str(fout), str(fk)], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"program exe failed ({p.returncode}): {p.stderr}")
    st.unpack(fout.read_bytes())
    kb = fk.read_bytes()
    st.kicks = []
    for o in range(0, len(kb), 1028):
        addr = struct.unpack_from("<I", kb, o)[0]
        st.kicks.append((addr, kb[o + 4:o + 1028]))
    st.src = src
    st.prog = prog
    return st
