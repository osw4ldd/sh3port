/*
 * DMAC transfers (normal and source-chain modes) and the VIF command
 * processors.  Transfers complete instantly; completion raises the channel's
 * D_STAT bit, delivered to DMAC handlers at the next safe point.
 *
 * VIF: UNPACK/MPG fill VU memory, DIRECT/DIRECTHL feed GIF PATH2.
 * VU1 micro programs (MSCAL...) are not executed.
 */
#include "soft.h"

static uint8_t *dma_ptr(uint32_t addr)
{
    if ((addr & 0x80000000u) || (addr >> 28) == 7)
        return ps2_spr + (addr & 0x3FF0);
    return ps2_ram + (addr & (PS2_RAM_SIZE - 16));
}

/* Feed `qwc` quadwords at guest address `addr` to the channel's sink. */
static void dma_feed(int ch, uint32_t addr, uint32_t qwc)
{
    while (qwc) {
        /* Never cross the end of RAM / scratchpad in one host copy. */
        uint32_t limit;
        if ((addr & 0x80000000u) || (addr >> 28) == 7)
            limit = (0x4000 - (addr & 0x3FF0)) / 16;
        else
            limit = (PS2_RAM_SIZE - (addr & (PS2_RAM_SIZE - 16))) / 16;
        uint32_t n = qwc < limit ? qwc : limit;
        const uint8_t *p = dma_ptr(addr);
        switch (ch) {
        case DMAC_VIF0:
            vif_process(0, p, n);
            break;
        case DMAC_VIF1:
            if (hw_dma[1].chcr & 1)
                vif_process(1, p, n);
            else
                ps2_warn_once("vif1dl", "VIF1 -> memory (GS download) DMA is not emulated");
            break;
        case DMAC_GIF:
            gif_packet(p, n, 3);
            break;
        case DMAC_TO_SPR: {
            for (uint32_t i = 0; i < n; i++) {
                memcpy(ps2_spr + (hw_dma[9].sadr & 0x3FF0), p + i * 16, 16);
                hw_dma[9].sadr = (hw_dma[9].sadr + 16) & 0x3FF0;
            }
            break;
        }
        case DMAC_FROM_SPR: {
            uint8_t *dst = (uint8_t *)p;
            for (uint32_t i = 0; i < n; i++) {
                memcpy(dst + i * 16, ps2_spr + (hw_dma[8].sadr & 0x3FF0), 16);
                hw_dma[8].sadr = (hw_dma[8].sadr + 16) & 0x3FF0;
            }
            break;
        }
        default: {
            char key[32];
            snprintf(key, sizeof key, "dmach%d", ch);
            ps2_warn_once(key, "DMA channel %d is not emulated", ch);
            return;
        }
        }
        qwc -= n;
        addr += n * 16;
    }
}

