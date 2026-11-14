"""Build, recompile and run the test programs.

    python tests/run_tests.py [name ...]
"""

import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "recompiler"))

import programs  # noqa: E402
import programs_sys  # noqa: E402
from ps2recomp.__main__ import main as recomp  # noqa: E402

BUILD = ROOT / "build" / "tests"


def run_exe(exe, args=(), timeout=60):
    t = time.time()
    p = subprocess.run([str(exe), "--headless", *args], capture_output=True, text=True,
                       timeout=timeout, cwd=exe.parent)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def check_output(name, code, out, err, expect_code=0, expect_text="ALL PASS"):
    ok = code == expect_code and expect_text in out
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name} (exit {code})")
    if not ok:
        print("  stdout:\n    " + out.replace("\n", "\n    "))
        print("  stderr:\n    " + err.replace("\n", "\n    "))
    return ok


BACKEND = "soft"


def build_and_run(name, builder, run_args=(), expect_code=0, expect_text="ALL PASS",
                  recomp_args=(), post=None):
    BUILD.mkdir(parents=True, exist_ok=True)
    elf = BUILD / f"{name}.elf"
    builder(str(elf))
    out = BUILD / BACKEND / name
    rc = recomp([str(elf), "-o", str(out), "--backend", BACKEND, *recomp_args])
    if rc != 0:
        print(f"[FAIL] {name}: recompiler exit {rc}")
        return False
    exe = (out / "bin" / f"{name}.exe") if BACKEND == "pcsx2" else (out / f"{name}.exe")
    code, so, se, dt = run_exe(exe, run_args)
    ok = check_output(name, code, so, se, expect_code, expect_text)
    if ok and post:
        ok = post(out, so)
    return ok


TESTS = {}


def test(fn):
    TESTS[fn.__name__.replace("test_", "")] = fn
    return fn


@test
def test_cpu():
    return build_and_run("cpu", lambda p: programs.build_cpu_test(p, symbols=True))


@test
def test_fake_ra():
    """S0-B1: jr ra to a fake in-function return address (SH3 quicksort pattern)."""
    return build_and_run("fake_ra", programs_sys.build_fake_ra_test)


@test
def test_cpu_stripped():
    # Same program without a symbol table: functions must be discovered from
    # calls, data pointers and code constants only.
    return build_and_run("cpu_stripped", lambda p: programs.build_cpu_test(p, symbols=False))


@test
def test_negative():
    # The harness itself must catch failing checks.
    return build_and_run("negative", programs_sys.build_negative_test,
                         expect_code=1, expect_text="FAIL deliberate")


@test
def test_threads():
    return build_and_run("threads", programs_sys.build_thread_test, expect_text="ABCDEFGHI\n")


@test
def test_interrupts():
    return build_and_run("interrupts", programs_sys.build_intr_test)


def read_image(path):
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size

    def px(x, y):
        return img.getpixel((x, y))
    return w, h, px


def check_gs_frame(out, _stdout):
    shot = out / "bin" / "frame.png" if BACKEND == "pcsx2" else out / "frame.bmp"
    w, h, raw_px = read_image(shot)
    # PCSX2 screenshots may be scaled; sample in 640x448 coordinates.
    sx, sy = w / 640.0, h / 448.0

    def px(x, y):
        return raw_px(min(w - 1, int((x + 0.5) * sx)), min(h - 1, int((y + 0.5) * sy)))
    ok = True

    def expect(x, y, rgb, tol=2, what=""):
        nonlocal ok
        got = px(x, y)
        good = all(abs(g - e) <= tol for g, e in zip(got, rgb))
        if not good:
            print(f"  pixel ({x},{y}) {what}: got {got}, expected {rgb}")
            ok = False

    # 640x448 native, or 4:3 aspect-corrected (PCSX2 screenshots).
    if not ((w, h) == (640, 448) or abs(w * 3 - h * 4) <= 4):
        print(f"  display size {w}x{h}, expected 640x448 or 4:3")
        return False
    expect(10, 10, (0x20, 0x20, 0x80), what="background")
    if (w, h) == (640, 448):
        # Exact edge rules are only checkable without scaling.
        expect(100, 100, (255, 0, 0), what="sprite top-left (inclusive)")
        expect(200, 200, (0x20, 0x20, 0x80), what="sprite bottom-right (exclusive)")
    expect(150, 150, (255, 0, 0), what="sprite")
    expect(400, 167, (85, 85, 85), tol=8, what="gouraud centroid")
    expect(302, 101, (250, 3, 1), tol=12, what="gouraud red corner")
    expect(54, 304, (0, 0, 0), what="texel (0,0) black")
    expect(62, 304, (255, 255, 255), what="texel (1,0) white")
    expect(62, 312, (0, 0, 0), what="texel (1,1) black")
    expect(550, 350, (143, 143, 64), what="alpha blend")
    print(f"  [{'PASS' if ok else 'FAIL'}] gs frame pixel checks")
    return ok


@test
def test_gs():
    if BACKEND == "pcsx2":
        args = ("--frames", "30", "--renderer", "dx11", "--dump-frame", "frame.png")
    else:
        args = ("--frames", "6", "--dump-frame", "frame.bmp")
    return build_and_run("gs", programs_sys.build_gs_test, expect_text="", run_args=args,
                         post=check_gs_frame)


def check_vu1_frame(out, _stdout):
    shot = out / "bin" / "frame.png" if BACKEND == "pcsx2" else out / "frame.bmp"
    w, h, raw_px = read_image(shot)
    sx, sy = w / 640.0, h / 448.0
    ok = True
    for (x, y, rgb, what) in ((350, 290, (0, 255, 0), "VU1-computed sprite"),
                              (310, 260, (0, 255, 0), "VU1 sprite near corner"),
                              (420, 345, (0x20, 0x20, 0x80), "outside sprite"),
                              (50, 50, (0x20, 0x20, 0x80), "background")):
        got = raw_px(min(w - 1, int((x + 0.5) * sx)), min(h - 1, int((y + 0.5) * sy)))
        if not all(abs(g - e) <= 3 for g, e in zip(got, rgb)):
            print(f"  pixel ({x},{y}) {what}: got {got}, expected {rgb}")
            ok = False
    print(f"  [{'PASS' if ok else 'FAIL'}] vu1 frame pixel checks")
    return ok


@test
def test_vu1():
    if BACKEND != "pcsx2":
        print("[SKIP] vu1 (needs the PCSX2 backend)")
        return True
    return build_and_run("vu1", programs_sys.build_vu1_test, expect_text="",
                         run_args=("--frames", "30", "--renderer", "dx11", "--dump-frame", "frame.png"),
                         post=check_vu1_frame)


@test
def test_vu0_micro():
    if BACKEND != "pcsx2":
        print("[SKIP] vu0_micro (needs the PCSX2 backend)")
        return True
    return build_and_run("vu0", programs_sys.build_vu0_test)


def main(argv):
    global BACKEND
    if argv and argv[0].startswith("--backend="):
        BACKEND = argv[0].split("=", 1)[1]
        argv = argv[1:]
    print(f"backend: {BACKEND}")
    names = argv or list(TESTS)
    results = {n: TESTS[n]() for n in names}
    failed = [n for n, ok in results.items() if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
