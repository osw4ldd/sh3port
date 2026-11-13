"""Package the Silent Hill 3 native port as a release folder (and zip):

    release/SilentHill3/
        Silent Hill 3.exe      launcher (pick disc + BIOS, settings, play)
        bin/                   SLUS_206_22.exe + PCSX2 core DLLs/resources
        LICENSE.txt, OKUBENI.txt

No game data or BIOS is included; the user selects their own in the launcher.

Usage:  python scripts/package_sh3.py [--zip]
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_BIN = ROOT / "build" / "games" / "sh3" / "bin"
OUT = ROOT / "release" / "SilentHill3"
WORK = ROOT / "build" / "pyinstaller-sh3"

# Development leftovers that must not ship.
SKIP_SUFFIXES = {".pdb", ".ilk", ".exp", ".lib", ".log", ".tmp", ".png", ".bmp", ".dmp"}
SKIP_NAMES = {"memcards", "logs", "ps2native_missing.txt", "inis", "cache", "snaps", "sstates"}

# Settings shipped with the release (paths are chosen in the launcher).
DEFAULT_INI = """# Silent Hill 3 native port - settings (edited by "Silent Hill 3.exe")
boot=cdrom0:\\SLUS_206.22;1
renderer=auto
upscale=3
widescreen=1
fullscreen=0
anisotropy=16
fxaa=0
sharpen=0
vsync=0
gpu=high
fast_cdvd=1
fps60=0
camera_behind=0
camera_l3=0
controller=1
vibration=1
"""

README = """Silent Hill 3 - Native PC Port
==============================

1. "Silent Hill 3.exe"yi çalıştırın.
2. "Oyun dosyaları" sayfasında kendi Silent Hill 3 (ABD, SLUS-20622) disk
   imajınızı (.iso) ve kendi PS2 konsolunuzdan aldığınız BIOS'u seçin.
3. Grafik / Oyun ayarları / Kontroller sayfalarından ayarlarınızı yapın.
4. "OYNA".

Tam ekran: Alt+Enter veya F11.
Kayıtlar: bin\\memcards\\Mcd001.ps2 (launcher'dan yedeklenebilir).
Sorun olursa: bin\\logs\\last_run.log

Bu paket oyun dosyası veya BIOS içermez.
"""


def copy_game(dst):
    if not (GAME_BIN / "SLUS_206_22.exe").exists():
        sys.exit(f"game build not found: {GAME_BIN} (run the recompiler first)")
    for src in GAME_BIN.iterdir():
        if src.name in SKIP_NAMES or src.suffix.lower() in SKIP_SUFFIXES:
            continue
        if src.suffix.lower() == ".txt" or src.name == "ps2native.ini":
            continue
        target = dst / src.name
        if src.is_dir():
            shutil.copytree(src, target, dirs_exist_ok=True)
        else:
            shutil.copy2(src, target)
    (dst / "ps2native.ini").write_text(DEFAULT_INI, encoding="utf-8")


def build_launcher(dst):
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--windowed",
           "--name", "Silent Hill 3", "--paths", str(ROOT / "recompiler"),
           "--icon", str(ROOT / "launcher" / "assets" / "icon.ico"),
           "--add-data", f"{ROOT / 'launcher' / 'assets'}{os.pathsep}assets",
           "--distpath", str(dst), "--workpath", str(WORK), "--specpath", str(WORK),
           str(ROOT / "launcher" / "sh3_launcher.py")]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", action="store_true", help="also create release/SilentHill3.zip")
    args = ap.parse_args()
    # Never start deleting a release that is in use (locked exe -> half-deleted folder).
    running = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    busy = [n for n in ("Silent Hill 3.exe", "SLUS_206_22.exe") if f'"{n}"' in running]
    if busy:
        sys.exit(f"close {', '.join(busy)} first (the release folder is in use)")
    # Keep the user's saves and settings when rebuilding a release in place.
    keep = WORK / "keep"
    if keep.exists():
        shutil.rmtree(keep)
    keep.mkdir(parents=True)
    for name in ("memcards", "ps2native.ini"):
        src = OUT / "bin" / name
        if src.is_dir():
            shutil.copytree(src, keep / name)
        elif src.is_file():
            shutil.copy2(src, keep / name)
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "bin").mkdir(parents=True)
    copy_game(OUT / "bin")
    for item in keep.iterdir():
        if item.is_dir():
            shutil.copytree(item, OUT / "bin" / item.name, dirs_exist_ok=True)
        else:
            shutil.copy2(item, OUT / "bin" / item.name)
    build_launcher(OUT)
    shutil.copy2(ROOT / "LICENSE", OUT / "LICENSE.txt")
    (OUT / "OKUBENI.txt").write_text(README, encoding="utf-8")
    print(f"release: {OUT}")
    if args.zip:
        archive = shutil.make_archive(str(OUT), "zip", OUT.parent, OUT.name)
        print(f"zip: {archive}")


if __name__ == "__main__":
    main()
