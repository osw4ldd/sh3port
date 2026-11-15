"""System-level test programs: kernel threads, interrupts/timers, GS drawing."""

from mips_asm import Asm
from programs import Checker, prologue, epilogue, SYS_EXIT, SYS_PRINT


def syscall(a, num):
    a.li("v1", num)
    a.syscall()


def puts(a, label):
    a.la("a0", label)
    syscall(a, SYS_PRINT)


# =====================================================================
# Kernel: threads, semaphores, sleep/wakeup, priorities
# Expected output: ABCDEFGHI
# =====================================================================
def build_thread_test(path):
    a = Asm()
    a.func("_start")
    a.li("sp", 0x01F00000)
    a.la("a0", "semaparam")
    syscall(a, 0x40)             # CreateSema
    a.move("s0", "v0")
    a.la("t0", "sema_id")
    a.sw("s0", 0, "t0")
    a.la("a0", "thparam1")
    syscall(a, 0x20)             # CreateThread(worker, prio 10)
    a.move("s1", "v0")
    a.move("a0", "s1")
    a.li("a1", 0)
    syscall(a, 0x22)             # StartThread: lower priority, no preemption
    puts(a, "sA")
    a.move("a0", "s0")
    syscall(a, 0x44)             # WaitSema -> worker runs
    puts(a, "sC")
    a.li("a0", 0)
    a.li("a1", 20)
    syscall(a, 0x29)             # ChangeThreadPriority(self, 20) -> worker preempts
    puts(a, "sE")
    a.la("a0", "thparam2")
    syscall(a, 0x20)             # worker2 at prio 5
    a.move("s2", "v0")
    a.move("a0", "s2")
    a.li("a1", 0)
    syscall(a, 0x22)             # preempts immediately, then sleeps
    puts(a, "sG")
    a.move("a0", "s2")
    syscall(a, 0x33)             # WakeupThread -> worker2 preempts
    puts(a, "sI")
    a.move("a0", "s0")
    syscall(a, 0x45)             # PollSema on an empty sema fails with -1 (EE kernel)
    a.li("t0", -1)
    a.beq("v0", "t0", "poll_ok")
    a.nop()
    puts(a, "sPollFail")
    a.label("poll_ok")
    syscall(a, 0x2F)             # GetThreadId == 1
    a.li("t0", 1)
    a.beq("v0", "t0", "tid_ok")
    a.nop()
    puts(a, "sTidFail")
    a.label("tid_ok")
    puts(a, "sNL")
    a.li("a0", 0)
    syscall(a, SYS_EXIT)
    a.nop()

    a.func("worker")
    puts(a, "sB")
    a.la("t0", "sema_id")
    a.lw("a0", 0, "t0")
    syscall(a, 0x42)             # SignalSema -> main (prio 0) preempts
    puts(a, "sD")
    syscall(a, 0x23)             # ExitThread
    a.nop()

    a.func("worker2")
    puts(a, "sF")
    syscall(a, 0x32)             # SleepThread
    puts(a, "sH")
    a.jr("ra")                   # returning from the entry function exits
    a.nop()

    a.dalign(16)
    a.dlabel("semaparam")
    a.word(0, 1, 0, 0, 0, 0)
    a.dlabel("sema_id")
    a.word(0)
    a.dalign(16)
    a.dlabel("thparam1")
    a.word(0, "worker", "stack1", 0x4000, 0, 10, 10, 0, 0)
    a.dlabel("thparam2")
    a.word(0, "worker2", "stack2", 0x4000, 0, 5, 5, 0, 0)
    for ch in "ABCDEFGHI":
        a.dlabel(f"s{ch}")
        a.asciiz(ch)
    a.dlabel("sNL")
    a.asciiz("\n")
    a.dlabel("sPollFail")
    a.asciiz("[pollsema failed]")
    a.dlabel("sTidFail")
    a.asciiz("[threadid failed]")
    a.dalign(16)
    a.dlabel("stack1")
    a.space(0x4000)
    a.dlabel("stack2")
    a.space(0x4000)
    a.write_elf(path)


