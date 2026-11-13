/*
 * libmpeg (SDK MPEG-2 / IPU library) native replacement: FMV without the IPU.
 *
 * Native mode (PS2NATIVE_MPEG=native):
 *   - DMA channel 4 (toIPU) does not reach the IPU: the backend calls
 *     mpeg_ipu_dma_start(), which walks the transfer as the DMAC would,
 *     queues the bytes (the MPEG-2 video elementary stream) for the native
 *     decoder (mpeg_native.c, FFmpeg) and reports the transfer finished, so
 *     the game's ring buffer accounting runs unchanged.
 *   - sceMpegGetPictureRAW8 decodes the next picture natively; while none is
 *     ready it calls the game's "no data" callback (registered through
 *     sceMpegAddCallback), which is what starts the next channel 4 transfer.
 *   - sceMpegIsEnd / sceMpegReset follow the native decoder.
 *
 * Observation (PS2NATIVE_MPEG_DUMP=<dir>, real IPU): saves the RAW8 pictures
 * of the original library (pic_NNNNN.raw + pics.log) - the reference the
 * native decoder was checked against (mean abs. difference 0.11/255 over 170
 * pictures of the SH3 intro, see NATIVE_PLAN.md).
 *
 * sceMpeg header (SDK libmpeg.h; same offsets as ran-j/PS2Recomp):
 *   +0x00 width, +0x04 height, +0x08 frameCount, +0x10 pts, +0x18 dts,
 *   +0x20 flags, +0x28 pts2, +0x30 dts2, +0x38 flags2, +0x40 sys (private).
 * Values measured with the real library on the SH3 intro (512x384, 29.97):
 *   pts = first + 3003 * n (display order); dts = pts of the previous anchor
 *   for I/P pictures, -1 for B; flags = 0x1D8 | picture_coding_type.
 */
#include "../internal.h"
#include "mpeg_native.h"
#include <windows.h>

/* ------------------------------------------------------------ modes */
static const char *dump_dir(void)
{
    static int init;
    static const char *dir;
    if (!init) {
        init = 1;
        dir = getenv("PS2NATIVE_MPEG_DUMP");
        if (dir && !dir[0])
            dir = NULL;
    }
    return dir;
}

static int native_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PS2NATIVE_MPEG");
        mode = e && !strcmp(e, "native") && !dump_dir();
        if (mode)
            ps2_log("mpeg: native MPEG-2 decoding (no IPU)");
    }
    return mode;
}

/* ------------------------------------------------------------ native state */
static mpeg_native *dec;
static uint8_t *es;                 /* elementary stream bytes not decoded yet */
static size_t es_len, es_cap;
static uint32_t es_total;           /* bytes received from channel 4 */
static int seq_end;                 /* sequence_end_code (00 00 01 B7) seen */
static uint32_t tail;               /* last 3 bytes, to find start codes across chunks */
static uint32_t pictures;
static uint32_t cb_func[8], cb_data[8];
static int64_t last_anchor = -1;   /* pts of the last I/P picture */

static void es_push(const uint8_t *p, size_t n)
{
    if (es_len + n > es_cap) {
        es_cap = (es_len + n) * 2 + 65536;
        es = realloc(es, es_cap);
    }
    memcpy(es + es_len, p, n);
    es_len += n;
    es_total += (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        tail = (tail << 8) | p[i];
        if (tail == 0x000001B7u)
            seq_end = 1;
    }
}

static void decode_some(void)
{
    if (!dec)
        dec = mpn_create();
    if (!dec)
        return;
    while (es_len && !mpn_has_picture(dec)) {
        size_t used = mpn_feed(dec, es, es_len);
        memmove(es, es + used, es_len - used);
        es_len -= used;
        if (!used)
            break;
    }
    if (!es_len && seq_end && !mpn_has_picture(dec))
        mpn_flush(dec);   /* the last pictures the decoder holds back */
}

static void native_reset(void)
{
    if (dec)
        mpn_reset(dec);
    es_len = 0;
    seq_end = 0;
    tail = 0xFFFFFFFFu;
    pictures = 0;
    last_anchor = -1;
}

/* ------------------------------------------------------------ channel 4 */
/* Copies one block of a transfer to the dump file or the decoder queue. */
static void take(FILE *f, uint32_t addr, uint32_t qwc)
{
    uint8_t buf[16];
    for (uint32_t i = 0; i < qwc; i++) {
        ps2_mem_read((addr & 0x1FFFFFF0u) + i * 16, buf, 16);
        if (f)
            fwrite(buf, 1, 16, f);
        else
            es_push(buf, 16);
    }
}

