"""Build a recompiled program against the PCSX2 core (VU1 + hardware GS).

Uses MSVC through scripts/configure_pcsx2.bat / build_pcsx2.bat and the
shared build tree build/pcsx2 (PCSX2 itself is compiled once; afterwards only
the program's generated code is compiled and linked).  The result is a
self-contained folder: <out>/bin/<name>.exe + DLLs + resources/.
"""

import os
import shutil
import subprocess
from pathlib import Path

from .paths import project_root, NO_WINDOW

ROOT = project_root()
PCSX2 = ROOT / "third_party" / "pcsx2"
SCRIPTS = ROOT / "scripts"


class Pcsx2BuildError(Exception):
    pass


def _run_bat(bat, *args, log=print):
    inner = f'"{bat}" ' + " ".join(f'"{a}"' if " " in a else a for a in args)
    # cmd /s /c "<line>": the outer quotes are stripped, inner quoting kept.
    p = subprocess.run(f'cmd /s /c "{inner}"', capture_output=True, text=True, errors="replace",
                       creationflags=NO_WINDOW)
    if p.returncode != 0:
        lines = (p.stdout + p.stderr).splitlines()
        errs = [ln for ln in lines if "error" in ln.lower() or "FAILED" in ln]
        tail = "\n".join((errs or lines)[-40:])
        raise Pcsx2BuildError(f"{Path(bat).name} failed:\n{tail}")
    return p.stdout


def _wanted_dll(name):
    n = name.lower()
    if n.startswith("qt6") or n.startswith("kddockwidgets"):
        return False
    stem = n[:-4]
    # Debug variants (Qt-style "...d.dll") are skipped when a release twin exists.
    return not (stem.endswith("d") and (PCSX2 / "deps" / "bin" / (name[:-5] + ".dll")).exists())


def package(bindir, log=print):
    bindir = Path(bindir)
    deps_bin = PCSX2 / "deps" / "bin"
    n = 0
    for dll in deps_bin.glob("*.dll"):
        if _wanted_dll(dll.name):
            dst = bindir / dll.name
            if not dst.exists() or dst.stat().st_mtime < dll.stat().st_mtime:
                shutil.copy2(dll, dst)
            n += 1
    for sub in ("D3D12",):
        src = deps_bin / sub
        if src.is_dir():
            shutil.copytree(src, bindir / sub, dirs_exist_ok=True)
    res_src = PCSX2 / "bin" / "resources"
    shutil.copytree(res_src, bindir / "resources", dirs_exist_ok=True)
    # Full FFmpeg (same library majors as PCSX2's, which has no decoders):
    # the native MPEG-2 decoding of FMVs (runtime/src/core/mpeg_native.c).
    ff = sorted((PCSX2.parent.parent / "tools" / "ffmpeg").glob("ffmpeg-*-shared*/bin"))
    if ff:
        for dll in ff[-1].glob("*.dll"):
            shutil.copy2(dll, bindir / dll.name)
        log(f"FFmpeg decoders from {ff[-1].parent.name}")
    else:
        log("warning: tools/ffmpeg not found - native FMV decoding will be unavailable")
    log(f"packaged {n} DLLs + resources into {bindir}")


def build_pcsx2(outdir, name, log=print):
    outdir = Path(outdir).resolve()
    if not (PCSX2 / "deps").is_dir():
        raise Pcsx2BuildError("third_party/pcsx2/deps is missing (PCSX2 Windows dependency package)")
    log("configuring PCSX2 host build...")
    _run_bat(SCRIPTS / "configure_pcsx2.bat",
             f"-DPS2NATIVE_HOST_DIR={ROOT / 'host'}",
             f"-DPS2NATIVE_GAME_DIR={outdir}",
             f"-DPS2NATIVE_GAME_NAME={name}", log=log)
    log("building with MSVC (first build compiles PCSX2 itself, this takes a while)...")
    _run_bat(SCRIPTS / "build_pcsx2.bat", "ps2native-game", log=log)
    exe = outdir / "bin" / f"{name}.exe"
    if not exe.exists():
        raise Pcsx2BuildError(f"expected output {exe} was not produced")
    package(exe.parent, log=log)
    return exe