# =====================================================================
# Interrupts: vblank handler (polled busy loop), timer compare interrupts,
# alarms, and a thread blocked in WaitSema woken by iSignalSema.
# =====================================================================
def build_intr_test(path):
    a = Asm()
    k = Checker(a)
    prologue(a)
    a.li("a0", 2)
    a.la("a1", "vbl_handler")
    a.li("a2", 0)
    a.li("a3", 0)
    syscall(a, 0x10)             # AddIntcHandler(VBLANK_S)
    a.li("a0", 2)
    syscall(a, 0x14)             # EnableIntc
    a.la("s0", "counters")
    a.label("spin")
    a.lw("t0", 0, "s0")
    a.sltiu("t1", "t0", 5)
    a.bnez("t1", "spin")
    a.nop()
    k.eq("zero", 0, "vblank_count")
    a.li("a0", 9)
    a.la("a1", "tim_handler")
    a.li("a2", 0)
    a.li("a3", 0)
    syscall(a, 0x10)             # AddIntcHandler(TIM0)
    a.li("a0", 9)
    syscall(a, 0x14)
    a.li("t0", 0x10000000)
    a.sw("zero", 0, "t0")        # T0_COUNT = 0
    a.li("t1", 0x400)
    a.sw("t1", 0x20, "t0")       # T0_COMP
    a.li("t1", 0x2 | 0x40 | 0x80 | 0x100)   # BUSCLK/256, ZRET, CUE, CMPE
    a.sw("t1", 0x10, "t0")
    a.label("spin2")
    a.lw("t2", 4, "s0")
    a.sltiu("t1", "t2", 20)
    a.bnez("t1", "spin2")
    a.nop()
    a.li("t0", 0x10000000)
    a.sw("zero", 0x10, "t0")     # stop timer
    a.li("a0", 100)
    a.la("a1", "alarm_handler")
    a.li("a2", 0x55)
    syscall(a, 0xFC)             # SetAlarm(100 hsync, handler, 0x55)
    a.label("spin3")
    a.lw("t2", 8, "s0")
    a.beqz("t2", "spin3")
    a.nop()
    k.eq("t2", 0x55, "alarm_arg")
    a.la("a0", "semaparam")
    syscall(a, 0x40)
    a.la("t0", "sema_id")
    a.sw("v0", 0, "t0")
    a.li("s3", 3)
    a.label("waitloop")
    a.la("t0", "sema_id")
    a.lw("a0", 0, "t0")
    syscall(a, 0x44)             # WaitSema: woken by the vblank handler
    a.addiu("s3", "s3", -1)
    a.bnez("s3", "waitloop")
    a.nop()
    k.eq("s3", 0, "sema_from_handler")
    epilogue(a)

    a.func("vbl_handler")
    a.la("t0", "counters")
    a.lw("t1", 0, "t0")
    a.addiu("t1", "t1", 1)
    a.sw("t1", 0, "t0")
    a.la("t2", "sema_id")
    a.lw("a0", 0, "t2")
    a.beqz("a0", "vbl_out")
    a.nop()
    syscall(a, -0x43)            # iSignalSema
    a.label("vbl_out")
    a.jr("ra")
    a.li("v0", 0)

    a.func("tim_handler")
    a.la("t0", "counters")
    a.lw("t1", 4, "t0")
    a.addiu("t1", "t1", 1)
    a.sw("t1", 4, "t0")
    a.li("t0", 0x10000000)       # write back mode to clear EQUF
    a.lw("t1", 0x10, "t0")
    a.sw("t1", 0x10, "t0")
    a.jr("ra")
    a.li("v0", 0)

    a.func("alarm_handler")      # (id, time, arg)
    a.la("t0", "counters")
    a.jr("ra")
    a.sw("a2", 8, "t0")

    a.dalign(16)
    a.dlabel("counters")
    a.word(0, 0, 0, 0)
    a.dlabel("semaparam")
    a.word(0, 100, 0, 0, 0, 0)
    a.dlabel("sema_id")
    a.word(0)
    a.write_elf(path)