/* Called by the backend when the game starts DMA channel 4 (toIPU).  In native
 * mode the whole transfer is consumed here; returns 1 and the register values
 * the channel has once the transfer is over (the backend then signals the
 * end of the transfer instead of starting the real IPU DMA). */
int mpeg_ipu_dma_start(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr,
                       uint32_t *end_madr, uint32_t *end_tadr, uint32_t *end_tag)
{
    const char *dir = dump_dir();
    int native = native_mode();
    if (!dir && !native)
        return 0;
    FILE *f = NULL;
    if (dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/ipu_in.log", dir);
        FILE *lf = fopen(path, "a");
        if (lf) {
            fprintf(lf, "chcr %08X madr %08X qwc %u tadr %08X\n", chcr, madr, qwc, tadr);
            fclose(lf);
        }
        snprintf(path, sizeof path, "%s/ipu_in.bin", dir);
        f = fopen(path, "ab");
        if (!f)
            return 0;
    }
    take(f, madr, qwc);   /* normal mode, or the pending block of a chain */
    madr += qwc * 16;
    uint32_t t = tadr, tag_hi = chcr & 0xFFFF0000u;
    if (((chcr >> 2) & 3) == 1) {
        /* Source chain (PCSX2 Dmac.h tDMA_TAG: QWC 0-15, ID 28-30, IRQ 31;
         * ADDR in the second word), walked as the DMAC does it. */
        for (int n = 0; n < 4096; n++) {
            uint32_t tagaddr = t & 0x1FFFFFF0u;
            uint32_t tag = ps2_read32(tagaddr), addr = ps2_read32(tagaddr + 4) & 0x7FFFFFF0u;
            uint32_t tq = tag & 0xFFFF, id = (tag >> 28) & 7;
            int end = 0;
            tag_hi = tag & 0xFFFF0000u;
            switch (id) {
            case 0: take(f, addr, tq); madr = addr + tq * 16; t = tagaddr + 16; end = 1; break; /* REFE */
            case 1: take(f, tagaddr + 16, tq); madr = t = tagaddr + 16 + tq * 16; break;     /* CNT */
            case 2: take(f, tagaddr + 16, tq); madr = tagaddr + 16 + tq * 16; t = addr; break; /* NEXT */
            case 3: case 4: take(f, addr, tq); madr = addr + tq * 16; t = tagaddr + 16; break; /* REF(S) */
            case 7: take(f, tagaddr + 16, tq); madr = tagaddr + 16 + tq * 16; end = 1; break;  /* END */
            default: end = 1; break;                                                          /* CALL/RET */
            }
            if (end || ((tag & 0x80000000u) && (chcr & 0x80u)))   /* IRQ tag with TIE */
                break;
        }
    }
    if (f)
        fclose(f);
    if (native && ps2_verbose)
        ps2_log("mpeg: ch4 chcr %08X madr %08X qwc %u tadr %08X -> %u bytes queued (end madr %08X tadr %08X)",
                chcr, madr, qwc, tadr, (unsigned)es_len, madr, t);
    *end_madr = madr;
    *end_tadr = t;
    *end_tag = tag_hi;
    return native;
}

/* ------------------------------------------------------------ library */
/* sceMpegAddCallback(sceMpeg *mp, int type, func, void *data): remember the
 * callbacks, then let the library register them as usual. */
int hle_sceMpegAddCallback(R5900 *c)
{
    uint32_t type = c->r[5].UW[0];
    if (type < 8) {
        cb_func[type] = c->r[6].UW[0];
        cb_data[type] = c->r[7].UW[0];
    }
    if (ps2_verbose || native_mode())
        ps2_log("mpeg: callback type %u -> %08X (data %08X)", type, c->r[6].UW[0], c->r[7].UW[0]);
    return 0;
}

int hle_sceMpegReset(R5900 *c)
{
    if (!native_mode())
        return 0;
    native_reset();
    ps2_hle_original(c, "sceMpegReset");
    return 1;
}

int hle_sceMpegIsEnd(R5900 *c)
{
    if (!native_mode())
        return 0;
    decode_some();
    int end = seq_end && !es_len && !(dec && mpn_has_picture(dec));
    c->r[2].SD[0] = end;
    return 1;
}

