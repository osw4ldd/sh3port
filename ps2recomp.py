"""Convenience entry point: python ps2recomp.py GAME.ELF -o OUTDIR"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "recompiler"))

from ps2recomp.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