static void dma_chain(int ch)
{
    dma_chan *d = &hw_dma[ch];
    int tte = (d->chcr >> 6) & 1;
    for (int guard = 0; guard < 1 << 20; guard++) {
        uint64_t tag[2];
        memcpy(tag, dma_ptr(d->tadr), 16);
        uint32_t qwc = (uint32_t)tag[0] & 0xFFFF;
        uint32_t id = ((uint32_t)tag[0] >> 28) & 7;
        int irq = ((uint32_t)tag[0] >> 31) & 1;
        uint32_t addr = (uint32_t)(tag[0] >> 32) & 0x7FFFFFF0u;
        if (tag[0] >> 63)
            addr |= 0x80000000u;
        int asp = (d->chcr >> 4) & 3;
        int end = 0;
        d->chcr = (d->chcr & 0xFFFF) | ((uint32_t)tag[0] & 0xFFFF0000u);
        d->qwc = qwc;
        if (tte && (ch == DMAC_VIF0 || ch == DMAC_VIF1)) {
            /* The upper half of the tag is passed through as VIF code. */
            uint32_t w[4];
            memcpy(w, tag, 16);
            vif_words(ch, w + 2, 2);
        }
        uint32_t data = d->tadr + 16;
        switch (id) {
        case 0: /* refe */
            dma_feed(ch, addr, qwc);
            d->madr = addr + qwc * 16;
            d->tadr += 16;
            end = 1;
            break;
        case 1: /* cnt */
            dma_feed(ch, data, qwc);
            d->madr = data + qwc * 16;
            d->tadr = d->madr;
            break;
        case 2: /* next */
            dma_feed(ch, data, qwc);
            d->madr = data + qwc * 16;
            d->tadr = addr;
            break;
        case 3: /* ref */
        case 4: /* refs */
            dma_feed(ch, addr, qwc);
            d->madr = addr + qwc * 16;
            d->tadr += 16;
            break;
        case 5: /* call */
            dma_feed(ch, data, qwc);
            d->madr = data + qwc * 16;
            if (asp == 0)
                d->asr0 = d->madr;
            else
                d->asr1 = d->madr;
            asp = asp < 2 ? asp + 1 : 2;
            d->tadr = addr;
            break;
        case 6: /* ret */
            dma_feed(ch, data, qwc);
            d->madr = data + qwc * 16;
            if (asp == 2) {
                d->tadr = d->asr1;
                asp = 1;
            } else if (asp == 1) {
                d->tadr = d->asr0;
                asp = 0;
            } else {
                d->tadr = d->madr;
                end = 1;
            }
            break;
        default: /* end */
            dma_feed(ch, data, qwc);
            d->madr = data + qwc * 16;
            d->tadr = d->madr;
            end = 1;
            break;
        }
        d->chcr = (d->chcr & ~0x30u) | ((uint32_t)asp << 4);
        if (irq && (d->chcr & 0x80))
            end = 1;
        if (end)
            return;
    }
    ps2_warn_once("dmaloop", "DMA chain on channel %d did not terminate", ch);
}

void dma_start(int ch)
{
    dma_chan *d = &hw_dma[ch];
    int mode = (d->chcr >> 2) & 3;
    if (ch == DMAC_SIF0 || ch == DMAC_SIF1 || ch == DMAC_SIF2) {
        /* SIF traffic is handled at the syscall level (sif.c). */
        d->chcr &= ~0x100u;
        return;
    }
    if (mode == 1 && ch != DMAC_FROM_SPR) {
        dma_chain(ch);
    } else {
        if (mode == 2)
            ps2_warn_once("dmainterleave", "interleaved DMA treated as normal transfer");
        dma_feed(ch, d->madr, d->qwc);
        d->madr += d->qwc * 16;
        d->qwc = 0;
    }
    d->chcr &= ~0x100u;
    hw_dma_complete(ch);
}

/* ================================================================ VIF */
typedef struct vif_state {
    uint32_t cmd;            /* current command word */
    uint32_t remaining;      /* words of payload still expected */
    uint32_t stage;          /* progress inside the command */
    uint32_t cl, wl, mode, mask, row[4], col[4];
    uint32_t base, ofst, tops, itop, top;
    uint32_t mpg_addr;
    /* unpack */
    uint32_t up_addr, up_num, up_done, up_vn, up_vl, up_usn, up_masked;
    uint32_t up_bits, up_bitbuf_len;
    uint64_t up_bitbuf;
    /* direct */
    uint8_t qbuf[16];
    uint32_t qlen;
} vif_state;

static vif_state vifs[2];

static uint8_t *vu_data(int unit) { return unit ? ps2_vu1_mem : ps2_vu0_mem; }
static uint32_t vu_data_mask(int unit) { return unit ? 0x3FFF : 0xFFF; }
static uint8_t *vu_code(int unit) { return unit ? ps2_vu1_code : ps2_vu0_code; }
static uint32_t vu_code_mask(int unit) { return unit ? 0x3FFF : 0xFFF; }

