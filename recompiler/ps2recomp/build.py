"""Compile generated sources + runtime into a native Windows executable."""

import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .paths import project_root, NO_WINDOW

ROOT = project_root()
RUNTIME = ROOT / "runtime"

CFLAGS = ["-std=gnu11", "-fno-strict-aliasing", "-fwrapv", "-msse2",
          "-Wall", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-function",
          "-Wno-format-truncation"]
LIBS = ["-lwinmm", "-lgdi32", "-luser32", "-Wl,--stack,67108864"]


class BuildError(Exception):
    pass


def find_gcc():
    gcc = shutil.which("gcc")
    if gcc:
        return gcc
    for cand in (r"C:\msys64\ucrt64\bin\gcc.exe", r"C:\msys64\mingw64\bin\gcc.exe"):
        if os.path.exists(cand):
            return cand
    raise BuildError("gcc not found (install MSYS2 ucrt64 gcc or put gcc on PATH)")


def _needs_build(src, obj, deps):
    if not obj.exists():
        return True
    t = obj.stat().st_mtime
    if src.stat().st_mtime > t:
        return True
    return any(d.stat().st_mtime > t for d in deps)


def build(outdir, gen_files, exe_name, opt="-O2", jobs=None, log=print):
    gcc = find_gcc()
    env = dict(os.environ)
    # gcc needs its own bin dir on PATH for cc1/as/ld helpers.
    env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
    outdir = Path(outdir)
    objdir = outdir / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    inc = ["-I", str(RUNTIME / "include"), "-I", str(RUNTIME / "src"), "-I", str(outdir / "gen")]
    headers = list((RUNTIME / "include").glob("*.h")) + list((RUNTIME / "src").rglob("*.h"))
    gen_header = outdir / "gen" / "funcs.h"

    units = []
    for sub in ("core", "soft"):
        for src in sorted((RUNTIME / "src" / sub).glob("*.c")):
            units.append((src, objdir / f"rt_{src.stem}.o", headers, "-O2"))
    for src in gen_files:
        units.append((Path(src), objdir / f"{Path(src).stem}.o", headers + [gen_header], opt))

    def compile_one(unit):
        src, obj, deps, o = unit
        if not _needs_build(src, obj, deps):
            return None
        cmd = [gcc, o, *CFLAGS, *inc, "-c", str(src), "-o", str(obj)]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, creationflags=NO_WINDOW)
        if p.returncode != 0:
            return f"{src.name}:\n{p.stderr}"
        if p.stderr.strip():
            log(p.stderr.strip())
        return None

    todo = [u for u in units if _needs_build(u[0], u[1], u[2])]
    log(f"compiling {len(todo)} of {len(units)} files with {Path(gcc).name} {opt}...")
    with ThreadPoolExecutor(max_workers=jobs or os.cpu_count() or 4) as ex:
        errors = [e for e in ex.map(compile_one, units) if e]
    if errors:
        raise BuildError("compilation failed:\n" + "\n".join(errors))

    exe = outdir / exe_name
    cmd = [gcc, "-o", str(exe), *[str(u[1]) for u in units], *LIBS]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, creationflags=NO_WINDOW)
    if p.returncode != 0:
        raise BuildError("link failed:\n" + p.stderr)
    return exe