# =====================================================================
# GS: sprites, gouraud triangle, texture upload + textured sprite and alpha
# blending, sent through GIF DMA (PATH3); the frame is checked as a BMP.
# =====================================================================
GS = {"PRIM": 0x00, "RGBAQ": 0x01, "UV": 0x03, "XYZ2": 0x05, "TEX0_1": 0x06,
      "TEX1_1": 0x14, "XYOFFSET_1": 0x18, "PRMODECONT": 0x1A, "SCISSOR_1": 0x40,
      "ALPHA_1": 0x42, "TEST_1": 0x47, "FRAME_1": 0x4C, "ZBUF_1": 0x4E,
      "BITBLTBUF": 0x50, "TRXPOS": 0x51, "TRXREG": 0x52, "TRXDIR": 0x53}


def gif_tag(nloop, eop, flg, nreg, regs, pre=0, prim=0):
    lo = nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60)
    return lo, regs


def rgbaq(r, g, b, a=0x80):
    return r | (g << 8) | (b << 16) | (a << 24) | (0x3F800000 << 32)


def xyz2(x, y, z=0):
    return (x << 4) | ((y << 4) << 16) | (z << 32)


def build_gs_test(path):
    a = Asm()
    a.func("_start")
    a.li("sp", 0x01F00000)
    a.li("a0", 1)
    a.li("a1", 2)
    a.li("a2", 0)
    syscall(a, 0x02)             # SetGsCrt(interlaced, NTSC, field)
    a.li("s0", 0x12000000)
    a.li("t0", 0x0001 | (1 << 5) | (0xFF << 8))  # PMODE: EN1, MMOD, ALP
    a.sd("t0", 0x00, "s0")
    a.li("t0", 10 << 9)          # DISPFB1: FBP 0, FBW 10, PSMCT32
    a.sd("t0", 0x70, "s0")
    a.li("t0", 636 | (50 << 12) | (3 << 23))    # DISPLAY1: DX, DY, MAGH=3
    a.li("t1", 2559 | (447 << 12))              # DW, DH
    a.dsll32("t1", "t1", 0)
    getattr(a, "or_")("t0", "t0", "t1")
    a.sd("t0", 0x80, "s0")
    a.li("s1", 0x1000A000)       # GIF DMA channel
    a.la("t0", "gifpkt")
    a.sw("t0", 0x10, "s1")       # MADR
    a.la("t1", "gifpkt_end")
    a.subu("t1", "t1", "t0")
    a.srl("t1", "t1", 4)
    a.sw("t1", 0x20, "s1")       # QWC
    a.li("t0", 0x101)
    a.sw("t0", 0x00, "s1")       # CHCR: DIR | STR
    a.label("forever")
    a.b("forever")
    a.nop()

    ad = []

    def reg(name, value):
        ad.append((value, GS[name]))

    reg("FRAME_1", (10 << 16))
    reg("ZBUF_1", 0x70 | (1 << 32))
    reg("XYOFFSET_1", 0)
    reg("SCISSOR_1", (639 << 16) | (447 << 48))
    reg("TEST_1", (1 << 16) | (1 << 17))
    reg("PRMODECONT", 1)
    reg("PRIM", 6)
    reg("RGBAQ", rgbaq(0x20, 0x20, 0x80))
    reg("XYZ2", xyz2(0, 0))
    reg("XYZ2", xyz2(640, 448))
    reg("RGBAQ", rgbaq(255, 0, 0))
    reg("XYZ2", xyz2(100, 100))
    reg("XYZ2", xyz2(200, 200))
    reg("PRIM", 3 | (1 << 3))
    reg("RGBAQ", rgbaq(255, 0, 0))
    reg("XYZ2", xyz2(300, 100))
    reg("RGBAQ", rgbaq(0, 255, 0))
    reg("XYZ2", xyz2(500, 100))
    reg("RGBAQ", rgbaq(0, 0, 255))
    reg("XYZ2", xyz2(400, 300))
    reg("BITBLTBUF", (0x3000 << 32) | (1 << 48))
    reg("TRXPOS", 0)
    reg("TRXREG", 8 | (8 << 32))
    reg("TRXDIR", 0)
    pre = ad[:]
    ad.clear()
    reg("TEX0_1", 0x3000 | (1 << 14) | (3 << 26) | (3 << 30) | (1 << 34) | (1 << 35))
    reg("TEX1_1", 0)
    reg("PRIM", 6 | (1 << 4) | (1 << 8))
    reg("UV", 0)
    reg("XYZ2", xyz2(50, 300))
    reg("UV", (8 << 4) | ((8 << 4) << 16))
    reg("XYZ2", xyz2(114, 364))
    reg("ALPHA_1", (1 << 2) | (1 << 6))
    reg("PRIM", 6 | (1 << 6))
    reg("RGBAQ", rgbaq(255, 255, 0, 0x40))
    reg("XYZ2", xyz2(500, 300))
    reg("XYZ2", xyz2(600, 400))
    post = ad[:]

    def emit_ad(lst, eop):
        lo, hi = gif_tag(len(lst), eop, 0, 1, 0xE)
        a.dword(lo, hi)
        for v, r in lst:
            a.dword(v, r)

    a.dalign(16)
    a.dlabel("gifpkt")
    emit_ad(pre, 0)
    lo, hi = gif_tag(16, 0, 2, 0, 0)
    a.dword(lo, hi)
    for y in range(8):
        for x in range(8):
            a.word(0xFFFFFFFF if (x + y) & 1 else 0x80000000)
    emit_ad(post, 1)
    a.dlabel("gifpkt_end")
    a.write_elf(path)


