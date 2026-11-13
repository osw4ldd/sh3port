# sh3port — Silent Hill 3 Native PC Port

**A native x86-64 Windows port of Silent Hill 3 (USA, SLUS-20622), without an emulator
running the game code.**

The game's code for the PlayStation 2 main CPU (Emotion Engine, MIPS R5900) is
**statically recompiled** to C and built into a regular Windows `.exe` with MSVC. The
game logic is not interpreted or JIT-compiled; it runs as native code on your CPU. The
goal is a Silent Hill 3 port that fits modern PCs: high resolutions, 16:9 widescreen, a
60 FPS option, modern controllers, its own launcher, and **eventually no dependency on
PCSX2 at all**.

Today the graphics (GS/VU1), sound (SPU2) and part of the PS2 hardware still come from
the [PCSX2](https://github.com/PCSX2/pcsx2) core. These parts are being replaced with
native code step by step; the plan and progress are in [`NATIVE_PLAN.md`](NATIVE_PLAN.md).
The underlying technology is called **ps2native**: the recompiler
(`recompiler/ps2recomp`) and the runtime (`runtime/`).

> **This repository contains no game files and no BIOS.** You need a disc image dumped
> from your own copy of the game and a BIOS dumped from your own PS2 console.

---

## Contents

- [How it works](#how-it-works)
- [Status](#status)
- [Layout](#layout)
- [Setup](#setup)
- [Building Silent Hill 3](#building-silent-hill-3)
- [Playing: the launcher](#playing-the-launcher)
- [Settings (`ps2native.ini`)](#settings-ps2nativeini)
- [Command line](#command-line)
- [Patches (pnach)](#patches-pnach)
- [Release package](#release-package)
- [Tests](#tests)
- [Debugging tools](#debugging-tools)
- [Known issues and limitations](#known-issues-and-limitations)
- [License and legal notes](#license-and-legal-notes)

---

## How it works

```
 Game disc (.iso)
   │  SYSTEM.CNF → SLUS_206.22 (EE ELF)
   ▼
 ps2recomp (Python) ──► gen/*.c ──► MSVC ──► SLUS_206_22.exe
   ├─ R5900 decoder (MMI, FPU, VU0 macro)             ├─ ps2native runtime (C)
   ├─ function discovery, jump tables                 │   native EE code, EE kernel HLE,
   ├─ pnach patches (code patches compiled in)        │   threads/semaphores/interrupts/alarms,
   ├─ "hle" map: SDK functions replaced natively      │   SIF, native IOP services, native FMV
   └─ C code generation                               └─ PCSX2 core (C++)
                                                          real IOP (BIOS), CD/DVD, SPU2 sound,
                                                          DMAC, VIF, VU0/VU1 (microVU), IPU,
                                                          GIF, GS (DX12/DX11/Vulkan/GL), pads,
                                                          memory cards
```

* **Recompiler (`recompiler/ps2recomp`, Python):** reads the ELF, finds functions with a
  full R5900 decoder (symbols, `jal` targets, `switch` jump tables, data pointers,
  `lui/addiu` constants, secondary entry points) and turns every function into a C
  function. Delay slots, branch-likely, 128-bit GPRs, HI/LO, MMI, the PS2 FPU rules (no
  Inf/NaN, round toward zero) and VU0 macro mode are supported. Given an ISO, it extracts
  the boot ELF from the disc itself. Functions listed in the game config's `"hle"` map
  get a hook that lets the runtime replace them natively (the original body stays
  callable).
* **Runtime (`runtime/`, C):** the environment the native code runs in. The EE kernel is
  re-implemented (HLE): fiber-based priority threads, semaphores,
  sleep/wakeup/suspend, INTC/DMAC handlers, alarms, `SetSyscall`, SIF DMA/RPC services.
  Real-time EE clock and event processing at "safe points" (backward branches).
  Code the game loads at run time (overlays) is recompiled on the fly into DLLs
  (`dyn.c`). Native IOP services (`hle_iop.c`, `iop_services.c`, `iso.c`) and native
  MPEG-2 video (`hle_mpeg.c`, `mpeg_native.c`) are the first steps away from PCSX2.
* **PCSX2 backend (`host/`, C++):** PCSX2's VMManager is opened in a "native host" mode
  (`patches/pcsx2-native-host.patch`). The EE CPU provider runs the recompiled game;
  hardware accesses go through PCSX2's memory map (vtlb). With a BIOS, the **real IOP**
  boots from the BIOS (LLE): disc, sound, pads and memory cards are PCSX2's own.
* **Software backend (`runtime/src/soft`):** no dependencies, builds with gcc; has its own
  DMA/VIF/GIF and a software GS (no VU1, no IPU). For tests and experiments.
* **Launcher (`launcher/`, Python/Tkinter):** game file selection, settings and the play
  screen (12 languages).

## Status

Silent Hill 3 (SLUS-20622) with the PCSX2 backend and the real IOP (LLE):

| Part | Status |
|---|---|
| Boot, logos, memory card check, warning screen | ✅ Works, boot time matches a real PS2 / PCSX2 |
| Sound, disc reading, memory cards, controllers (Xbox/PS4/PS5/Switch Pro + keyboard) | ✅ |
| High resolution (1×–8×), 16:9 widescreen, borderless fullscreen | ✅ |
| Discrete GPU selection (NVIDIA Optimus / AMD) | ✅ |
| 60 FPS, camera patches (toggle at run time) | ✅ builds, needs in-game testing |
| **FMV / video playback** | ✅ Full screen ([`VIDEO_FIX_NOTES.md`](VIDEO_FIX_NOTES.md)). FMVs play at about half speed (≈16 pictures/s); the vblank timing is being investigated |
| In-game code modules (overlays) | ✅ Recompiled at run time and loaded as DLLs (`runtime/src/core/dyn.c`) |
| Gameplay | ⏳ Being tested |

Moving off PCSX2 ([`NATIVE_PLAN.md`](NATIVE_PLAN.md)):

| Stage | Status |
|---|---|
| 1. Native IOP (no BIOS), `--hle-iop` | ⏳ CD/files (from the ISO), IRX loading, the SH3 sound driver and CRI ADX protocols, IOP heap work; the loading screen is passed. Memory card and pads are next |
| 2. FMV: native MPEG-2 instead of the IPU (FFmpeg), `PS2NATIVE_MPEG=native` | ✅ Verified picture by picture against the real IPU output (mean abs. difference 0.11/255) |
| 3. VU1, 4. GS renderer, 5. sound (SPU2), 6. removing PCSX2 | ⏳ |

## Layout

```
sh3port/
├── recompiler/ps2recomp/   Python recompiler (elf, r5900, analysis, codegen, pnach, iso, build, dyn)
├── runtime/
│   ├── include/            ps2rt.h, ps2_mmi.h, ps2_vu0.h (headers used by the generated code)
│   └── src/core/           kernel.c (EE kernel HLE), dispatch.c, memory.c, sif.c, sif_lle.c, rtmain.c,
│                           dyn.c (overlay recompilation), hle_iop.c / iop_services.c / iso.c (native IOP),
│                           hle_mpeg.c / mpeg_native.c (native FMV)
│   └── src/soft/           software backend
├── host/                   PCSX2 backend: Main.cpp (window, settings, controllers), NativeCpu.cpp (EE provider)
├── patches/                PCSX2 commit id + native-host patch
├── games/                  Per-game configuration format (games/README.md); the files themselves are not distributed
├── launcher/               SH3 launcher (sh3_launcher.py) and its translations (sh3_lang.py)
├── app/                    Generic recompiler GUI (ps2native_gui.py) and CLI entry
├── scripts/                PCSX2 setup/build, run-time compilation and packaging scripts
├── tests/                  R5900 test programs generated with an assembler
├── bios/                   Put your own BIOS here (not in the repository)
├── NATIVE_PLAN.md          Plan, measurements and findings for the PCSX2-free port
└── VIDEO_FIX_NOTES.md      Notes on the FMV fix
```

Folders **not** in the repository (generated or provided by you): `third_party/`
(PCSX2), `build/`, `release/`, `tools/`, `bios/*.bin`.

## Setup

Requirements:

* Windows 10/11 x64
* Visual Studio (MSVC, "Desktop development with C++") + CMake + Ninja.
  The build scripts use the `vcvars64.bat` path in `scripts/configure_pcsx2.bat` (and
  `scripts/dyncompile.bat`); fix it if Visual Studio is installed elsewhere.
* Python 3.11+ (`pip install pillow` for the launcher images, `pip install pyinstaller`
  for packaging)
* git, curl, 7-Zip (for the PCSX2 setup)
* FFmpeg (LGPL, shared) for native FMV decoding: extract
  `ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip`
  ([BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)) into
  `tools/ffmpeg/`. Packaging copies these DLLs next to the game (PCSX2's own FFmpeg has
  no decoders).

To download PCSX2 at the tested commit, apply the patch and get the prebuilt
dependencies:

```bat
scripts\setup_pcsx2.bat
```

The first game build compiles PCSX2 once (15–20 min). Later builds only compile the game
code and the runtime (1–3 min).

## Building Silent Hill 3

First create `games/sh3/game.json` (and copy the pnach patches you want) as described in
[`games/README.md`](games/README.md); these game-specific files are not in the repository.

```bat
cd recompiler
python -m ps2recomp "D:\Games\Silent Hill 3 (USA).iso" -o ..\build\games\sh3 --config ..\games\sh3\game.json
```

This command:

1. reads `SYSTEM.CNF` from the ISO and extracts `SLUS_206.22`,
2. applies the pnach patches from `game.json` (code patches are compiled into the ELF,
   switchable patches become run-time options),
3. turns ~12,800 functions into C (`build/games/sh3/gen/`),
4. builds `build/games/sh3/bin/SLUS_206_22.exe` with MSVC and puts the DLLs next to it,
5. writes the ISO and BIOS paths into `bin/ps2native.ini`.

Put the BIOS into the `bios\` folder or pass `--bios file.bin`.

## Playing: the launcher

```bat
python launcher\sh3_launcher.py
```

or the packaged `release\SilentHill3\Silent Hill 3.exe`.

* **Game files:** select the Silent Hill 3 disc image (.iso/.chd/.cso) and a PS2 BIOS.
  The ISO is checked to really be SLUS-20622 and the BIOS to be a valid PS2 BIOS; the
  BIOS version and region are shown. Files are not copied, only their locations saved.
* **Graphics:** internal resolution, aspect ratio, fullscreen, VSync, anisotropic
  filtering, FXAA, AMD CAS sharpening, graphics API, discrete/integrated GPU.
* **Game settings:** 60 FPS, fast loading, over-the-shoulder camera patches.
* **Controls:** controller auto/off, vibration, keyboard remapping.
* **Saves:** memory card backup/restore (automatic backup before playing).
* **Language:** Türkçe, English, Español, Português (BR), Français, Deutsch, Italiano,
  Русский, Polski, 日本語, 한국어, 简体中文. The Windows language is picked on first start.

In game, **Alt+Enter** or **F11** toggles fullscreen.

## Settings (`ps2native.ini`)

`bin/ps2native.ini` is written by the launcher and can be edited by hand. Command line
options override these values.

| Key | Default | Meaning |
|---|---|---|
| `iso`, `bios`, `boot` | | Disc image, BIOS file/folder, boot path (`cdrom0:\SLUS_206.22;1`) |
| `renderer` | auto | `dx12`, `dx11`, `vulkan`, `opengl`, `software` |
| `upscale` | 3 | Internal resolution multiplier (1 = PS2, 3 ≈ 1080p, 6 ≈ 4K) |
| `widescreen` | 1 | 16:9 picture (together with the widescreen patch) |
| `fullscreen` | 0 | Borderless fullscreen |
| `vsync` | 0 | Vertical sync |
| `anisotropy` | 16 | 0/2/4/8/16 |
| `fxaa`, `sharpen` | 0 | FXAA; CAS sharpening (0–100) |
| `gpu` | high | `high`: discrete GPU, `integrated`: integrated GPU |
| `fast_cdvd` | 1 | Skip disc read delays |
| `fps60`, `camera_behind`, `camera_l3` | 0 | Game patches switchable at run time |
| `controller`, `vibration` | 1 | Controller and vibration |
| `key_<Button>` | | Keyboard mapping, e.g. `key_Cross=S`, `key_Start=Return` |

## Command line

```
SLUS_206_22.exe [options]
  --iso FILE           disc image
  --bios PATH          PS2 BIOS file or folder (real IOP)
  --hle-iop            ignore the BIOS: native IOP services, CD from --iso (work in progress)
  --boot PATH          boot path (cdrom0:\SLUS_206.22;1)
  --renderer NAME      auto, dx12, dx11, vulkan, opengl, software
  --upscale N          internal resolution multiplier
  --widescreen / --no-widescreen
  --fullscreen / --windowed
  --frames N           exit after N frames (for tests)
  --dump-frame FILE    save the last frame as PNG before exiting (use a C:\... path)
  --reference          run the same disc fully emulated by PCSX2 (comparison)
  --trace              system calls, thread states, profiling report
  -v                   verbose log
```

Recompiler (`python -m ps2recomp`): `--config game.json`, `--pnach file`,
`--pnach-sections "Widescreen 16:9,60 FPS"`, `--backend pcsx2|soft`, `--bios`,
`--entry 0xADDR`, `--skip 0xADDR`, `--hints funcs.json` (function list from
Ghidra/IDA/REA), `--no-build`.

## Patches (pnach)

The widescreen / 60 FPS / camera patches are PCSX2 community patches (widescreen:
nemesis2000, 60 FPS: asasega, camera: Snake356); get them from the
[PCSX2 patches repository](https://github.com/PCSX2/pcsx2_patches) (see
[`games/README.md`](games/README.md)). Since native code cannot be patched at run time:

* **Permanent code patches** (`pnach_sections`) are applied to the ELF before compiling.
* **Switchable patches** (`pnach_options`): data patches are applied every VSync;
  "conditional branch → nop" code patches are compiled with an extra condition and
  enabled with the key in `ps2native.ini` (`fps60=1`).

## Release package

```bat
python scripts\package_sh3.py          :: release\SilentHill3\
python scripts\package_sh3.py --zip    :: + release\SilentHill3.zip
```

Package: `Silent Hill 3.exe` (launcher), `bin\` (game + PCSX2 DLLs), license and readme.
It contains no game files and no BIOS. The script keeps the memory card and settings of
an existing package; it does not run while the launcher or the game is open.

## Tests

```bat
python tests\run_tests.py --backend=pcsx2
python tests\run_tests.py --backend=soft
```

The tests build ELFs with a small assembler that emits real R5900 machine code
(`tests/mips_asm.py`), recompile them, run the exe and check the result: CPU (with and
without symbols), FPU rounding, thread scheduling, interrupts/timers/alarms, GS drawing
(pixel checks), a VU1 microprogram (VIF1 MPG/UNPACK → XGKICK), VU0 micro mode (VCALLMS)
and negative tests showing that failures are caught.

## Debugging tools

* `--trace`: every system call, a thread state dump every 2 seconds, a guest code
  sampling profile (time per address) and a host profile (hottest functions with PDB
  symbols), IPU/DMA access log.
* `PS2NATIVE_WATCH=2B5830,2B5F50`: logs indirect calls to these addresses (callbacks,
  interrupt handlers).
* `PS2NATIVE_PEEK=addr,addr`: guest words printed with every `--trace` report and every
  watched call.
* `PS2NATIVE_RPCLOG=1`: logs the SIF RPC binds/calls/replies the game sends to the real
  IOP (and the CRI DTX buffers) — the reference for the native IOP services.
* `PS2NATIVE_MPEG_DUMP=dir`: saves the pictures the real IPU decodes (RAW8) and the
  data sent to it — the reference for the native MPEG decoder.
* `PS2NATIVE_IO_GS=1`: limits the I/O log to GS `CSR`/`IMR` accesses.
* `--reference`: runs the same game fully in PCSX2 to compare behaviour and timing.

## Known issues and limitations

* **FMV speed:** videos play at about 16 pictures/s instead of 29.97 (also with the
  real IPU): about a quarter of the vblank interrupts do not reach the film player.
* **BIOS-free mode (`--hle-iop`)** stops at the memory card check (work in progress).
* No DLSS / FSR / frame generation directly (PS2 rendering produces no motion vectors).
  Driver-level frame generation (NVIDIA Smooth Motion) can be used.
* Code loaded at run time (overlays) is compiled the first time it runs (a few seconds,
  cached in `bin\dyncache\`); this needs MSVC on the machine for now.
* A VU0 program started with `VCALLMS` completes before the EE continues.
* The software backend has no IPU, so it cannot play videos.

## License and legal notes

* This project links with PCSX2 code (GPL-3.0+) and is therefore licensed under
  **GPL-3.0-or-later** (`LICENSE`). The generated exes are covered by the same license.
* This repository contains **no game files, no BIOS, no code generated from the game, no
  game patches and no Konami artwork.** Use your own legal copies.
* Silent Hill is a trademark of Konami Digital Entertainment. This project is not
  affiliated with Konami or Sony.
* Some EE kernel behaviour (e.g. the `RemoveIntcHandler` return value, alarm system call
  numbers) was checked against [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp)
  (MIT), and ideas (SID-based IOP services, SDK signature scanning) were taken from
  [PS2Recomp](https://github.com/ran-j/PS2Recomp); no code was copied from either project.
* Native FMV decoding uses [FFmpeg](https://ffmpeg.org/) (LGPL), not included in this
  repository.
