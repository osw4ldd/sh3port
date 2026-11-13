# Plan: running without PCSX2

Goal: SH3 (SLUS-20622) runs fully native on PC, without a BIOS and without the PCSX2 core.
Today: the EE code is native (static recompilation + overlay DLLs compiled at run time).
GS, VIF/VU1, IPU, SPU2 and the IOP still come from PCSX2 (LLE with a BIOS).

The game's own configuration (`games/sh3/game.json`, function addresses) is not part of
the repository; the addresses below are the reference for writing it.

## 1. Analysis (2026-11-13)

Tools:
- Ghidra 12.1.4 + ghidra-emotionengine-reloaded v2.1.39 (`r5900:LE:32` language), headless.
- REA. On Windows its Ghidra route only accepts PE/x86 files and does not open MIPS
  ELFs, so the SH3 analysis was done with Ghidra headless.
- The ran-j/PS2Recomp source. That repository is GPL-3.0; **only ideas** were taken from
  it, no code was copied.

### Where SH3 gave us trouble (Ghidra decompilation)

| Address | What it does | What it means for us |
|---|---|---|
| `0x1EC420` | `FlushCache(2)`, then `0x1512D0(file,0,0x1F6D600)`, `0x151130`, `0x1EC3E0` | Overlay loader. |
| `0x1512D0` | Queues a request on a command queue via object `0x484980` (handler `0x154680`) | File loading runs in another thread through CD reads. If CD reads work on the native IOP, overlays load too; their code is already compiled by `dyn.c`. |
| `0x2B5830` | FMV VSync callback. Reads `REG_GS_CSR` bit 13 (FIELD) and switches fields | Confirms the FMV root cause: on a native GS, FIELD must toggle every VSync (with SMODE1/SYNCV set). |
| `0x2B5FC0` | Spins in `0x100520` until the field changes, then sets display flags | Same issue. |
| `0x103918` / `0x103AE8` / `0x103050` | `sceSifBindRpc` / `sceSifCallRpc` / `sceSifSendCmd` | IOP services are bound by RPC number; HLE can work at this level. |

### IOP services used by SH3 (`lui 0x8000`/`ori` constants in the ELF + decompilation)

| SID | Module | Source |
|---|---|---|
| `0x80000001` | fileio | in sif.c |
| `0x80000003` | iopheap | in sif.c |
| `0x80000006` | loadfile | in sif.c |
| `0x80000592` / `593` / `595` / `597` / `59A` | cdvdfsv: init / S-cmd / N-cmd / search / diskready | `0x10B178` "Libcdvd bind err"; PS2SDK `common/include/libcdvd-rpc.h` |
| `0x80000900`–`0x8000091B` | libdbc / dbcman (pad, DualShock 2) | `0x121BD0` "libdbc: bind failed", "dbcman.irx" |
| `0x80000701` | sdrdrv (libsd RPC) | `0x109EF0` |
| `0x80000400` | mcserv (memory card) | |

IRX modules loaded from the disc: `dbcman`, `ds2u`, `sio2d`, `sio2man`, `libsd`, `sdrdrv`,
`mcman`, `mcserv`, `iop_sound/cri_adxi.irx` (CRI ADX audio streaming), `libsmf2` (MIDI),
`sd_call`.

### Measured RPC traffic (real IOP, LLE)

Command:
`PS2NATIVE_RPCLOG=1 PS2NATIVE_PRESS="2600-5000/150:Return,2675-9000/150:S" SLUS_206_22.exe --frames 7000`

5715 RPC calls in 7000 frames, from the intro to entering the game. Every (SID, fno) pair
the native IOP has to answer:

| SID | fno (count) | Notes |
|---|---|---|
| `80000001` fileio | 0 open (58), 1 close (57), 2 read (83), 4 lseek (168), 23 (139), 255 (1) | Packet layout of the SCE ROM fileio, different from PS2SDK. open: `+12` mode, `+20` path. |
| `80000003` iopheap | 1 (9), 2 (12), 4 (12) | |
| `80000006` loadfile | 6 (12), 255 (1) | |
| `80000592` cdvd init | 0 (2) | |
| `80000593` cdvd S-cmd | 1 (1), 4 (1439), 34 (1) | 1 = ReadClock. 4 is probably GetError, called after every read (always answers 0). |
| `80000595` cdvd N-cmd | 1 read (590), 14 (590) | read packet: lbn, sectors, ee dst, rmode, ... (24 bytes) |
| `80000597` cdvd search | 0 (106) | `cdvdfsv_rpc4_sz12c` (300 bytes) |
| `80000701` sdrdrv | 32784, 32976, 32992, 33008, 33024 (97) | |
| `80000400` mcserv | 1, 2, 3, 4, 5, 12, 13, 18, 21, 254 | |
| `80000900` / `8000091B` dbcman | `0x80000901`… / `0x8000091B` (982, the pad, every frame) | |
| `00000057` | 0x01000000 (981), 0x02000000 (1) | SH3 sound driver (sd_call / smf2). |
| `7D000000` | 2, 1024, 1027, 1032, 1034, 1058 | CRI ADX (cri_adxi.irx). |

### SDK signature scan (SH3)

With the sce-symbol-scanner database from ran-j/PS2Recomp (used for analysis only, not
added to the repository) **627 SDK functions** were identified in SH3. The result goes to
`build/games/sh3/sdk_symbols.json` (build/ is not in git).

| Library | Count | Examples |
|---|---|---|
| libkernl | 259 | system call wrappers, `sceOpen` 0x104B10, `sceRead` 0x105150 … |
| libmpeg | 92 | `sceMpegCreate`, `sceMpegGetPictureRAW8` 0x11DFA0, `sceMpegIsEnd` 0x11E070 |
| libcdvd | 18 | `sceCdRead` 0x10B650, `sceCdSync` 0x10AE30, `sceCdInit` 0x10B0B0, `sceCdLayerSearchFile` 0x10A8F0 |
| libdbc | 13 | `sceDbcInit` 0x121B88, `sceDbcSRData`, `sceDbcReceiveData` |
| libmc | 27 | `sceMcOpen` / `Read` / `Write` / `Sync` … |
| libipu | 7 | `sceIpuInit`, `sceIpuSync` |
| libgraph | 11 | `sceGsSyncPath` 0x116E08, `sceGsSwapDBuff` |
| libsdr | 3 | `sceSdRemote` 0x10A008 |

So HLE can work at two levels: RPC (SID/fno) or the SDK function itself.

### Ideas taken from ran-j/PS2Recomp

1. **IOP services per SID** (`IopService::sids()` / `handleRpc` in `ps2xIOP`). Our `sif.c`
   already works that way; `iop_services.c` extends the list.
2. **SDK signature database** (sce-symbol-scanner): SHA-1 of the bytes with relocation
   fields masked finds functions such as `sceCdRead`, `sceMpeg*`, `scePad*` in a stripped
   ELF, so the IPU and MPEG can be replaced at library level. ran-j uses FFmpeg for that.
3. **Game-specific function replacement** (`handler@0xADDR`, `ret0`). **Done:** the
   `"hle"` map in `game.json`.
4. **Delay-slot guard:** an address right after a control transfer must not be seeded as
   a function start. **Done:** `is_branch()` in `dyn.c`; the overlay was recompiled from
   scratch and ran 9000 frames without a crash.
5. **To avoid:** ran-j's `libsd` HLE is a ~70-line stub; there is no sound without SPU2.
   Sound will be handled separately.

## 2. Stages

Every stage keeps the game working on its own before PCSX2 is removed. The LLE path stays
available for comparison (`--reference`).