static void unpack_write(int unit, vif_state *v, const uint32_t *in)
{
    uint32_t n = v->up_done;
    uint32_t cl = v->cl ? v->cl : 1, wl = v->wl ? v->wl : 1;
    uint32_t qw;
    if (wl <= cl)
        qw = v->up_addr + (n / wl) * cl + (n % wl);
    else
        qw = v->up_addr + n;
    uint8_t *dst = vu_data(unit) + ((qw * 16) & vu_data_mask(unit));
    uint32_t cycle = (wl <= cl) ? (n % wl) : (n % cl);
    uint32_t out[4];
    memcpy(out, dst, 16);
    for (int i = 0; i < 4; i++) {
        uint32_t m = v->up_masked ? (v->mask >> ((cycle < 4 ? cycle : 3) * 8 + i * 2)) & 3 : 0;
        uint32_t val = in[i];
        switch (m) {
        case 0:
            if (v->mode == 1)
                val += v->row[i];
            else if (v->mode == 2) {
                val += v->row[i];
                v->row[i] = val;
            }
            out[i] = val;
            break;
        case 1: out[i] = v->row[i]; break;
        case 2: out[i] = v->col[cycle < 4 ? cycle : 3]; break;
        default: break; /* write protect */
        }
    }
    memcpy(dst, out, 16);
    v->up_done++;
}

static uint32_t sext(uint32_t v, int bits, int usn)
{
    if (usn || bits == 32)
        return v;
    uint32_t sign = 1u << (bits - 1);
    return (v ^ sign) - sign;
}

static void unpack_feed_word(int unit, vif_state *v, uint32_t word)
{
    v->up_bitbuf |= (uint64_t)word << v->up_bitbuf_len;
    v->up_bitbuf_len += 32;
    uint32_t comps = v->up_vn + 1;
    uint32_t esize = 32u >> v->up_vl;
    uint32_t vbits = (v->up_vl == 3) ? 16 : esize * comps;
    while (v->up_bitbuf_len >= vbits && v->up_done < v->up_num) {
        uint32_t in[4] = {0, 0, 0, 0};
        uint64_t bits = v->up_bitbuf;
        if (v->up_vl == 3) { /* V4-5: RGBA 5551 */
            uint32_t c = (uint32_t)bits & 0xFFFF;
            in[0] = (c & 0x1F) << 3;
            in[1] = ((c >> 5) & 0x1F) << 3;
            in[2] = ((c >> 10) & 0x1F) << 3;
            in[3] = ((c >> 15) & 1) << 7;
        } else {
            for (uint32_t i = 0; i < comps; i++) {
                uint32_t e = (uint32_t)(bits >> (i * esize)) & (esize == 32 ? 0xFFFFFFFFu : ((1u << esize) - 1));
                in[i] = sext(e, (int)esize, (int)v->up_usn);
            }
            if (comps == 1)
                in[1] = in[2] = in[3] = in[0];
            else if (comps == 2) {
                in[2] = in[0];
                in[3] = in[1];
            }
        }
        unpack_write(unit, v, in);
        v->up_bitbuf = vbits >= 64 ? 0 : (v->up_bitbuf >> vbits);
        v->up_bitbuf_len -= vbits;
    }
}

static void vif_direct_word(vif_state *v, uint32_t word)
{
    memcpy(v->qbuf + v->qlen, &word, 4);
    v->qlen += 4;
    if (v->qlen == 16) {
        gif_packet(v->qbuf, 1, 2);
        v->qlen = 0;
    }
}