def build_negative_test(path):
    """A deliberately failing check: the harness must report it."""
    a = Asm()
    k = Checker(a)
    prologue(a)
    a.li("t0", 1)
    k.eq("t0", 2, "deliberate")
    epilogue(a)
    a.write_elf(path)


# =====================================================================
# VU1: a hand-assembled micro program (uploaded with VIF1 MPG) reads a vertex
# and an offset, adds them (VADD), converts to 12.4 fixed point (FTOI4),
# writes the GIF packet back to VU1 memory and draws it with XGKICK (PATH1).
# Only meaningful on the PCSX2 backend (the soft backend has no VU1).
# =====================================================================
VU_NOP_UPPER = 0x000002FF
VU_NOP_LOWER = 0x8000033C
XYZW = 0xF << 21


def vu_upper_vadd(fd, fs, ft):
    return XYZW | (ft << 16) | (fs << 11) | (fd << 6) | 0x28


def vu_upper_ftoi4(ft, fs):
    idx = 0x15  # VFTOI4 in the SPECIAL2 table
    return XYZW | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3)


def vu_lower_lq(ft, imm, is_):
    return (0x00 << 25) | XYZW | (ft << 16) | (is_ << 11) | (imm & 0x7FF)


def vu_lower_sq(fs, imm, it):
    return (0x01 << 25) | XYZW | (it << 16) | (fs << 11) | (imm & 0x7FF)


def vu_lower_xgkick(is_):
    return 0x80000000 | (is_ << 11) | 0x6FC


