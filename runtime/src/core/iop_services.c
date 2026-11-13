/*
 * HLE IOP RPC services beyond fileio/iopheap/loadfile (see sif.c).
 *
 * Replies are what the real IOP answered in the LLE reference run
 * (PS2NATIVE_RPCLOG=1, see NATIVE_PLAN.md "Measured RPC traffic").
 */
#include "../internal.h"

typedef int32_t (*server_fn)(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply,
                             uint32_t *reply_size);

/* SID 0x57: Silent Hill 3 sound driver (iop_sound/sd_call.irx).
 *   fno 0x02000000 {"\SD.BIN;1"}: load the sound bank        -> zeros
 *   fno 0x01000000 {cmd, arg}[n]: command list; reply word 1 = status
 * Measured with the real IOP (NATIVE_PLAN.md):
 *   low byte: 0 -> 3 -> 7 while SD.BIN loads; the game waits for 7.
 *   cmd 0x1404TTSS {iop addr}: load bank SS of type TT (data already DMA'd);
 *     sets bit 0x10 << SS (TT = 1) or 0x100 << SS (TT = 2): 0x207, 0x307,
 *     0x707, 0x727, 0x737, 0x777.  The game waits for the bit.
 *   cmd 0x140FTTSS: release (clears the bit).  0x19 = no-op / status.
 * No audio yet: every command completes immediately. */
static int32_t srv_sh3_sound(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply,
                             uint32_t *rsz)
{
    static uint32_t status = 7;
    if (fno == 0x01000000u) {
        for (uint32_t o = 0; o + 8 <= ssize; o += 8) {
            uint32_t cmd = rd32le(arg + o);
            if (ps2_cfg.trace_syscalls && cmd != 0x19000000u)
                ps2_log("sh3 sound cmd %08X %08X (status %X)", cmd, rd32le(arg + o + 4), status);
            if ((cmd >> 24) != 0x14)
                continue;
            uint32_t op = (cmd >> 16) & 0xFF, type = (cmd >> 8) & 0xFF, slot = cmd & 0xFF;
            uint32_t bit = type == 1 ? (0x10u << slot) : type == 2 ? (0x100u << slot) : 0;
            if (slot > 3 || !bit)
                continue;
            if (op == 0x04)
                status |= bit;
            else if (op == 0x0F)
                status &= ~bit;
        }
        wr32le(reply + 4, status);
        wr32le(reply + 24, 0xFF);
    }
    *rsz = 32;
    return 0;
}

/* CRI DTX: EE<->IOP data exchange of the CRI middleware (decompiled from SH3,
 * 0x2050B0 create / 0x2052D0 send-receive).  A channel is an EE buffer of
 * wklen bytes; its last 64 bytes are a trailer whose word at +0x3C is a
 * sequence number.  The EE fills the buffer, stores seq+1 in the trailer and
 * SIF-DMAs it to the IOP buffer, then waits until the IOP has DMA'd its own
 * buffer back with a larger sequence number and hands that buffer to the
 * receive callback.  HLE answer: an empty (zeroed) reply with seq+1. */
#define MAX_DTX 8
static struct { uint32_t ee, iop, wklen; } dtx[MAX_DTX];

static void dtx_register(uint32_t id, uint32_t ee, uint32_t iop, uint32_t wklen)
{
    if (id < MAX_DTX) {
        dtx[id].ee = ee & 0x1FFFFFFFu;
        dtx[id].iop = iop;
        dtx[id].wklen = wklen;
    }
}

int iop_dtx_dma(uint32_t ee_src, uint32_t iop_dest, uint32_t size)
{
    (void)size;
    for (int i = 0; i < MAX_DTX; i++) {
        if (!dtx[i].wklen || dtx[i].iop != iop_dest)
            continue;
        uint32_t ee = dtx[i].ee, data = dtx[i].wklen - 0x40;
        (void)ee_src;
        uint32_t seq = ps2_read32(ee + data + 0x3C);
        static uint8_t zero[0x10000];
        ps2_mem_write(ee, zero, data < sizeof zero ? data : sizeof zero);
        ps2_write32(ee + data + 0x3C, seq + 1);
        return 1;
    }
    return 0;
}

/* SID 0x7D000000: CRI ADX (iop_sound/cri_adxi.irx).  Measured replies:
 *   fno 2      init {0, ee work, iop work, size}   -> IOP address (0 makes the
 *              game print an error and loop forever, see 0x20DD78)
 *   fno 0x422  create stream {1, iop buf, 0x100}   -> handle (IOP address)
 *   fno 0x400  create player {ee, handle, 1, ee}   -> handle (IOP address)
 *   fno 0x408  create ADX handle {2, 0, sj, sj}     -> handle (0: "Can't create
 *              ADX handle" and an endless loop, see 0x1E2830)
 *   others (0x403, 0x40A, ...)                      -> 0
 * Handles only need to be distinct and non-zero; no audio yet. */
static int32_t srv_cri_adx(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply,
                           uint32_t *rsz)
{
    (void)ssize;
    static uint32_t next = 0x000D0000u;
    if (fno == 2)   /* DTX init {id, ee buf, iop buf, wklen} */
        dtx_register(rd32le(arg), rd32le(arg + 4), rd32le(arg + 8), rd32le(arg + 12));
    uint32_t r = 0;
    if (fno == 2 || fno == 0x422 || fno == 0x400 || fno == 0x408) {
        r = next;
        next += 0x40;
    }
    wr32le(reply, r);
    *rsz = 4;
    if (ps2_cfg.trace_syscalls)
        ps2_log("adx rpc fno=%X args %08X %08X %08X %08X -> %08X", fno, rd32le(arg), rd32le(arg + 4),
                rd32le(arg + 8), rd32le(arg + 12), r);
    return (int32_t)r;
}

server_fn iop_service_for_sid(uint32_t sid)
{
    switch (sid) {
    case 0x00000057u: return srv_sh3_sound;
    case 0x7D000000u: return srv_cri_adx;
    default: return NULL;
    }
}
