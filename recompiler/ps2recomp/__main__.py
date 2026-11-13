"""ps2recomp: statically recompile a PS2 (EE) ELF into a native x86-64 program.

Usage:
    python -m ps2recomp GAME.ELF -o OUTDIR [options]
"""

import argparse
import json
import sys
import time
from pathlib import Path

from .elf import load_elf, ElfError
from .analysis import Program
from .project import write_project
from .build import build, BuildError
from .build_pcsx2 import build_pcsx2, Pcsx2BuildError
from .iso import Iso, IsoError
from .paths import project_root
from . import pnach as pnach_mod


def parse_addr(v):
    if isinstance(v, int):
        return v
    return int(str(v), 0)


def load_hints(path):
    """Function start hints from an external analysis (REA, Ghidra, IDA...).

    Accepts a JSON list of addresses, or objects with an address field
    (address/addr/start/entry), or {"functions": [...]}."""
    with open(path) as f:
        data = json.load(f)
    if isinstance(data, dict):
        data = data.get("functions") or data.get("procedures") or data.get("entries") or []
    out = set()
    for item in data:
        if isinstance(item, dict):
            for k in ("address", "addr", "start", "entry", "entrypoint"):
                if k in item:
                    out.add(parse_addr(item[k]))
                    break
        else:
            out.add(parse_addr(item))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="ps2recomp", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", help="PS2 EE executable (SLUS_123.45, game.elf) or a disc image (.iso)")
    ap.add_argument("-o", "--out", required=True, help="output directory")
    ap.add_argument("--name", help="program name (default: ELF file name)")
    ap.add_argument("--entry", action="append", default=[], help="extra function entry (hex)")
    ap.add_argument("--skip", action="append", default=[], help="never treat as function")
    ap.add_argument("--config", help="JSON game config (entries, skip, name, pnach, pnach_sections, settings)")
    ap.add_argument("--pnach", help="PCSX2 .pnach patch file (widescreen, 60 FPS, ...)")
    ap.add_argument("--pnach-sections", help="comma-separated [section] names of the pnach to apply")
    ap.add_argument("--hints", help="JSON function list from an external analyser (e.g. REA)")
    ap.add_argument("--no-build", action="store_true", help="only generate C sources")
    ap.add_argument("--backend", choices=("pcsx2", "soft"), default="pcsx2",
                    help="pcsx2: PCSX2 core (VU1, Vulkan/DX11/DX12/OpenGL GS), MSVC build; "
                         "soft: built-in software GS, gcc build, no VU1")
    ap.add_argument("--bios", help="PS2 BIOS file/folder for the real IOP (default: <ps2native>/bios)")
    ap.add_argument("--opt", default="-O2", help="gcc optimisation for generated code")
    ap.add_argument("-j", "--jobs", type=int, help="parallel compile jobs")
    args = ap.parse_args(argv)

    t0 = time.time()
    iso_path = boot_path = None
    if is_iso(args.elf):
        # Disc image: compile the executable named in SYSTEM.CNF.
        try:
            iso = Iso(args.elf)
            boot_path = iso.boot_path()
            data = iso.read_file(boot_path)
            iso.close()
        except (OSError, IsoError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        iso_path = str(Path(args.elf).resolve())
        boot_name = boot_path.replace("\\", "/").split("/")[-1].split(";")[0]
        Path(args.out).mkdir(parents=True, exist_ok=True)
        elf_file = Path(args.out) / boot_name
        elf_file.write_bytes(data)
        print(f"disc {iso_path}: boot {boot_path} ({len(data)} bytes)")
        args.elf = str(elf_file)
        if not args.name:
            args.name = boot_name
    try:
        elf = load_elf(args.elf)
    except (OSError, ElfError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    entries = {parse_addr(e) for e in args.entry}
    skips = {parse_addr(s) for s in args.skip}
    name = args.name or Path(args.elf).name
    ini_defaults = {}
    pnach_options = {}   # runtime option name -> pnach section
    pnach_file, pnach_sections = args.pnach, None
    if args.pnach_sections is not None:
        pnach_sections = [x for x in args.pnach_sections.split(",")]
    if args.config:
        with open(args.config, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg_dir = Path(args.config).resolve().parent
        entries |= {parse_addr(e) for e in cfg.get("entries", [])}
        skips |= {parse_addr(s) for s in cfg.get("skip", [])}
        name = cfg.get("name", name)
        ini_defaults = {k: str(v) for k, v in cfg.get("settings", {}).items()}
        if not pnach_file and cfg.get("pnach"):
            pnach_file = str(cfg_dir / cfg["pnach"])
        if pnach_sections is None:
            pnach_sections = cfg.get("pnach_sections")
        pnach_options = cfg.get("pnach_options", {})

    runtime_patches, options, toggles = [], list(pnach_options), {}
    if pnach_file:
        elf, runtime_patches, toggles = apply_pnach(elf, pnach_file, pnach_sections or [],
                                                    args.out, pnach_options)
        if elf is None:
            return 2
    for missing in (Path(args.out) / "bin" / "ps2native_missing.txt", Path(args.out) / "ps2native_missing.txt"):
        if missing.exists():
            found = {parse_addr(x) for x in missing.read_text().split() if x.strip()}
            print(f"adding {len(found)} entries recorded at run time ({missing.name})")
            entries |= found
    if args.hints:
        hinted = load_hints(args.hints)
        print(f"loaded {len(hinted)} function hints from {args.hints}")
        entries |= hinted

    print(f"{args.elf}: entry {elf.entry:08X}, {len(elf.segments)} segments, "
          f"{len(elf.function_symbols())} function symbols")
    prog = Program(elf, extra_entries=entries, skip_entries=skips).analyze()
    print("analysis: " + prog.summary())

    files, warnings = write_project(prog, args.out, name, elf.path, patches=runtime_patches,
                                    options=options, branch_toggles=toggles)
    print(f"generated {len(files)} C files in {Path(args.out) / 'gen'}"
          + (f" ({len(warnings)} warnings, see recomp_report.txt)" if warnings else ""))
    if args.no_build:
        return 0
    exe_stem = (Path(name).stem if name.lower().endswith(".elf") else name).replace(" ", "_").replace(".", "_")
    try:
        if args.backend == "pcsx2":
            exe = build_pcsx2(args.out, exe_stem)
        else:
            exe = build(args.out, files, exe_stem + ".exe", opt=args.opt, jobs=args.jobs)
    except (BuildError, Pcsx2BuildError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    write_ini(Path(exe).parent, iso_path, boot_path, args.bios, ini_defaults)
    print(f"built {exe} in {time.time() - t0:.1f}s")
    return 0


def is_iso(path):
    try:
        with open(path, "rb") as f:
            f.seek(16 * 2048 + 1)
            return f.read(5) == b"CD001"
    except OSError:
        return False


def apply_pnach(elf, pnach_file, sections, outdir, options=None):
    """Static part of a pnach goes into a patched copy of the ELF (which is
    then recompiled and embedded); the rest is returned as runtime patches.
    `options` maps runtime option names (ps2native.ini keys) to sections that
    stay switchable while the game runs.  Returns (elf, patches, toggles)."""
    try:
        parsed = pnach_mod.parse_pnach(pnach_file)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return None, [], {}
    patches, _settings, used, missing = pnach_mod.select(parsed, sections)
    opt_sections = list((options or {}).values())
    _p, _s, _u, opt_missing = pnach_mod.select(parsed, opt_sections)
    missing = sorted(set(missing) | set(opt_missing))
    if missing:
        avail = ", ".join(f"[{n}]" for n in parsed if n)
        print(f"error: pnach sections not found: {', '.join(missing)} (available: {avail})", file=sys.stderr)
        return None, [], {}
    optional, toggles = [], {}
    by_name = {n.lower(): sec for n, sec in parsed.items()}
    for idx, (opt, section) in enumerate((options or {}).items()):
        try:
            rt, tg = pnach_mod.make_optional(elf, by_name[section.lower()].patches, idx, elf.code_ranges())
        except ValueError as e:
            print(f"error: option {opt} [{section}]: {e}", file=sys.stderr)
            return None, [], {}
        optional += rt
        toggles.update(tg)
        print(f"option {opt} = [{section}]: {len(tg)} switchable branches, {len(rt)} runtime patches")
    data, runtime, applied = pnach_mod.apply_to_elf(elf.raw, elf, patches, elf.code_ranges())
    runtime += optional
    patched = Path(outdir) / (Path(elf.path).name + ".patched.elf")
    patched.write_bytes(data)
    print(f"pnach {Path(pnach_file).name}: {', '.join('[' + u + ']' for u in used) or '(global)'}: "
          f"{applied} code patches recompiled, {len(runtime)} runtime patches")
    return load_elf(str(patched)), runtime, toggles


def write_ini(bindir, iso_path, boot_path, bios, defaults=None):
    """Runtime defaults next to the exe (command-line options override them).
    Values the user already set in an existing ini are kept."""
    ini = Path(bindir) / "ps2native.ini"
    values = dict(defaults or {})
    if ini.exists():   # keep settings from an earlier build (e.g. from the ISO)
        for line in ini.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                values[k.strip()] = v.strip()
    if bios or "bios" not in values:
        values["bios"] = str(Path(bios or (project_root() / "bios")).resolve())
    if iso_path:
        values["iso"] = iso_path
    if boot_path:
        values["boot"] = boot_path
    ini.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