def build_vu1_test(path):
    import struct as _st
    a = Asm()
    a.func("_start")
    a.li("sp", 0x01F00000)
    a.li("a0", 1)
    a.li("a1", 2)
    a.li("a2", 0)
    syscall(a, 0x02)
    a.li("s0", 0x12000000)
    a.li("t0", 0x0001 | (1 << 5) | (0xFF << 8))
    a.sd("t0", 0x00, "s0")
    a.li("t0", 10 << 9)
    a.sd("t0", 0x70, "s0")
    a.li("t0", 636 | (50 << 12) | (3 << 23))
    a.li("t1", 2559 | (447 << 12))
    a.dsll32("t1", "t1", 0)
    getattr(a, "or_")("t0", "t0", "t1")
    a.sd("t0", 0x80, "s0")
    # PATH3: frame setup + background clear
    a.li("s1", 0x1000A000)
    a.la("t0", "gifpkt")
    a.sw("t0", 0x10, "s1")
    a.la("t1", "gifpkt_end")
    a.subu("t1", "t1", "t0")
    a.srl("t1", "t1", 4)
    a.sw("t1", 0x20, "s1")
    a.li("t0", 0x101)
    a.sw("t0", 0x00, "s1")
    a.label("wait_gif")              # wait until the GIF DMA has finished
    a.lw("t0", 0x00, "s1")
    a.andi("t0", "t0", 0x100)
    a.bnez("t0", "wait_gif")
    a.nop()
    # VIF1 DMA: MPG + UNPACK + MSCAL
    a.li("s2", 0x10009000)
    a.la("t0", "vifpkt")
    a.sw("t0", 0x10, "s2")
    a.la("t1", "vifpkt_end")
    a.subu("t1", "t1", "t0")
    a.srl("t1", "t1", 4)
    a.sw("t1", 0x20, "s2")
    a.li("t0", 0x101)
    a.sw("t0", 0x00, "s2")
    a.label("forever")
    a.b("forever")
    a.nop()

    ad = []

    def reg(name, value):
        ad.append((value, GS[name]))

    reg("FRAME_1", (10 << 16))
    reg("ZBUF_1", 0x70 | (1 << 32))
    reg("XYOFFSET_1", 0)
    reg("SCISSOR_1", (639 << 16) | (447 << 48))
    reg("TEST_1", (1 << 16) | (1 << 17))
    reg("PRMODECONT", 1)
    reg("PRIM", 6)
    reg("RGBAQ", rgbaq(0x20, 0x20, 0x80))
    reg("XYZ2", xyz2(0, 0))
    reg("XYZ2", xyz2(640, 448))
    a.dalign(16)
    a.dlabel("gifpkt")
    lo, hi = gif_tag(len(ad), 1, 0, 1, 0xE)
    a.dword(lo, hi)
    for v, r in ad:
        a.dword(v, r)
    a.dlabel("gifpkt_end")

    # --- VU1 micro program (lower word first, then upper)
    prog = [
        (vu_lower_lq(1, 3, 0), VU_NOP_UPPER),      # LQ vf01, 3(vi00)   vertex
        (vu_lower_lq(2, 5, 0), VU_NOP_UPPER),      # LQ vf02, 5(vi00)   offset
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, vu_upper_vadd(3, 1, 2)),    # VADD vf03, vf01, vf02
        (VU_NOP_LOWER, vu_upper_ftoi4(4, 1)),      # FTOI4 vf04, vf01
        (VU_NOP_LOWER, vu_upper_ftoi4(5, 3)),      # FTOI4 vf05, vf03
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (vu_lower_sq(4, 3, 0), VU_NOP_UPPER),      # SQ vf04, 3(vi00)
        (vu_lower_sq(5, 4, 0), VU_NOP_UPPER),      # SQ vf05, 4(vi00)
        (vu_lower_xgkick(0), VU_NOP_UPPER),        # XGKICK vi00
        (VU_NOP_LOWER, VU_NOP_UPPER | 0x40000000), # [E]
        (VU_NOP_LOWER, VU_NOP_UPPER),
    ]

    def f(x):
        return _st.unpack("<I", _st.pack("<f", x))[0]

    vu_data = [
        list(gif_tag(1, 1, 0, 4, 0x5510)),          # GIF tag: PRIM, RGBAQ, XYZ2, XYZ2
        [6, 0],                                     # PRIM: sprite
        [0, 0],                                     # RGBAQ (filled below)
        [0, 0], [0, 0], [0, 0],
    ]
    a.dalign(16)
    a.dlabel("vifpkt")
    words = []
    words.append((0x4A << 24) | (len(prog) << 16) | 0)        # MPG num, addr 0
    for lo_w, hi_w in prog:
        words += [lo_w, hi_w]
    words.append((0x01 << 24) | 0x0101)                       # STCYCL 1,1
    words.append((0x6C << 24) | (6 << 16) | 0)                # UNPACK V4-32 x6 @0
    tag_lo, tag_hi = vu_data[0]
    words += [tag_lo & 0xFFFFFFFF, tag_lo >> 32, tag_hi & 0xFFFFFFFF, tag_hi >> 32]
    words += [6, 0, 0, 0]                                     # PRIM sprite
    words += [0, 255, 0, 0x80]                                # RGBAQ green
    words += [f(300.0), f(250.0), 0, 0]                       # vertex (floats)
    words += [0, 0, 0, 0]                                     # second vertex (computed)
    words += [f(100.0), f(80.0), 0, 0]                        # offset
    words.append((0x14 << 24) | 0)                            # MSCAL 0
    while len(words) % 4:
        words.append(0)                                       # VIF NOP padding
    a.word(*words)
    a.dlabel("vifpkt_end")
    a.write_elf(path)