static uint32_t unpack_words(uint32_t vn, uint32_t vl, uint32_t num)
{
    uint32_t bits = (vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    return (num * bits + 31) / 32;
}

static void vif_begin(int unit, vif_state *v, uint32_t w)
{
    uint32_t cmd = (w >> 24) & 0x7F;
    uint32_t num = (w >> 16) & 0xFF;
    uint32_t imm = w & 0xFFFF;
    v->cmd = w;
    v->remaining = 0;
    v->stage = 0;
    if (cmd >= 0x60) { /* UNPACK */
        v->up_vn = (cmd >> 2) & 3;
        v->up_vl = cmd & 3;
        v->up_masked = (cmd >> 4) & 1;
        v->up_num = num ? num : 256;
        v->up_done = 0;
        v->up_usn = (imm >> 14) & 1;
        v->up_addr = imm & 0x3FF;
        if (unit == 1 && (imm & 0x8000))
            v->up_addr += v->tops;
        v->up_bitbuf = 0;
        v->up_bitbuf_len = 0;
        v->remaining = unpack_words(v->up_vn, v->up_vl, v->up_num);
        return;
    }
    switch (cmd) {
    case 0x00: break;                                   /* NOP */
    case 0x01: v->cl = imm & 0xFF; v->wl = imm >> 8; break;  /* STCYCL */
    case 0x02: v->ofst = imm & 0x3FF; break;            /* OFFSET */
    case 0x03: v->base = imm & 0x3FF; v->tops = v->base; break; /* BASE */
    case 0x04: v->itop = imm & 0x3FF; break;            /* ITOP */
    case 0x05: v->mode = imm & 3; break;                /* STMOD */
    case 0x06: case 0x07: break;                        /* MSKPATH3 / MARK */
    case 0x10: case 0x11: case 0x13: break;             /* FLUSHE/FLUSH/FLUSHA */
    case 0x14: case 0x15: case 0x17: {                  /* MSCAL/MSCALF/MSCNT */
        char key[32];
        snprintf(key, sizeof key, "vu%dmscal", unit);
        ps2_warn_once(key, "VIF%d MSCAL: VU%d micro programs are not emulated "
                      "(3D graphics rendered through VU1 will be missing)", unit, unit);
        if (unit == 1) {
            uint32_t t = v->tops;
            v->tops = v->base + (t == v->base ? v->ofst : 0);
            v->top = t;
        }
        break;
    }
    case 0x20: v->remaining = 1; break;                 /* STMASK */
    case 0x30: case 0x31: v->remaining = 4; break;      /* STROW / STCOL */
    case 0x4A:                                          /* MPG */
        v->mpg_addr = imm * 8;
        v->remaining = (num ? num : 256) * 2;
        break;
    case 0x50: case 0x51:                               /* DIRECT / DIRECTHL */
        if (unit == 0) {
            ps2_warn_once("vif0direct", "DIRECT on VIF0 ignored");
            break;
        }
        v->remaining = (imm ? imm : 65536) * 4;
        v->qlen = 0;
        break;
    default: {
        char key[32];
        snprintf(key, sizeof key, "vifcmd%02X", cmd);
        ps2_warn_once(key, "unknown VIF%d command %02X", unit, cmd);
        break;
    }
    }
}

static void vif_payload(int unit, vif_state *v, uint32_t w)
{
    uint32_t cmd = (v->cmd >> 24) & 0x7F;
    if (cmd >= 0x60) {
        unpack_feed_word(unit, v, w);
    } else if (cmd == 0x20) {
        v->mask = w;
    } else if (cmd == 0x30) {
        v->row[v->stage] = w;
    } else if (cmd == 0x31) {
        v->col[v->stage] = w;
    } else if (cmd == 0x4A) {
        memcpy(vu_code(unit) + ((v->mpg_addr + v->stage * 4) & vu_code_mask(unit)), &w, 4);
    } else if (cmd == 0x50 || cmd == 0x51) {
        if (unit == 1)
            vif_direct_word(v, w);
    }
    v->stage++;
    v->remaining--;
}

void vif_words(int unit, const uint32_t *w, uint32_t n)
{
    vif_state *v = &vifs[unit & 1];
    for (uint32_t i = 0; i < n; i++) {
        if (v->remaining)
            vif_payload(unit, v, w[i]);
        else
            vif_begin(unit, v, w[i]);
    }
}

void vif_process(int unit, const uint8_t *data, uint32_t qwc)
{
    uint32_t words[4];
    for (uint32_t q = 0; q < qwc; q++) {
        memcpy(words, data + q * 16, 16);
        vif_words(unit, words, 4);
    }
}
