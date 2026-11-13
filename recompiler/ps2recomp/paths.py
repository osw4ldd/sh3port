"""Locations of the ps2native tree (works from source and from a frozen exe)."""

import subprocess
import sys
from pathlib import Path


def project_root() -> Path:
    if getattr(sys, "frozen", False):
        # Packaged ps2native.exe / ps2recomp.exe live in the project root.
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[2]


# Child processes must not open console windows from the GUI build.
NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