# =====================================================================
# VU0 micro mode: VIF0 uploads a micro program, the EE starts it with
# VCALLMS and reads the results back through COP2 (QMFC2 / CFC2).
# =====================================================================
def vu_lower_iaddiu(it, is_, imm):
    return (0x08 << 25) | (((imm >> 11) & 0xF) << 21) | (it << 16) | (is_ << 11) | (imm & 0x7FF)


def build_vu0_test(path):
    from programs import f2u
    a = Asm()
    k = Checker(a)
    prologue(a)
    a.li("s2", 0x10008000)           # VIF0 DMA channel
    a.la("t0", "vifpkt")
    a.sw("t0", 0x10, "s2")
    a.la("t1", "vifpkt_end")
    a.subu("t1", "t1", "t0")
    a.srl("t1", "t1", 4)
    a.sw("t1", 0x20, "s2")
    a.li("t0", 0x101)
    a.sw("t0", 0x00, "s2")
    a.label("wait_vif0")
    a.lw("t0", 0x00, "s2")
    a.andi("t0", "t0", 0x100)
    a.bnez("t0", "wait_vif0")
    a.nop()
    a.la("s1", "vdata")
    a.lqc2("vf1", 0, "s1")
    a.lqc2("vf2", 16, "s1")
    a.emit((18 << 26) | (1 << 25) | (0 << 6) | 0x38)      # VCALLMS 0
    a.qmfc2("t0", "vf3")
    k.eq128("t0", [f2u(11), f2u(22), f2u(33), f2u(44)], "vcallms_vadd")
    a.cfc2("t1", 1)                                        # vi01
    k.eq("t1", 5, "vcallms_iaddiu")
    epilogue(a)

    prog = [
        (vu_lower_iaddiu(1, 0, 5), vu_upper_vadd(3, 1, 2)),   # IADDIU vi01,vi00,5 | VADD vf03
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER),
        (VU_NOP_LOWER, VU_NOP_UPPER | 0x40000000),            # [E]
        (VU_NOP_LOWER, VU_NOP_UPPER),
    ]
    words = [(0x4A << 24) | (len(prog) << 16) | 0]
    for lo_w, hi_w in prog:
        words += [lo_w, hi_w]
    while len(words) % 4:
        words.append(0)
    a.dalign(16)
    a.dlabel("vifpkt")
    a.word(*words)
    a.dlabel("vifpkt_end")
    a.dalign(16)
    a.dlabel("vdata")
    a.floats(1, 2, 3, 4, 10, 20, 30, 40)
    a.write_elf(path)


# =====================================================================
# Fake return address (S0-B1): a function sets ra to a label inside itself
# and tail-loops to its own entry, like SH3's quicksorts at 0x139CC0/0x139EB0
# (0x139E98: lui/addiu ra,0x139D48; b 0x139CC0).  `jr ra` must jump to the
# in-function epilogue, not return to the C caller.
#   fsort(a0 = depth) on the private stack s6:
#     entry: if a0 == 0: jr ra
#            push ra; a0--; if a0 == 0 goto pop; ra = &pop; b entry
#     pop:   s1++; pop ra; jr ra
# With a0 = 2 the frame is nested once: pop runs twice and s6 is restored.
# =====================================================================
def build_fake_ra_test(path):
    a = Asm()
    k = Checker(a)
    prologue(a)
    a.la("s6", "fake_ra_stack_top")
    a.move("s5", "s6")
    a.li("s1", 0)
    a.li("a0", 2)
    a.jal("fsort")
    a.nop()
    k.eq("s1", 2, "fake_ra_pops")
    a.subu("t0", "s6", "s5")
    k.eq("t0", 0, "fake_ra_stack_balanced")
    a.li("s1", 0)
    a.li("a0", 0)                    # no frame: plain return
    a.jal("fsort")
    a.nop()
    k.eq("s1", 0, "fake_ra_leaf")
    epilogue(a)

    a.func("fsort")
    a.beqz("a0", "fsort_ret")
    a.nop()
    a.addiu("s6", "s6", -16)
    a.sw("ra", 0, "s6")
    a.addiu("a0", "a0", -1)
    a.beqz("a0", "fsort_pop")        # nothing left: straight to the epilogue
    a.nop()                          # (keeps fsort_pop inside fsort, as at 0x139E90)
    a.la_addiu("ra", "fsort_pop")    # fake return address inside this function
    a.b("fsort")
    a.nop()
    a.label("fsort_pop")
    a.addiu("s1", "s1", 1)
    a.lw("ra", 0, "s6")
    a.jr("ra")
    a.addiu("s6", "s6", 16)
    a.label("fsort_ret")
    a.jr("ra")
    a.nop()

    a.dalign(16)
    a.space(256)
    a.dlabel("fake_ra_stack_top")
    a.space(16)
    a.write_elf(path)