static void dump_picture(uint32_t mp, uint32_t out, uint32_t mbcount, int ret)
{
    const char *dir = dump_dir();
    static int n;
    static uint32_t prev_out, prev_mbs;
    char path[1024];
    /* The previous picture is complete by the time the game asks for the
     * next one (the IPU output may still be running when the call returns). */
    if (prev_out) {
        snprintf(path, sizeof path, "%s/pic_%05d.raw", dir, n - 1);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t mb[384];
            for (uint32_t i = 0; i < prev_mbs; i++) {
                ps2_mem_read((prev_out & 0x1FFFFFFFu) + i * 384, mb, 384);
                fwrite(mb, 1, 384, f);
            }
            fclose(f);
        }
    }
    prev_out = out;
    prev_mbs = mbcount;
    snprintf(path, sizeof path, "%s/pics.log", dir);
    FILE *lf = fopen(path, "a");
    if (lf) {
        fprintf(lf, "pic %d ret %d out %08X mbs %u | w %u h %u count %u pts %08X%08X dts %08X%08X flags %08X\n",
                n, ret, out, mbcount, ps2_read32(mp), ps2_read32(mp + 4), ps2_read32(mp + 8),
                ps2_read32(mp + 0x14), ps2_read32(mp + 0x10), ps2_read32(mp + 0x1C), ps2_read32(mp + 0x18),
                ps2_read32(mp + 0x20));
        fclose(lf);
    }
    n++;
}

/* sceMpegGetPictureRAW8(sceMpeg *mp, sceIpuRAW8 *buf, int mbcount) */
int hle_sceMpegGetPictureRAW8(R5900 *c)
{
    uint32_t mp = c->r[4].UW[0], out = c->r[5].UW[0], mbcount = c->r[6].UW[0];
    if (dump_dir()) {
        ps2_hle_original(c, "sceMpegGetPictureRAW8");
        static uint32_t obs_n;
        if (++obs_n % 100 == 0)
            ps2_log("mpeg: %u pictures (real IPU)", obs_n);
        dump_picture(mp, out, mbcount, c->r[2].SW[0]);
        return 1;
    }
    if (!native_mode()) {
        static uint32_t real_n;
        if (++real_n % 100 == 0 && ps2_verbose)
            ps2_log("mpeg: %u pictures (real IPU)", real_n);
        return 0;
    }
    static uint8_t raw[1024 * 384];
    int idle = 0;
    /* Only the "no data" callback is needed: the real library calls the
     * stopDMA/restartDMA pair back to back while it sends its own data to
     * the IPU (measured on SH3), which the native decoder never does. */
    for (;;) {
        decode_some();
        if (dec && mpn_has_picture(dec))
            break;
        if (seq_end && !es_len) {   /* nothing left */
            c->r[2].SD[0] = -1;
            return 1;
        }
        uint32_t before = es_total;
        if (cb_func[1])   /* sceMpegCbNodata: the game feeds channel 4 */
            ps2_hle_call_guest(c, cb_func[1], mp, 0, cb_data[1], 0);
        if (es_total != before) {
            idle = 0;
            continue;
        }
        /* The ring is waiting for the disc: let (wall-clock) time pass and
         * run the hardware events, as the real library does while it polls
         * the IPU.  The callback's syscall delivers the interrupts. */
        double t0 = ps2_host_time();
        Sleep(0);
        be_update();
        static double waited;
        waited = idle ? waited + (ps2_host_time() - t0) : 0;
        idle = 1;
        if (waited > 15.0) {   /* 15 s without data */
            ps2_log("mpeg: no video data, giving up on this picture (cb %08X, %u bytes so far)", cb_func[1], es_total);
            c->r[2].SD[0] = -1;
            return 1;
        }
    }
    int w = 0, h = 0;
    int max = mbcount < 1024 ? (int)mbcount : 1024;
    int n = mpn_take_raw8(dec, raw, max, &w, &h);
    ps2_mem_write(out, raw, (uint32_t)n * 384);
    /* Header fields as the real library sets them (measured, see top). */
    const int64_t first_pts = 0x11DA;   /* SH3 intro; the game does not read pts */
    int64_t pts = first_pts + 3003 * (int64_t)pictures;
    int type = mpn_last_picture_type(dec);   /* 1 I, 2 P, 3 B */
    int64_t dts = type == 3 ? -1 : (last_anchor >= 0 ? last_anchor : pts - 3003);
    if (type != 3)
        last_anchor = pts;
    ps2_write32(mp + 0x00, (uint32_t)w);
    ps2_write32(mp + 0x04, (uint32_t)h);
    ps2_write32(mp + 0x08, pictures);
    ps2_write32(mp + 0x10, (uint32_t)pts);
    ps2_write32(mp + 0x14, (uint32_t)(pts >> 32));
    ps2_write32(mp + 0x18, (uint32_t)dts);
    ps2_write32(mp + 0x1C, (uint32_t)(dts >> 32));
    ps2_write32(mp + 0x20, 0x1D8u | (uint32_t)type);
    ps2_write32(mp + 0x24, 0);
    pictures++;
    if (pictures % 100 == 0)
        ps2_log("mpeg: %u pictures", pictures);
    c->r[2].SD[0] = 1;
    return 1;
}
