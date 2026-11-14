"""VU1 end-to-end check against PCSX2 (S3-M1).  TO BE RUN BY THE VERIFIER (needs the
run token, a PCSX2 backend build and the S3-T0 capture hook).

Steps for the verifier:
  1. Capture the real VU1 run of programs_sys.build_vu1_test:
       set PS2NATIVE_VU1_DUMP=<dir>
       python tests/run_tests.py --backend=pcsx2 vu1
  2. Compare every captured program call with the native translation:
       python tests/vu_e2e_pcsx2.py --dump <dir> [--clamp 2]
     Pass = same XGKICK packets (address + bytes), same VU1 data memory and same
     VF/VI/ACC/flags/Q/P after the program.  Float differences are reported per register.
  Without --dump the script runs the self-check (build_vu1_test program on the data the
  VIF1 UNPACK uploads, expected packet computed by hand); that part runs without PCSX2.

Proposed capture layout (one directory per MSCAL/MSCNT, written by S3-T0; see
build/analysis/s3-vu1/interface.md):
  micro.bin   16 KiB micro memory at the call
  mem_in.bin  16 KiB data memory before, mem_out.bin after
  regs_in.bin / regs_out.bin: vf[32] (16 B each), acc (16 B), vi[32] (u32 each, PCSX2
              REG_* order: 16 STATUS, 17 MAC, 18 CLIP, 20 R, 21 I, 22 Q, 23 P, 26 TPC)
  entry.txt   entry pc in bytes (hex)
  kicks.bin   sequence of {u32 addr, u32 size, size bytes} for every PATH1 XGKICK
"""

import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import vu_harness as H  # noqa: E402


def load_regs(path, st):
    b = Path(path).read_bytes()
    for r in range(32):
        st.vf[r] = list(struct.unpack_from("<4I", b, 16 * r))
    st.acc = list(struct.unpack_from("<4I", b, 512))
    vi = struct.unpack_from("<32I", b, 528)
    st.vi = [v & 0xFFFF for v in vi[:16]]
    st.status, st.mac, st.clip = vi[16] & 0xFFF, vi[17] & 0xFFFF, vi[18] & 0xFFFFFF
    st.r, st.i, st.q, st.p = vi[20], vi[21], vi[22], vi[23]
    return vi


def parse_kicks(path):
    out, b, o = [], Path(path).read_bytes() if Path(path).exists() else b"", 0
    while o + 8 <= len(b):
        addr, size = struct.unpack_from("<II", b, o)
        out.append((addr, b[o + 8:o + 8 + size]))
        o += 8 + size
    return out


def check_call(d, clamp):
    d = Path(d)
    micro = (d / "micro.bin").read_bytes()
    st = H.State()
    load_regs(d / "regs_in.bin", st)
    st.mem = bytearray((d / "mem_in.bin").read_bytes())
    entry = int((d / "entry.txt").read_text().strip(), 16)
    r = H.run(micro, st, entry=entry, clamp_mode=clamp)
    exp = H.State()
    load_regs(d / "regs_out.bin", exp)
    problems = []
    if bytes(r.mem) != (d / "mem_out.bin").read_bytes():
        problems.append("data memory differs")
    for k in range(32):
        if r.vf[k] != exp.vf[k]:
            problems.append(f"vf{k:02d} {r.getf(k)} != {exp.getf(k)}")
    if r.acc != exp.acc:
        problems.append("acc differs")
    if r.vi[1:] != exp.vi[1:]:
        problems.append(f"vi {r.vi} != {exp.vi}")
    for nm in ("status", "mac", "clip", "q", "p", "r"):
        if getattr(r, nm) != getattr(exp, nm):
            problems.append(f"{nm} {getattr(r, nm):#x} != {getattr(exp, nm):#x}")
    kicks = parse_kicks(d / "kicks.bin")
    if len(kicks) != len(r.kicks):
        problems.append(f"{len(r.kicks)} XGKICKs != {len(kicks)}")
    for (ea, eb), (na, nb) in zip(kicks, r.kicks):
        if ea != na or nb[:len(eb)] != eb:
            problems.append(f"XGKICK @{ea:#x} differs")
    return problems


def selfcheck():
    import test_vu_codegen as T
    import unittest
    suite = unittest.defaultTestLoader.loadTestsFromName("XGKick.test_build_vu1_test_packet", T)
    return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", help="S3-T0 capture directory (one subdirectory per call)")
    ap.add_argument("--clamp", type=int, default=2)
    a = ap.parse_args()
    if not a.dump:
        return 0 if selfcheck() else 1
    bad = 0
    calls = sorted(p for p in Path(a.dump).iterdir() if (p / "micro.bin").exists())
    for c in calls:
        probs = check_call(c, a.clamp)
        print(f"[{'PASS' if not probs else 'FAIL'}] {c.name}")
        for p in probs[:20]:
            print("    " + p)
        bad += bool(probs)
    print(f"{len(calls) - bad}/{len(calls)} calls match")
    return 1 if bad or not calls else 0


if __name__ == "__main__":
    raise SystemExit(main())
