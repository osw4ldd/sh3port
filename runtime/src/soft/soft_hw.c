/*
 * EE hardware registers (0x10000000-0x1000FFFF): timers, INTC, DMAC,
 * GIF/VIF control, SIO console and SIF mailbox registers; plus the
 * real-time event model (vblank, timer interrupts).
 */
#include "soft.h"

uint32_t hw_intc_stat, hw_intc_mask;
uint32_t hw_dmac_stat;
static uint32_t dmac_ctrl = 1, dmac_pcr, dmac_sqwc, dmac_rbsr, dmac_rbor, dmac_stadr;
static uint32_t dmac_enable = 0x1201;
uint64_t hw_vblank_count;

/* DMA channel registers */
dma_chan hw_dma[10];
static const uint32_t dma_base[10] = {
    0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

/* ------------------------------------------------------------ timers */
typedef struct timer {
    uint32_t mode, comp, hold;
    double t0;        /* host time of last update */
    double count;     /* count at t0 */
} timer;
static timer timers[4];

static double timer_rate(const timer *t)
{
    switch (t->mode & 3) {
    case 0: return 147456000.0;
    case 1: return 147456000.0 / 16.0;
    case 2: return 147456000.0 / 256.0;
    default: return 15734.0;   /* HBLANK (NTSC) */
    }
}

int hw_timer_running(int n) { return (timers[n].mode & 0x80) != 0; }

static void timer_advance(int n, double now)
{
    timer *t = &timers[n];
    if (!(t->mode & 0x80)) {
        t->t0 = now;
        return;
    }
    double rate = timer_rate(t);
    double remaining = (now - t->t0) * rate;
    for (int guard = 0; guard < 64 && remaining > 0; guard++) {
        double cmp = (double)(t->comp & 0xFFFF);
        double to_cmp = cmp > t->count ? cmp - t->count : 1e30;
        double to_ovf = 65536.0 - t->count;
        double step = to_cmp < to_ovf ? to_cmp : to_ovf;
        if (remaining < step) {
            t->count += remaining;
            remaining = 0;
            break;
        }
        remaining -= step;
        if (step == to_cmp) {
            t->count = cmp;
            t->mode |= 0x400;             /* EQUF */
            if (t->mode & 0x100)
                hw_raise_intc(INTC_TIM0 + n);
            if (t->mode & 0x40)           /* ZRET */
                t->count = 0;
        } else {
            t->count = 0;
            t->mode |= 0x800;             /* OVFF */
            if (t->mode & 0x200)
                hw_raise_intc(INTC_TIM0 + n);
        }
    }
    if (remaining > 0)  /* fell far behind: drop the missed events */
        t->count = fmod(t->count + remaining, 65536.0);
    t->t0 = now;
}

static double timer_next_event(int n)
{
    timer *t = &timers[n];
    if (!(t->mode & 0x80) || !(t->mode & 0x300))
        return 1e30;
    double rate = timer_rate(t);
    double cmp = (double)(t->comp & 0xFFFF);
    double ticks = 1e30;
    if (t->mode & 0x200)
        ticks = 65536.0 - t->count;
    if (t->mode & 0x100) {
        double to_cmp = cmp > t->count ? cmp - t->count : 65536.0 - t->count + cmp;
        if (to_cmp < ticks)
            ticks = to_cmp;
    }
    return t->t0 + ticks / rate;
}

/* ------------------------------------------------------------ vblank */
static double vblank_rate = 59.94;
static double vblank_epoch;
static uint64_t vblank_seen;
static int vblank_end_pending;
static double vblank_end_time;

void hw_set_video_rate(int pal)
{
    double now = ps2_host_time();
    vblank_rate = pal ? 50.0 : 59.94;
    vblank_epoch = now;
    vblank_seen = 0;
}

void hw_update_time(void)
{
    double now = ps2_host_time();
    for (int i = 0; i < 4; i++)
        timer_advance(i, now);

    if (vblank_end_pending && now >= vblank_end_time) {
        vblank_end_pending = 0;
        gs_vblank(0);
        hw_raise_intc(INTC_VBLANK_E);
    }
    uint64_t frames = (uint64_t)((now - vblank_epoch) * vblank_rate);
    if (frames > vblank_seen) {
        /* Deliver a single vblank even if several were missed. */
        vblank_seen = frames;
        hw_vblank_count++;
        gs_vblank(1);
        hw_raise_intc(INTC_VBLANK_S);
        vblank_end_pending = 1;
        vblank_end_time = now + 0.0014;
        display_pump();
        kernel_on_vblank();
    }
}

double hw_next_event_time(void)
{
    double next = vblank_epoch + (double)(vblank_seen + 1) / vblank_rate;
    if (vblank_end_pending && vblank_end_time < next)
        next = vblank_end_time;
    for (int i = 0; i < 4; i++) {
        double t = timer_next_event(i);
        if (t < next)
            next = t;
    }
    return next;
}

void hw_raise_intc(int cause)
{
    hw_intc_stat |= 1u << cause;
    if (hw_intc_mask & (1u << cause))
        ps2_event_flag = 1;
}

void hw_dma_complete(int channel)
{
    hw_dmac_stat |= 1u << channel;
    if (hw_dmac_stat & (1u << (16 + channel)))
        ps2_event_flag = 1;
}

void hw_init(void)
{
    double now = ps2_host_time();
    memset(timers, 0, sizeof timers);
    for (int i = 0; i < 4; i++)
        timers[i].t0 = now;
    /* T3 is owned by the kernel's alarm system; report it as configured so
     * ps2sdk does not try to patch the kernel. */
    timers[3].mode = 0x0103;
    vblank_epoch = now;
}

/* --------------------------------------------------------- registers */
static int dma_index(uint32_t a)
{
    for (int i = 0; i < 10; i++)
        if ((a & ~0xFFu) == dma_base[i])
            return i;
    return -1;
}

static uint32_t sio_line_pos;

uint32_t hw_read32(uint32_t a)
{
    if (a < 0x10002000) {
        int n = (a >> 11) & 3;
        timer *t = &timers[n];
        timer_advance(n, ps2_host_time());
        switch (a & 0x7FF) {
        case 0x00: return (uint32_t)t->count & 0xFFFF;
        case 0x10: return t->mode;
        case 0x20: return t->comp;
        case 0x30: return t->hold;
        default: return 0;
        }
    }
    int ch = dma_index(a);
    if (ch >= 0) {
        dma_chan *d = &hw_dma[ch];
        switch (a & 0xFF) {
        case 0x00: return d->chcr;
        case 0x10: return d->madr;
        case 0x20: return d->qwc;
        case 0x30: return d->tadr;
        case 0x40: return d->asr0;
        case 0x50: return d->asr1;
        case 0x80: return d->sadr;
        default: return 0;
        }
    }
    switch (a) {
    case 0x10003020: return 0;              /* GIF_STAT: idle */
    case 0x10003800: return 0;              /* VIF0_STAT */
    case 0x10003C00: return 0;              /* VIF1_STAT */
    case 0x1000E000: return dmac_ctrl;
    case 0x1000E010: return hw_dmac_stat;
    case 0x1000E020: return dmac_pcr;
    case 0x1000E030: return dmac_sqwc;
    case 0x1000E040: return dmac_rbsr;
    case 0x1000E050: return dmac_rbor;
    case 0x1000E060: return dmac_stadr;
    case 0x1000F000:
        hw_update_time();
        return hw_intc_stat;
    case 0x1000F010: return hw_intc_mask;
    case 0x1000F110: return 0x60;           /* SIO LSR: TX empty */
    case 0x1000F130: return 0;              /* SIO ISR */
    case 0x1000F200: return sif_get_reg(1);
    case 0x1000F210: return sif_get_reg(2);
    case 0x1000F220: return sif_get_reg(3);
    case 0x1000F230: return sif_get_reg(4);
    case 0x1000F240: return 0;
    case 0x1000F520: return dmac_enable;
    default: break;
    }
    if (a >= 0x1000F400 && a < 0x1000F500)
        return 0;                           /* MCH / RDRAM init */
    char key[32];
    snprintf(key, sizeof key, "hwr%08X", a);
    ps2_warn_once(key, "unhandled hardware read %08X", a);
    return 0;
}

void hw_write32(uint32_t a, uint32_t v)
{
    if (a < 0x10002000) {
        int n = (a >> 11) & 3;
        timer *t = &timers[n];
        timer_advance(n, ps2_host_time());
        switch (a & 0x7FF) {
        case 0x00: t->count = v & 0xFFFF; break;
        case 0x10:
            t->mode = (t->mode & 0xC00 & ~(v & 0xC00)) | (v & 0x3FF);
            break;
        case 0x20: t->comp = v & 0xFFFF; t->mode &= ~0x400u; break;
        case 0x30: t->hold = v & 0xFFFF; break;
        default: break;
        }
        ps2_event_flag = 1;
        return;
    }
    int ch = dma_index(a);
    if (ch >= 0) {
        dma_chan *d = &hw_dma[ch];
        switch (a & 0xFF) {
        case 0x00:
            d->chcr = v;
            if (v & 0x100)
                dma_start(ch);
            break;
        case 0x10: d->madr = v; break;
        case 0x20: d->qwc = v & 0xFFFF; break;
        case 0x30: d->tadr = v; break;
        case 0x40: d->asr0 = v; break;
        case 0x50: d->asr1 = v; break;
        case 0x80: d->sadr = v & 0x3FF0; break;
        default: break;
        }
        return;
    }
    switch (a) {
    case 0x10003000: /* GIF_CTRL */
        if (v & 1)
            gs_gif_tag(NULL);  /* reset GIF path state */
        return;
    case 0x10003010: case 0x10003C10: case 0x10003810: case 0x10003C20:
    case 0x10003820: case 0x10003C30: case 0x10003830:
        return;              /* GIF_MODE / VIF FBRST / ERR / MARK */
    case 0x1000E000: dmac_ctrl = v; return;
    case 0x1000E010:
        hw_dmac_stat &= ~(v & 0xFFFF);
        hw_dmac_stat ^= v & 0xFFFF0000u;
        ps2_event_flag = 1;
        return;
    case 0x1000E020: dmac_pcr = v; return;
    case 0x1000E030: dmac_sqwc = v; return;
    case 0x1000E040: dmac_rbsr = v; return;
    case 0x1000E050: dmac_rbor = v; return;
    case 0x1000E060: dmac_stadr = v; return;
    case 0x1000F000: hw_intc_stat &= ~v; return;
    case 0x1000F010:
        hw_intc_mask ^= v & 0x7FFF;
        ps2_event_flag = 1;
        return;
    case 0x1000F180: { /* SIO TX */
        char ch8 = (char)v;
        fputc(ch8, stdout);
        if (ch8 == '\n')
            fflush(stdout);
        sio_line_pos++;
        return;
    }
    case 0x1000F100: case 0x1000F110: case 0x1000F120: case 0x1000F130:
    case 0x1000F140: case 0x1000F150:
        return;
    case 0x1000F200: case 0x1000F210: case 0x1000F220: case 0x1000F230:
        sif_set_reg(((a - 0x1000F200) >> 4) + 1, v);
        return;
    case 0x1000F240: case 0x1000F260:
        return;
    case 0x1000F590: dmac_enable = v; return;
    default: break;
    }
    if (a >= 0x1000F400 && a < 0x1000F500)
        return;
    char key[32];
    snprintf(key, sizeof key, "hww%08X", a);
    ps2_warn_once(key, "unhandled hardware write %08X = %08X", a, v);
}

uint64_t hw_read64(uint32_t a)
{
    return hw_read32(a) | ((uint64_t)hw_read32(a + 4) << 32);
}

void hw_write64(uint32_t a, uint64_t v)
{
    if ((a & 0xFFFFF000u) == 0x10006000u || (a & 0xFFFFF000u) == 0x10005000u ||
        (a & 0xFFFFF000u) == 0x10004000u) {
        ps2_warn_once("fifo64", "64-bit FIFO write ignored at %08X", a);
        return;
    }
    hw_write32(a, (uint32_t)v);
}

void hw_read128(uint32_t a, u128 *v)
{
    v->UD[0] = hw_read64(a);
    v->UD[1] = 0;
}

void hw_write128(uint32_t a, const u128 *v)
{
    switch (a & 0xFFFFF000u) {
    case 0x10004000: vif_process(0, (const uint8_t *)v, 1); return;
    case 0x10005000: vif_process(1, (const uint8_t *)v, 1); return;
    case 0x10006000: gif_packet((const uint8_t *)v, 1, 3); return;
    case 0x10007000: ps2_warn_once("ipufifo", "IPU FIFO is not emulated"); return;
    default: hw_write64(a, v->UD[0]); return;
    }
}

int ps2_cpcond0(R5900 *c)
{
    (void)c;
    /* True once every channel selected in D_PCR.CPC has completed. */
    uint32_t cpc = dmac_pcr & 0x3FF;
    return (hw_dmac_stat & cpc) == cpc;
}