# =====================================================================
# VU0 macro mode MAC / STATUS / FDIV flags (S0-B4).  PCSX2 microVU macro mode
# updates them right after each op (microVU_Macro.inl setupMacroOp mode 0x10);
# SH3 reads them with cfc2 vi16/vi17 after ctc2 zero,vi16 + vsub (cull tests).
# =====================================================================
def _vu_fdiv(name, fs, fsf, ft, ftf):
    idx = {"vdiv": 0x38, "vsqrt": 0x39, "vrsqrt": 0x3A}[name]
    return ((18 << 26) | (1 << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11)
            | ((idx >> 2) << 6) | 0x3C | (idx & 3))


def build_vu0_flags_test(path):
    a = Asm()
    k = Checker(a)
    prologue(a)
    a.la("s1", "vfdata")
    a.lqc2("vf1", 0, "s1")               # 1 2 3 4
    a.lqc2("vf2", 16, "s1")              # 2 2 1 4
    # vsub with a negative x, a zero y and w: MAC = Sx|Zy|Zw, STATUS = Z|S|ZS|SS
    a.ctc2("zero", 16)
    a.vsub_xyzw("vf3", "vf1", "vf2")     # (-1, 0, 2, 0)
    a.cfc2("t0", 17)
    k.eq("t0", 0x85, "vsub_mac")
    a.cfc2("t0", 16)
    k.eq("t0", 0xC3, "vsub_status")
    # only x written and positive: MAC 0, Z/S cleared, sticky ZS/SS kept
    a.vadd_x("vf4", "vf1", "vf1")
    a.cfc2("t0", 17)
    k.eq("t0", 0, "vadd_x_mac")
    a.cfc2("t0", 16)
    k.eq("t0", 0xC0, "vadd_x_status_sticky")
    # ctc2 vi16 writes only the sticky bits
    a.vsub_xyzw("vf3", "vf1", "vf2")     # Z S ZS SS again
    a.ctc2("zero", 16)
    a.cfc2("t0", 16)
    k.eq("t0", 0x03, "ctc2_keeps_nonsticky")
    a.li("t1", 0xFFF)
    a.ctc2("t1", 16)
    a.cfc2("t0", 16)
    k.eq("t0", 0xFC3, "ctc2_writes_sticky")
    # vdiv by zero sets D|DS; a normal vdiv clears D, DS stays
    a.ctc2("zero", 16)
    a.emit(_vu_fdiv("vdiv", 1, 0, 0, 0))   # 1 / vf0.x (0)
    a.cfc2("t0", 16)
    a.andi("t0", "t0", 0xC30)
    k.eq("t0", 0x820, "vdiv0_D")
    a.emit(_vu_fdiv("vdiv", 1, 0, 2, 1))   # 1 / 2
    a.cfc2("t0", 16)
    a.andi("t0", "t0", 0xC30)
    k.eq("t0", 0x800, "vdiv_clears_D")
    epilogue(a)
    a.dalign(16)
    a.dlabel("vfdata")
    a.floats(1, 2, 3, 4, 2, 2, 1, 4)
    a.write_elf(path)