1. **Native IOP (no BIOS needed).**
   - `--hle-iop`: forces the native IOP even when a BIOS is present.
   - `PS2NATIVE_RPCLOG=1`: logs the real RPCs in LLE (the reference every service is
     checked against).

   **Status (2026-11-13), with `--hle-iop`, without the BIOS IOP:**
   - **Function-level HLE:** the `"hle"` map in `game.json`. The recompiler puts a
     `ps2_hle()` hook into each listed function; in LLE mode the original code runs.
     18 SDK functions are native:
     - fileio: `sceOpen` / `Read` / `Lseek` / `Close` / `Devctl` / `Getstat`
     - libcdvd: `sceCdRead` / `Sync` / `LayerSearchFile` …

     Source: `runtime/src/core/hle_iop.c`; ISO reader: `runtime/src/core/iso.c`.
   - **`rom0:ROMVER` virtual file.** Without it the game keeps retrying the open.
   - **RPC services** (`runtime/src/core/iop_services.c`):
     - SH3 sound driver `0x57`: state 7 = ready. Commands `0x1404TTSS` (load bank SS of
       type TT) set status bits (0x207 → 0x777); without the bits the game stayed on the
       loading screen.
     - CRI ADX `0x7D000000`: non-zero handles. With 0 the game prints "Can't create ADX
       handle" and loops forever.
     - CRI DTX channel: empty reply to the SIF DMA, sequence number + 1. In LLE the
       channels are empty at this stage too. Protocol: `0x20DAB0` send, `0x20DA10`
       receive; 16-byte chunk entries.
   - **IOP heap:** first-fit allocator with `free`. The old 448 KB bump allocator ran out;
     SH3 allocates 266 KB + 160 KB buffers.
   - **Result:** IRX modules load, the file table is read from the ISO, 421 CD reads, all
     sound banks loaded (state 0x777), the loading screen is passed.
   - **Where it stops:** the memory card. In LLE the game now calls mcserv
     (`0x80000400`) fno 21/1 (GetInfo/Sync), 12, 13, 18, then dbcman (pad). Next: replace
     libmc at function level (the SDK scan has the `sceMcInit` / `GetInfo` / `Sync` /
     `Open` / `Read` / `Write` … addresses), keep saves in `memcards/`, then the pad
     (dbcman).
   - **Remaining services:** dbcman (pad), mcserv (memory card), sdrdrv (zeros for now).
   - **Screenshots:** `--dump-frame` only works with a Windows-style (`\`) path.

2. **FMV: native MPEG-2 instead of the IPU (FFmpeg).** Works with `PS2NATIVE_MPEG=native`.
   - **Decoder:** `runtime/src/core/mpeg_native.c`, on FFmpeg 9.0 LGPL shared
     (`tools/ffmpeg`, not in git); PCSX2's FFmpeg has no decoders. Same DLL majors
     (avcodec-63); packaging copies the full build.
   - **RAW8 layout measured against the real IPU output** (`PS2NATIVE_MPEG_DUMP`
     observation mode): macroblocks `Y[16][16] Cb[8][8] Cr[8][8]` (PCSX2
     `IPU_MultiISA.h:26`) in **column-major** order (`n = mx*mbh + my`). Mean absolute
     difference 0.11/255 over 170 pictures, identical picture order.
   - **Data path:** the game's channel 4 (toIPU) DMA chain does not reach the IPU.
     `mpeg_ipu_dma_start` walks the chain like the DMAC, queues the elementary stream and
     reports the transfer finished (MADR/TADR/QWC, STR, CIS4). The game's ring accounting
     (`0x2B7EB0`) runs unchanged.
   - **Native `sceMpegGetPictureRAW8`:** without a picture it only calls the "no data"
     callback. The real library uses the stopDMA/restartDMA pair only while it sends its
     own data to the IPU (measured), which the native path never does.
   - **Header:** w/h/frameCount/flags. Measured pattern: pts += 3003,
     flags = 0x1D8 | picture type; the game does not read pts.
   - **Native `sceMpegIsEnd` / `sceMpegReset`.**
   - **Result:** in native mode no BDEC/CSC command reaches the IPU during playback; only
     SETIQ/SETVQ/SETTH while the library starts. FMVs play with the correct picture
     (frames 3000, 4500, 6000).
   - **Open issue (also with the real IPU):** FMVs play at about 16 pictures/s instead of
     29.97. The VSync callback `0x2B5830` runs only ~45 times/s and CSR.FIELD repeats, so
     about a quarter of the vblank interrupts do not reach it. Stopping `be_update` on a
     newly raised VBLANK did not help; next: count deliveries in
     `kernel_deliver_interrupts`. The native path runs at about 14 pictures/s.

3. **VU1:** translate the microprograms (uploaded with VIF MPG) to C, statically or at run
   time like the EE code.
4. **GS:** our own GPU renderer (D3D11 or Vulkan). The biggest job.
5. **Sound:** SPU2 (the ADX and MIDI drivers are IOP code, so an IOP interpreter + SPU2
   mixer are needed).
6. Remove the PCSX2 dependency.
