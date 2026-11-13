# Silent Hill 3 FMV playback – working notes

Target: SLUS-20622, PCSX2 backend, real IOP (LLE, BIOS + ISO). The first video is the
Konami logo movie that starts about 39 s after boot.

## ROOT CAUSE (found 2026-11-13)

`CSR.FIELD` never toggled. The film player (`0x2B6E60` → `0x2B5FC0`) waits for the field
to change before it sets its "display on" flag `0x1F67B80` (loop around `0x100520`, which
waits for VSync and returns `CSR` bit 13). PCSX2 only flips FIELD when `SMODE1.CMOD != 0`
and `SYNCV` bit 0 is set (`third_party/pcsx2/pcsx2/Counters.cpp:52-56, 555-558`); the
real BIOS sets them in `SetGsCrt`, our HLE `be_set_crt` only wrote `SMODE2`. With FIELD
stuck at 1 the main thread waited forever, the VSync display callback `0x2B5830` returned
at once (flag 0), no frame was sent with a GIF FINISH, `0x2B5F50` never ran, and the
decoder stalled after 5 frames with every ring buffer full.

Fix: `host/NativeCpu.cpp` `be_set_crt` writes SMODE1/SRFSH/SYNCH1/SYNCH2/SYNCV for NTSC
with the values the real BIOS wrote (read back in `--reference --trace`, frame 600):
SMODE1 `0x740814504`, SMODE2 `0x1`, SRFSH `0x8`, SYNCH1 `0x7F5B61F06F040`,
SYNCH2 `0x33A4D8`, SYNCV `0xC7800601A01801`.

Evidence: before the fix `CSR` was `0x551B600C` in every dump, `0x1F67B80`=0 at every
`0x2B5830` call, `0x2B5F50` called 0 times. After: `0x2B5F50` runs ~30×/s and the movie
picture is displayed.

## Second bug: half-height FMV picture (fixed 2026-11-13)

After the FIELD fix every movie frame showed only its top half (rows 0..305 of a
1280×720 dump, constant across scenes; the reference was full height). Not the widescreen
pnach (same without it), not the busy-wait lead (same with 30 ms), not a GIF SIGNAL stall.
GS display registers were identical to the reference, so the framebuffer content differed.

Cause: the film player starts a DMA channel again right after a short transfer without
polling STR (sceGsPutDrawEnv, then a queued GIF packet from another thread). On hardware
the first transfer has long finished; here the native code is much faster than PCSX2's
DMA timing, the channel still has STR set, and PCSX2 ignores a CHCR write while STR is set
(`third_party/pcsx2/pcsx2/Dmac.cpp`, DmaExec), so the second transfer (the bottom half)
was lost.

Fix (found by the Fable agent): `FinishRunningDma()` in `host/NativeCpu.cpp`, called from
`ps2_io_write32`: when STR is written to a Dn_CHCR whose channel is still running, run
PCSX2's event scheduler until the channel stops (bounded to one frame of EE time), then
let the write through. Same idea as the busy-wait skipping: give the hardware the time a
real EE would have spent.

Also fixed by the agent: the sampling profiler could leave the EE thread suspended when
the process exited, keeping the memory card file locked (`s_prof_suspend_mutex`).

Evidence: `build/verify/opus_3000.png`, `opus_3150.png` (full 1280×720 picture).

## Where it stopped (before the root-cause fix)

1. The film player starts threads 14 (`0x2B72F0`, idle yield at prio 32) and 15
   (`0x2B8E60`, decoder) at ~39.4 s. Same as ps2-native-recomp (frame 2075 there).
2. Thread 15 gets stream data and decodes through the IPU (`IPU_CMD`/`IPU_CTRL`,
   DMA ch4 to IPU, ch3 from IPU). This path matches the reference.
3. After 5 decoded frames thread 15 loops in `0x2B8F54..0x2B8F74`: `RotateThreadReadyQueue(32)`
   and `0x2B95B0(ring 0x1F67FE0)` waiting for a **free output buffer**.
4. Buffers are released by the GS interrupt handler `0x2B5F50` (added with
   `AddIntcHandler(INTC_GS, 0x2B5F50)` by `0x2B6E60`, which also registers the VSync
   callback `0x2B5830` and calls `sceGsPutIMR(GsGetIMR() & ~0x200)` to unmask FINISH).
   `0x2B5F50` acks `CSR.FINISH` (`0x2B5810`), releases one ring entry (`0x2B9680`) and
   counts the frame.
5. **`0x2B5F50` is never called** (checked with `PS2NATIVE_WATCH`), while the VSync
   callback `0x2B5830` runs every frame. So the GS FINISH interrupt is either not raised
   by PCSX2 or not delivered by our kernel.

## Open questions / next steps

* Is `CSR.FINISH` ever set, and is `INTC_STAT` bit 0 raised? (PCSX2 raises it in
  `Gif_FinishIRQ()` when the GIF finishes all paths after a FINISH register write:
  `third_party/pcsx2/pcsx2/Gif_Unit.cpp:175`, called from `Gif_Unit.h:848`.)
* IMR difference: the reference ends with `GsPutIMR(0xFFFFFCFF)` (SIGNAL and FINISH
  unmasked), we end with `0x7D00` (SIGNAL masked). With SIGMSK set, a second SIGNAL
  before the first is cleared stalls the GIF (`Gif_Unit.cpp:49-60`), which would also
  prevent FINISH. Find out where the reference's IMR value comes from before copying it.
* Does our kernel deliver INTC_GS while thread 15 spins in `RotateThreadReadyQueue`?
  (Delivery happens in `ps2_service` / idle wait; check `kernel_interrupts_enabled()`
  and the handler's `ei` at `0x2B5FA0`.)

## Tried / fixed so far

| Change | Result |
|---|---|
| PCSX2 frame limiter (`VMManager::Internal::Throttle`) disabled in native host mode | Boot 2× faster (memory card screen at 28 s like PCSX2); the EE thread spent 80 % sleeping before |
| `GsGetIMR` returned the write-only GS register (garbage `0x551B6000`); now a shadow copy, initialised to `0x7F00` | Correct IMR values; video still stalls |
| `RemoveIntcHandler`/`RemoveDmacHandler` now return the id (or −1); alarm syscalls `0xFD` = ReleaseAlarm, `0xFE` = iSetAlarm; `GsPutIMR` returns the old value; i-variants of Enable/DisableIntc/Dmac return values; `ReferThreadStatus` of a deleted thread returns 0 | Matches the reference kernel and the game's syscall stub order; no visible change for the intro |
| Busy-wait skipping: a read of a busy `IPU_CMD`/`IPU_CTRL` or a running `Dn_CHCR` advances PCSX2's scheduler to the next event (at most 4 ms ahead of real time) | Decode rate 1–2 → 5+ frames/s; the IPU no longer starves the decoder |

## Ruled out

* The game writing `D4_CHCR = 0x5` (no STR) is intentional: `0x2B7AE0` only starts the
  DMA with `0x105` when the stream has data.
* Stream data arrives (thread 15 leaves the "no data" loop at the same point as the
  reference).
* Thread rotation (`RotateThreadReadyQueue`) and the vsync callback thread behave like
  the reference.
