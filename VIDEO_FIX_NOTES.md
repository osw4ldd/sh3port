# Silent Hill 3 FMV playback – working notes

Target: SLUS-20622, PCSX2 backend, real IOP (LLE, BIOS + ISO). The first video is the
Konami logo movie that starts about 39 s after boot.

## Where it stops (current state)

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
