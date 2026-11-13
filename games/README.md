# Game configurations

Game-specific files are **not distributed** with this repository. Put them into a folder
per game, e.g. `games/sh3/`, and pass the config to the recompiler:

```bat
cd recompiler
python -m ps2recomp "D:\Games\Silent Hill 3 (USA).iso" -o ..\build\games\sh3 --config ..\games\sh3\game.json
```

## `game.json`

```json
{
  "name": "SLUS_206.22",
  "title": "Silent Hill 3",
  "serial": "SLUS-20622",
  "pnach": "SLUS-20622_XXXXXXXX.pnach",
  "pnach_sections": ["Widescreen 16:9"],
  "pnach_options": {
    "fps60": "60 FPS"
  },
  "settings": {
    "widescreen": "1",
    "upscale": "3",
    "fast_cdvd": "1"
  },
  "entries": ["0x00123450"],
  "skip": [],
  "hle": {
    "0x00104000": "sceOpen",
    "0x0010B000": "sceCdRead",
    "0x0011D000": "sceMpegGetPictureRAW8"
  }
}
```

The addresses above are placeholders.

| Key | Meaning |
|---|---|
| `name` | Name of the boot ELF on the disc (`SYSTEM.CNF`) |
| `pnach`, `pnach_sections` | PCSX2 patch file (in this folder) and the sections compiled into the ELF |
| `pnach_options` | Patch sections that stay switchable at run time (`ps2native.ini` key → section) |
| `settings` | Default `ps2native.ini` values |
| `entries`, `skip` | Extra function entry points / addresses not to treat as functions |
| `hle` | SDK functions replaced by native implementations (`runtime/src/core/hle_iop.c`, `hle_mpeg.c`); find their addresses with an SDK signature scan or a disassembler (see `NATIVE_PLAN.md`) |

## Patches (pnach)

Widescreen / 60 FPS / camera patches for Silent Hill 3 are community work (widescreen:
nemesis2000, 60 FPS: asasega, camera: Snake356) and are available from the
[PCSX2 patches repository](https://github.com/PCSX2/pcsx2_patches). Copy the `.pnach` for
your disc into the game folder and name it in `game.json`.
