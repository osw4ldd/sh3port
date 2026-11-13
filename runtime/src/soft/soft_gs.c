/*
 * Graphics Synthesizer: GIF packet parsing, register state, local memory
 * with PS2 page/block addressing, a software rasterizer (points, lines,
 * triangles, strips, fans, sprites) with texturing, CLUT, fog, alpha/depth
 * tests and blending, image transfers and display read-out.
 *
 * Simplification: pixels inside a block are stored in a linear order for
 * 16/8/4-bit formats (32-bit formats use the real column layout).  This is
 * self-consistent and only differs from hardware when software reinterprets
 * memory between formats of different depth.
 */
#include "soft.h"

#define VRAM_WORDS (1u << 20)   /* 4 MiB */
static uint32_t *vram;

/* ------------------------------------------------------------ registers */
typedef struct gs_ctx {
    uint64_t frame, zbuf, tex0, tex1, clamp, xyoffset, scissor, alpha, test, fba, miptbp1;
} gs_ctx;

static gs_ctx ctx[2];
static uint64_t r_prim, r_prmode, r_prmodecont = 1, r_rgbaq, r_st, r_uv, r_fog;
static uint64_t r_texclut, r_texa, r_fogcol, r_dthe, r_colclamp = 1, r_pabe, r_scanmsk, r_dimx;
static uint64_t r_bitbltbuf, r_trxpos, r_trxreg, r_trxdir;
static float packed_q = 1.0f;

/* privileged registers */
static uint64_t p_pmode, p_smode1, p_smode2, p_srfsh, p_synch1, p_synch2, p_syncv;
static uint64_t p_dispfb[2], p_display[2], p_extbuf, p_extdata, p_extwrite, p_bgcolor;
static uint64_t p_csr, p_imr = 0x7F00, p_busdir, p_siglblid;

/* CLUT buffer */
static uint32_t clut[256];
static uint32_t clut_cbp0 = 0xFFFFFFFF, clut_cbp1 = 0xFFFFFFFF;

#define BITS(v, lo, n) ((uint32_t)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* ------------------------------------------------------------ addressing */
static const uint8_t block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t block32z[4][8] = {
    {24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15},
    {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
static const uint8_t column32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t block16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t block16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const uint8_t block16z[8][4] = {
    {24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23},
    {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
static const uint8_t block16sz[8][4] = {
    {24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3},
    {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
static const uint8_t block8[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t block4[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

static inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int z)
{
    uint32_t page = (x >> 6) + (y >> 5) * (bw ? bw : 1);
    uint32_t blk = bp + page * 32 + (z ? block32z : block32)[(y >> 3) & 3][(x >> 3) & 7];
    return (blk * 64 + column32[y & 7][x & 7]) & (VRAM_WORDS - 1);
}

/* returns halfword index */
static inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int psm)
{
    const uint8_t(*tbl)[4] = psm == PSMCT16S ? block16s : psm == PSMZ16 ? block16z :
                             psm == PSMZ16S ? block16sz : block16;
    uint32_t page = (x >> 6) + (y >> 6) * (bw ? bw : 1);
    uint32_t blk = bp + page * 32 + tbl[(y >> 3) & 7][(x >> 4) & 3];
    return (blk * 128 + (y & 7) * 16 + (x & 15)) & (VRAM_WORDS * 2 - 1);
}

static inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t pw = bw >> 1;
    uint32_t page = (x >> 7) + (y >> 6) * (pw ? pw : 1);
    uint32_t blk = bp + page * 32 + block8[(y >> 4) & 3][(x >> 4) & 7];
    return (blk * 256 + (y & 15) * 16 + (x & 15)) & (VRAM_WORDS * 4 - 1);
}

/* returns nibble index */
static inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t pw = bw >> 1;
    uint32_t page = (x >> 7) + (y >> 7) * (pw ? pw : 1);
    uint32_t blk = bp + page * 32 + block4[(y >> 4) & 7][(x >> 5) & 3];
    return (blk * 512 + (y & 15) * 32 + (x & 31)) & (VRAM_WORDS * 8 - 1);
}

/* Raw pixel access in a given format (value is the format's native bits). */
static uint32_t pix_read(uint32_t bp, uint32_t bw, int psm, uint32_t x, uint32_t y)
{
    switch (psm) {
    case PSMCT32: case PSMZ32:
        return vram[addr32(bp, bw, x, y, psm == PSMZ32)];
    case PSMCT24: case PSMZ24:
        return vram[addr32(bp, bw, x, y, psm == PSMZ24)] & 0xFFFFFF;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return ((uint16_t *)vram)[addr16(bp, bw, x, y, psm)];
    case PSMT8:
        return ((uint8_t *)vram)[addr8(bp, bw, x, y)];
    case PSMT4: {
        uint32_t n = addr4(bp, bw, x, y);
        return (((uint8_t *)vram)[n >> 1] >> ((n & 1) * 4)) & 15;
    }
    case PSMT8H:
        return vram[addr32(bp, bw, x, y, 0)] >> 24;
    case PSMT4HL:
        return (vram[addr32(bp, bw, x, y, 0)] >> 24) & 15;
    case PSMT4HH:
        return vram[addr32(bp, bw, x, y, 0)] >> 28;
    default:
        return vram[addr32(bp, bw, x, y, 0)];
    }
}

static void pix_write(uint32_t bp, uint32_t bw, int psm, uint32_t x, uint32_t y, uint32_t v)
{
    switch (psm) {
    case PSMCT32: case PSMZ32:
        vram[addr32(bp, bw, x, y, psm == PSMZ32)] = v;
        break;
    case PSMCT24: case PSMZ24: {
        uint32_t *p = &vram[addr32(bp, bw, x, y, psm == PSMZ24)];
        *p = (*p & 0xFF000000u) | (v & 0xFFFFFF);
        break;
    }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        ((uint16_t *)vram)[addr16(bp, bw, x, y, psm)] = (uint16_t)v;
        break;
    case PSMT8:
        ((uint8_t *)vram)[addr8(bp, bw, x, y)] = (uint8_t)v;
        break;
    case PSMT4: {
        uint32_t n = addr4(bp, bw, x, y);
        uint8_t *b = &((uint8_t *)vram)[n >> 1];
        if (n & 1)
            *b = (uint8_t)((*b & 0x0F) | ((v & 15) << 4));
        else
            *b = (uint8_t)((*b & 0xF0) | (v & 15));
        break;
    }
    case PSMT8H: {
        uint32_t *p = &vram[addr32(bp, bw, x, y, 0)];
        *p = (*p & 0x00FFFFFFu) | (v << 24);
        break;
    }
    case PSMT4HL: {
        uint32_t *p = &vram[addr32(bp, bw, x, y, 0)];
        *p = (*p & 0xF0FFFFFFu) | ((v & 15) << 24);
        break;
    }
    case PSMT4HH: {
        uint32_t *p = &vram[addr32(bp, bw, x, y, 0)];
        *p = (*p & 0x0FFFFFFFu) | ((v & 15) << 28);
        break;
    }
    default:
        vram[addr32(bp, bw, x, y, 0)] = v;
        break;
    }
}

/* ------------------------------------------------------------ colour */
static inline uint32_t rgba16_to_32(uint32_t c, int aem, uint32_t ta0, uint32_t ta1)
{
    uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    uint32_t a;
    if (c & 0x8000)
        a = ta1;
    else
        a = (aem && !(c & 0x7FFF)) ? 0 : ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline uint32_t rgba32_to_16(uint32_t c)
{
    return ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) |
           ((c >> 31) << 15);
}

/* ------------------------------------------------------------ CLUT */
static void clut_load(uint64_t tex0)
{
    uint32_t cbp = BITS(tex0, 37, 14), cpsm = BITS(tex0, 51, 4), csm = BITS(tex0, 55, 1);
    uint32_t csa = BITS(tex0, 56, 5), psm = BITS(tex0, 20, 6);
    int is4 = (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH);
    int n = is4 ? 16 : 256;
    uint32_t base = is4 ? (csa & 15) * 16 : 0;
    for (int i = 0; i < n; i++) {
        uint32_t x, y;
        if (csm == 0) {
            if (is4) {
                x = (uint32_t)i & 7;
                y = (uint32_t)i >> 3;
            } else {
                uint32_t p = ((uint32_t)i & 0xE7) | (((uint32_t)i & 0x08) << 1) | (((uint32_t)i & 0x10) >> 1);
                x = p & 15;
                y = p >> 4;
            }
            clut[(base + i) & 255] = pix_read(cbp, 1, cpsm == 0 ? PSMCT32 : (int)cpsm, x, y);
        } else {
            uint32_t cbw = BITS(r_texclut, 0, 6), cou = BITS(r_texclut, 6, 6), cov = BITS(r_texclut, 12, 10);
            clut[(base + i) & 255] = pix_read(cbp, cbw, cpsm == 0 ? PSMCT32 : (int)cpsm,
                                              cou * 16 + (uint32_t)i, cov);
        }
    }
}

static void tex0_written(uint64_t v)
{
    uint32_t cld = BITS(v, 61, 3), cbp = BITS(v, 37, 14);
    uint32_t psm = BITS(v, 20, 6);
    int indexed = psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
    if (!indexed)
        return;
    switch (cld) {
    case 1: clut_load(v); break;
    case 2: clut_load(v); clut_cbp0 = cbp; break;
    case 3: clut_load(v); clut_cbp1 = cbp; break;
    case 4: if (cbp != clut_cbp0) { clut_load(v); clut_cbp0 = cbp; } break;
    case 5: if (cbp != clut_cbp1) { clut_load(v); clut_cbp1 = cbp; } break;
    default: break;
    }
}

/* ------------------------------------------------------------ vertices */
typedef struct vtx {
    float x, y;       /* window coordinates (pixels) */
    uint32_t z;
    float r, g, b, a;
    float s, t, q;    /* STQ (FST=0) */
    float u, v;       /* texel coordinates (FST=1) */
    float f;          /* fog */
} vtx;

static vtx vq[3];
static int vcount;
static int strip_fan_first = 1;

static int prim_attr(void)
{
    return (int)((r_prmodecont & 1) ? r_prim : ((r_prim & 7) | (r_prmode & 0x7F8)));
}

/* --------------------------------------------------------- pixel pipeline */
typedef struct raster {
    gs_ctx *c;
    int tme, fge, abe, fst, iip, aa1;
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm, zmsk;
    int zte, ztst, ate, atst, aref, afail, date, datm;
    uint32_t alpha_a, alpha_b, alpha_c, alpha_d, alpha_fix;
    int scx0, scx1, scy0, scy1;
    /* texture */
    uint32_t tbp, tbw, tpsm, tw, th, tcc, tfx;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    int bilinear;
    int fba, pabe, colclamp;
    uint32_t fogcol;
} raster;

static void raster_setup(raster *rs)
{
    int attr = prim_attr();
    gs_ctx *c = &ctx[(attr >> 9) & 1];
    rs->c = c;
    rs->iip = (attr >> 3) & 1;
    rs->tme = (attr >> 4) & 1;
    rs->fge = (attr >> 5) & 1;
    rs->abe = (attr >> 6) & 1;
    rs->aa1 = (attr >> 7) & 1;
    rs->fst = (attr >> 8) & 1;
    rs->fbp = BITS(c->frame, 0, 9) * 32;
    rs->fbw = BITS(c->frame, 16, 6);
    rs->fpsm = BITS(c->frame, 24, 6);
    rs->fbmsk = (uint32_t)(c->frame >> 32);
    rs->zbp = BITS(c->zbuf, 0, 9) * 32;
    rs->zpsm = BITS(c->zbuf, 24, 4) | 0x30;
    rs->zmsk = BITS(c->zbuf, 32, 1);
    rs->ate = BITS(c->test, 0, 1);
    rs->atst = BITS(c->test, 1, 3);
    rs->aref = BITS(c->test, 4, 8);
    rs->afail = BITS(c->test, 12, 2);
    rs->date = BITS(c->test, 14, 1);
    rs->datm = BITS(c->test, 15, 1);
    rs->zte = BITS(c->test, 16, 1);
    rs->ztst = BITS(c->test, 17, 2);
    rs->alpha_a = BITS(c->alpha, 0, 2);
    rs->alpha_b = BITS(c->alpha, 2, 2);
    rs->alpha_c = BITS(c->alpha, 4, 2);
    rs->alpha_d = BITS(c->alpha, 6, 2);
    rs->alpha_fix = BITS(c->alpha, 32, 8);
    rs->scx0 = (int)BITS(c->scissor, 0, 11);
    rs->scx1 = (int)BITS(c->scissor, 16, 11);
    rs->scy0 = (int)BITS(c->scissor, 32, 11);
    rs->scy1 = (int)BITS(c->scissor, 48, 11);
    rs->tbp = BITS(c->tex0, 0, 14);
    rs->tbw = BITS(c->tex0, 14, 6);
    rs->tpsm = BITS(c->tex0, 20, 6);
    rs->tw = 1u << (BITS(c->tex0, 26, 4) > 10 ? 10 : BITS(c->tex0, 26, 4));
    rs->th = 1u << (BITS(c->tex0, 30, 4) > 10 ? 10 : BITS(c->tex0, 30, 4));
    rs->tcc = BITS(c->tex0, 34, 1);
    rs->tfx = BITS(c->tex0, 35, 2);
    rs->wms = BITS(c->clamp, 0, 2);
    rs->wmt = BITS(c->clamp, 2, 2);
    rs->minu = BITS(c->clamp, 4, 10);
    rs->maxu = BITS(c->clamp, 14, 10);
    rs->minv = BITS(c->clamp, 24, 10);
    rs->maxv = BITS(c->clamp, 34, 10);
    rs->bilinear = BITS(c->tex1, 5, 1);
    rs->fba = BITS(c->fba, 0, 1);
    rs->pabe = (int)(r_pabe & 1);
    rs->colclamp = (int)(r_colclamp & 1);
    rs->fogcol = (uint32_t)r_fogcol & 0xFFFFFF;
    if (rs->fbw == 0)
        rs->fbw = 1;
}

static inline int wrap_coord(int c, uint32_t size, uint32_t mode, uint32_t mn, uint32_t mx)
{
    switch (mode) {
    case 0: return c & (int)(size - 1);
    case 1: return c < 0 ? 0 : c >= (int)size ? (int)size - 1 : c;
    case 2: return c < (int)mn ? (int)mn : c > (int)mx ? (int)mx : c;
    default: return (int)(((uint32_t)c & mn) | mx);
    }
}

static uint32_t texel_fetch(const raster *rs, int u, int v)
{
    u = wrap_coord(u, rs->tw, rs->wms, rs->minu, rs->maxu);
    v = wrap_coord(v, rs->th, rs->wmt, rs->minv, rs->maxv);
    uint32_t aem = BITS(r_texa, 15, 1), ta0 = BITS(r_texa, 0, 8), ta1 = BITS(r_texa, 32, 8);
    uint32_t raw = pix_read(rs->tbp, rs->tbw, (int)rs->tpsm, (uint32_t)u, (uint32_t)v);
    switch (rs->tpsm) {
    case PSMCT32: case PSMZ32:
        return raw;
    case PSMCT24: case PSMZ24:
        return raw | ((aem && !(raw & 0xFFFFFF)) ? 0 : ta0 << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return rgba16_to_32(raw, (int)aem, ta0, ta1);
    default: {
        uint32_t cpsm = BITS(rs->c->tex0, 51, 4);
        uint32_t idx = raw;
        if (rs->tpsm == PSMT4 || rs->tpsm == PSMT4HL || rs->tpsm == PSMT4HH)
            idx += BITS(rs->c->tex0, 56, 4) * 16;
        uint32_t e = clut[idx & 255];
        return cpsm == 0 ? e : rgba16_to_32(e & 0xFFFF, (int)aem, ta0, ta1);
    }
    }
}

static uint32_t texture_sample(const raster *rs, float u, float v)
{
    if (!rs->bilinear)
        return texel_fetch(rs, (int)floorf(u), (int)floorf(v));
    u -= 0.5f;
    v -= 0.5f;
    int u0 = (int)floorf(u), v0 = (int)floorf(v);
    int fu = (int)((u - (float)u0) * 256.0f), fv = (int)((v - (float)v0) * 256.0f);
    uint32_t c00 = texel_fetch(rs, u0, v0), c10 = texel_fetch(rs, u0 + 1, v0);
    uint32_t c01 = texel_fetch(rs, u0, v0 + 1), c11 = texel_fetch(rs, u0 + 1, v0 + 1);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int a = (c00 >> sh) & 255, b = (c10 >> sh) & 255, c = (c01 >> sh) & 255, d = (c11 >> sh) & 255;
        int top = a + (((b - a) * fu) >> 8);
        int bot = c + (((d - c) * fu) >> 8);
        int val = top + (((bot - top) * fv) >> 8);
        out |= (uint32_t)(val & 255) << sh;
    }
    return out;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Shade one pixel.  r,g,b,a in 0..255 (vertex colour), u,v in texels. */
static void shade(const raster *rs, int x, int y, uint32_t z, int r, int g, int b, int a,
                  float u, float v, int fog)
{
    if (x < rs->scx0 || x > rs->scx1 || y < rs->scy0 || y > rs->scy1 || x < 0 || y < 0)
        return;
    if (r_scanmsk & 2) {
        if ((r_scanmsk & 3) == 2 && !(y & 1))
            return;
        if ((r_scanmsk & 3) == 3 && (y & 1))
            return;
    }
    if (rs->tme) {
        uint32_t t = texture_sample(rs, u, v);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (rs->tfx) {
        case 0: /* modulate */
            r = clamp255((tr * r) >> 7);
            g = clamp255((tg * g) >> 7);
            b = clamp255((tb * b) >> 7);
            a = rs->tcc ? clamp255((ta * a) >> 7) : a;
            break;
        case 1: /* decal */
            r = tr; g = tg; b = tb;
            a = rs->tcc ? ta : a;
            break;
        case 2: /* highlight */
            r = clamp255(((tr * r) >> 7) + a);
            g = clamp255(((tg * g) >> 7) + a);
            b = clamp255(((tb * b) >> 7) + a);
            a = rs->tcc ? clamp255(ta + a) : a;
            break;
        default: /* highlight2 */
            r = clamp255(((tr * r) >> 7) + a);
            g = clamp255(((tg * g) >> 7) + a);
            b = clamp255(((tb * b) >> 7) + a);
            a = rs->tcc ? ta : a;
            break;
        }
    }
    if (rs->fge) {
        int fr = rs->fogcol & 255, fgc = (rs->fogcol >> 8) & 255, fb = (rs->fogcol >> 16) & 255;
        r = (fog * r + (255 - fog) * fr) >> 8;
        g = (fog * g + (255 - fog) * fgc) >> 8;
        b = (fog * b + (255 - fog) * fb) >> 8;
    }

    int write_fb = 1, write_z = !rs->zmsk, write_alpha = 1;
    if (rs->ate) {
        int pass;
        switch (rs->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < rs->aref; break;
        case 3: pass = a <= rs->aref; break;
        case 4: pass = a == rs->aref; break;
        case 5: pass = a >= rs->aref; break;
        case 6: pass = a > rs->aref; break;
        default: pass = a != rs->aref; break;
        }
        if (!pass) {
            switch (rs->afail) {
            case 0: return;
            case 1: write_z = 0; break;
            case 2: write_fb = 0; break;
            default: write_z = 0; write_alpha = 0; break;
            }
        }
    }

    uint32_t ux = (uint32_t)x, uy = (uint32_t)y;
    if (rs->zte) {
        uint32_t zmax = rs->zpsm == PSMZ32 ? 0xFFFFFFFFu : rs->zpsm == PSMZ24 ? 0xFFFFFF : 0xFFFF;
        if (z > zmax)
            z = zmax;
        switch (rs->ztst) {
        case 0: return;
        case 1: break;
        default: {
            uint32_t zb = pix_read(rs->zbp, rs->fbw, (int)rs->zpsm, ux, uy);
            if (rs->ztst == 2 ? z < zb : z <= zb)
                return;
            break;
        }
        }
    }

    uint32_t dst = 0;
    int need_dst = rs->date || ((rs->abe || rs->aa1) && (rs->alpha_a == 1 || rs->alpha_b == 1 ||
                                rs->alpha_c == 1 || rs->alpha_d == 1)) || rs->fbmsk;
    int is16 = rs->fpsm == PSMCT16 || rs->fpsm == PSMCT16S || rs->fpsm == PSMZ16 || rs->fpsm == PSMZ16S;
    if (need_dst) {
        uint32_t raw = pix_read(rs->fbp, rs->fbw, (int)rs->fpsm, ux, uy);
        dst = is16 ? rgba16_to_32(raw, 0, 0, 0x80) : raw;
        if (rs->fpsm == PSMCT24 || rs->fpsm == PSMZ24)
            dst |= 0x80000000u;
    }
    if (rs->date) {
        int dbit = (int)(dst >> 31);
        if (dbit != rs->datm)
            return;
    }

    if ((rs->abe || rs->aa1) && !(rs->pabe && !(a & 0x80))) {
        int cs[3] = {r, g, b};
        int cd[3] = {(int)(dst & 255), (int)((dst >> 8) & 255), (int)((dst >> 16) & 255)};
        int ad = (int)(dst >> 24);
        int cval = rs->alpha_c == 0 ? a : rs->alpha_c == 1 ? ad : (int)rs->alpha_fix;
        int out[3];
        for (int i = 0; i < 3; i++) {
            int A = rs->alpha_a == 0 ? cs[i] : rs->alpha_a == 1 ? cd[i] : 0;
            int B = rs->alpha_b == 0 ? cs[i] : rs->alpha_b == 1 ? cd[i] : 0;
            int D = rs->alpha_d == 0 ? cs[i] : rs->alpha_d == 1 ? cd[i] : 0;
            out[i] = (((A - B) * cval) >> 7) + D;
        }
        r = out[0]; g = out[1]; b = out[2];
    }
    if (rs->colclamp) {
        r = clamp255(r); g = clamp255(g); b = clamp255(b);
    } else {
        r &= 255; g &= 255; b &= 255;
    }

    if (write_fb) {
        uint32_t outa = (uint32_t)a | (rs->fba ? 0x80u : 0u);
        uint32_t col = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((outa & 255) << 24);
        uint32_t mask = rs->fbmsk;
        if (!write_alpha)
            mask |= 0xFF000000u;
        if (is16) {
            uint32_t raw = pix_read(rs->fbp, rs->fbw, (int)rs->fpsm, ux, uy);
            uint32_t c16 = rgba32_to_16(col);
            uint32_t m16 = rgba32_to_16(mask);
            pix_write(rs->fbp, rs->fbw, (int)rs->fpsm, ux, uy, (c16 & ~m16) | (raw & m16));
        } else {
            if (mask) {
                uint32_t raw = pix_read(rs->fbp, rs->fbw, rs->fpsm == PSMCT24 ? PSMCT32 : (int)rs->fpsm, ux, uy);
                col = (col & ~mask) | (raw & mask);
            }
            pix_write(rs->fbp, rs->fbw, (int)rs->fpsm, ux, uy, col);
        }
    }
    if (write_z && rs->zte)
        pix_write(rs->zbp, rs->fbw, (int)rs->zpsm, ux, uy, z);
}

static inline void vtx_tex(const raster *rs, const vtx *v, float *u, float *t, float *q)
{
    if (rs->fst) {
        *u = v->u;
        *t = v->v;
        *q = 1.0f;
    } else {
        *u = v->s * (float)rs->tw;
        *t = v->t * (float)rs->th;
        *q = v->q;
    }
}

static void draw_sprite(const vtx *a, const vtx *b)
{
    raster rs;
    raster_setup(&rs);
    float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    float ua, va, qa, ub, vb, qb;
    vtx_tex(&rs, a, &ua, &va, &qa);
    vtx_tex(&rs, b, &ub, &vb, &qb);
    if (!rs.fst) {
        ua /= qa ? qa : 1; va /= qa ? qa : 1;
        ub /= qb ? qb : 1; vb /= qb ? qb : 1;
    }
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; t = ua; ua = ub; ub = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; t = va; va = vb; vb = t; }
    int ix0 = (int)ceilf(x0), ix1 = (int)ceilf(x1), iy0 = (int)ceilf(y0), iy1 = (int)ceilf(y1);
    if (ix0 < rs.scx0) ix0 = rs.scx0;
    if (iy0 < rs.scy0) iy0 = rs.scy0;
    if (ix1 > rs.scx1 + 1) ix1 = rs.scx1 + 1;
    if (iy1 > rs.scy1 + 1) iy1 = rs.scy1 + 1;
    float du = (x1 > x0) ? (ub - ua) / (x1 - x0) : 0, dv = (y1 > y0) ? (vb - va) / (y1 - y0) : 0;
    int r = (int)b->r, g = (int)b->g, bl = (int)b->b, al = (int)b->a;
    int fog = (int)b->f;
    for (int y = iy0; y < iy1; y++) {
        float v = va + ((float)y - y0) * dv;
        for (int x = ix0; x < ix1; x++) {
            float u = ua + ((float)x - x0) * du;
            shade(&rs, x, y, b->z, r, g, bl, al, u, v, fog);
        }
    }
}

static void draw_triangle(const vtx *v0, const vtx *v1, const vtx *v2)
{
    raster rs;
    raster_setup(&rs);
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0.0f)
        return;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int ix0 = (int)ceilf(minx), ix1 = (int)ceilf(maxx) - 1;
    int iy0 = (int)ceilf(miny), iy1 = (int)ceilf(maxy) - 1;
    if (ix0 < rs.scx0) ix0 = rs.scx0;
    if (iy0 < rs.scy0) iy0 = rs.scy0;
    if (ix1 > rs.scx1) ix1 = rs.scx1;
    if (iy1 > rs.scy1) iy1 = rs.scy1;
    if (ix0 > ix1 || iy0 > iy1)
        return;

    float u0, t0, q0, u1, t1, q1, u2, t2, q2;
    vtx_tex(&rs, v0, &u0, &t0, &q0);
    vtx_tex(&rs, v1, &u1, &t1, &q1);
    vtx_tex(&rs, v2, &u2, &t2, &q2);
    const vtx *flat = v2;
    float inv = 1.0f / area;
    for (int y = iy0; y <= iy1; y++) {
        float py = (float)y;
        for (int x = ix0; x <= ix1; x++) {
            float px = (float)x;
            float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * inv;
            float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            /* Top-left style inclusion: accept the shared edge on one side. */
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f)
                    continue;
            }
            double zf = (double)v0->z * w0 + (double)v1->z * w1 + (double)v2->z * w2;
            uint32_t z = zf <= 0 ? 0 : zf >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)zf;
            int r, g, b, a;
            if (rs.iip) {
                r = (int)(v0->r * w0 + v1->r * w1 + v2->r * w2);
                g = (int)(v0->g * w0 + v1->g * w1 + v2->g * w2);
                b = (int)(v0->b * w0 + v1->b * w1 + v2->b * w2);
                a = (int)(v0->a * w0 + v1->a * w1 + v2->a * w2);
            } else {
                r = (int)flat->r; g = (int)flat->g; b = (int)flat->b; a = (int)flat->a;
            }
            float u = 0, v = 0;
            if (rs.tme) {
                if (rs.fst) {
                    u = u0 * w0 + u1 * w1 + u2 * w2;
                    v = t0 * w0 + t1 * w1 + t2 * w2;
                } else {
                    float q = q0 * w0 + q1 * w1 + q2 * w2;
                    if (q == 0.0f)
                        q = 1e-6f;
                    u = (u0 * w0 + u1 * w1 + u2 * w2) / q;
                    v = (t0 * w0 + t1 * w1 + t2 * w2) / q;
                }
            }
            int fog = (int)(v0->f * w0 + v1->f * w1 + v2->f * w2);
            shade(&rs, x, y, z, clamp255(r), clamp255(g), clamp255(b), clamp255(a), u, v, fog);
        }
    }
}

static void draw_line(const vtx *a, const vtx *b)
{
    raster rs;
    raster_setup(&rs);
    float dx = b->x - a->x, dy = b->y - a->y;
    int steps = (int)fmaxf(fabsf(dx), fabsf(dy));
    if (steps < 1)
        steps = 1;
    float ua, va, qa, ub, vb, qb;
    vtx_tex(&rs, a, &ua, &va, &qa);
    vtx_tex(&rs, b, &ub, &vb, &qb);
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)steps;
        int x = (int)floorf(a->x + dx * t + 0.5f), y = (int)floorf(a->y + dy * t + 0.5f);
        const vtx *cv = rs.iip ? NULL : b;
        int r = cv ? (int)b->r : (int)(a->r + (b->r - a->r) * t);
        int g = cv ? (int)b->g : (int)(a->g + (b->g - a->g) * t);
        int bl = cv ? (int)b->b : (int)(a->b + (b->b - a->b) * t);
        int al = cv ? (int)b->a : (int)(a->a + (b->a - a->a) * t);
        double zf = (double)a->z + ((double)b->z - (double)a->z) * t;
        float q = qa + (qb - qa) * t;
        float u = (ua + (ub - ua) * t) / (rs.fst ? 1.0f : (q ? q : 1.0f));
        float v = (va + (vb - va) * t) / (rs.fst ? 1.0f : (q ? q : 1.0f));
        shade(&rs, x, y, (uint32_t)zf, r, g, bl, al, u, v, (int)b->f);
    }
}

static void draw_point(const vtx *a)
{
    raster rs;
    raster_setup(&rs);
    float u, v, q;
    vtx_tex(&rs, a, &u, &v, &q);
    if (!rs.fst && q)
        u /= q, v /= q;
    shade(&rs, (int)floorf(a->x + 0.5f), (int)floorf(a->y + 0.5f), a->z, (int)a->r, (int)a->g,
          (int)a->b, (int)a->a, u, v, (int)a->f);
}

static void vertex_kick(uint64_t xyz, int has_fog, int draw)
{
    int attr = prim_attr();
    gs_ctx *c = &ctx[(attr >> 9) & 1];
    vtx v;
    float ofx = (float)BITS(c->xyoffset, 0, 16) / 16.0f;
    float ofy = (float)BITS(c->xyoffset, 32, 16) / 16.0f;
    v.x = (float)BITS(xyz, 0, 16) / 16.0f - ofx;
    v.y = (float)BITS(xyz, 16, 16) / 16.0f - ofy;
    if (has_fog) {
        v.z = BITS(xyz, 32, 24);
        v.f = (float)BITS(xyz, 56, 8);
    } else {
        v.z = (uint32_t)(xyz >> 32);
        v.f = (float)BITS(r_fog, 56, 8);
    }
    v.r = (float)BITS(r_rgbaq, 0, 8);
    v.g = (float)BITS(r_rgbaq, 8, 8);
    v.b = (float)BITS(r_rgbaq, 16, 8);
    v.a = (float)BITS(r_rgbaq, 24, 8);
    uint32_t qbits = (uint32_t)(r_rgbaq >> 32), sbits = (uint32_t)r_st, tbits = (uint32_t)(r_st >> 32);
    memcpy(&v.q, &qbits, 4);
    memcpy(&v.s, &sbits, 4);
    memcpy(&v.t, &tbits, 4);
    v.u = (float)BITS(r_uv, 0, 14) / 16.0f;
    v.v = (float)BITS(r_uv, 16, 14) / 16.0f;

    int prim = (int)(r_prim & 7);
    switch (prim) {
    case 0:
        if (draw)
            draw_point(&v);
        break;
    case 1: /* line */
        vq[vcount++] = v;
        if (vcount == 2) {
            if (draw)
                draw_line(&vq[0], &vq[1]);
            vcount = 0;
        }
        break;
    case 2: /* line strip */
        vq[vcount++] = v;
        if (vcount == 2) {
            if (draw)
                draw_line(&vq[0], &vq[1]);
            vq[0] = vq[1];
            vcount = 1;
        }
        break;
    case 3: /* triangle */
        vq[vcount++] = v;
        if (vcount == 3) {
            if (draw)
                draw_triangle(&vq[0], &vq[1], &vq[2]);
            vcount = 0;
        }
        break;
    case 4: /* triangle strip */
        vq[vcount++] = v;
        if (vcount == 3) {
            if (draw)
                draw_triangle(&vq[0], &vq[1], &vq[2]);
            vq[0] = vq[1];
            vq[1] = vq[2];
            vcount = 2;
        }
        break;
    case 5: /* triangle fan */
        vq[vcount++] = v;
        if (vcount == 3) {
            if (draw)
                draw_triangle(&vq[0], &vq[1], &vq[2]);
            vq[1] = vq[2];
            vcount = 2;
        }
        break;
    case 6: /* sprite */
        vq[vcount++] = v;
        if (vcount == 2) {
            if (draw)
                draw_sprite(&vq[0], &vq[1]);
            vcount = 0;
        }
        break;
    default:
        vcount = 0;
        break;
    }
    (void)strip_fan_first;
}

/* ------------------------------------------------------------ transfers */
static struct {
    int active;
    uint32_t x, y;
    uint32_t bits_buf;
    int bits_len;
} xfer;

static void transfer_start(void)
{
    uint32_t dir = BITS(r_trxdir, 0, 2);
    if (dir == 0) {
        xfer.active = 1;
        xfer.x = 0;
        xfer.y = 0;
        xfer.bits_len = 0;
        xfer.bits_buf = 0;
    } else if (dir == 2) {
        uint32_t sbp = BITS(r_bitbltbuf, 0, 14), sbw = BITS(r_bitbltbuf, 16, 6), spsm = BITS(r_bitbltbuf, 24, 6);
        uint32_t dbp = BITS(r_bitbltbuf, 32, 14), dbw = BITS(r_bitbltbuf, 48, 6), dpsm = BITS(r_bitbltbuf, 56, 6);
        uint32_t sx = BITS(r_trxpos, 0, 11), sy = BITS(r_trxpos, 16, 11);
        uint32_t dx = BITS(r_trxpos, 32, 11), dy = BITS(r_trxpos, 48, 11);
        uint32_t w = BITS(r_trxreg, 0, 12), h = BITS(r_trxreg, 32, 12);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                pix_write(dbp, dbw, (int)dpsm, (dx + x) & 2047, (dy + y) & 2047,
                          pix_read(sbp, sbw, (int)spsm, (sx + x) & 2047, (sy + y) & 2047));
        xfer.active = 0;
    } else if (dir == 1) {
        ps2_warn_once("gsdownload", "GS local->host transfer is not emulated");
        xfer.active = 0;
    } else {
        xfer.active = 0;
    }
}

static void transfer_pixel(uint32_t v)
{
    uint32_t dbp = BITS(r_bitbltbuf, 32, 14), dbw = BITS(r_bitbltbuf, 48, 6), dpsm = BITS(r_bitbltbuf, 56, 6);
    uint32_t dx = BITS(r_trxpos, 32, 11), dy = BITS(r_trxpos, 48, 11);
    uint32_t w = BITS(r_trxreg, 0, 12), h = BITS(r_trxreg, 32, 12);
    if (!xfer.active || !w || xfer.y >= h) {
        xfer.active = 0;
        return;
    }
    pix_write(dbp, dbw, (int)dpsm, (dx + xfer.x) & 2047, (dy + xfer.y) & 2047, v);
    if (++xfer.x >= w) {
        xfer.x = 0;
        if (++xfer.y >= h)
            xfer.active = 0;
    }
}

void gs_image_data(const uint8_t *data, uint32_t qwc)
{
    uint32_t dpsm = BITS(r_bitbltbuf, 56, 6);
    uint32_t n = qwc * 16;
    switch (dpsm) {
    case PSMCT32: case PSMZ32:
        for (uint32_t i = 0; i + 4 <= n && xfer.active; i += 4)
            transfer_pixel(rd32le(data + i));
        break;
    case PSMCT24: case PSMZ24: case PSMT8H:
        if (dpsm == PSMT8H) {
            for (uint32_t i = 0; i < n && xfer.active; i++)
                transfer_pixel(data[i]);
        } else {
            for (uint32_t i = 0; i < n && xfer.active; i++) {
                xfer.bits_buf |= (uint32_t)data[i] << xfer.bits_len;
                xfer.bits_len += 8;
                if (xfer.bits_len == 24) {
                    transfer_pixel(xfer.bits_buf);
                    xfer.bits_buf = 0;
                    xfer.bits_len = 0;
                }
            }
        }
        break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        for (uint32_t i = 0; i + 2 <= n && xfer.active; i += 2)
            transfer_pixel((uint32_t)data[i] | ((uint32_t)data[i + 1] << 8));
        break;
    case PSMT8:
        for (uint32_t i = 0; i < n && xfer.active; i++)
            transfer_pixel(data[i]);
        break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        for (uint32_t i = 0; i < n && xfer.active; i++) {
            transfer_pixel(data[i] & 15);
            if (xfer.active)
                transfer_pixel(data[i] >> 4);
        }
        break;
    default:
        ps2_warn_once("xferpsm", "image transfer in unsupported format %02X", dpsm);
        break;
    }
}

/* ------------------------------------------------------------ registers */
void gs_write_reg(int reg, uint64_t v)
{
    switch (reg) {
    case 0x00: r_prim = v & 0x7FF; vcount = 0; break;
    case 0x01: r_rgbaq = v; break;
    case 0x02: r_st = v; break;
    case 0x03: r_uv = v; break;
    case 0x04: vertex_kick(v, 1, 1); break;
    case 0x05: vertex_kick(v, 0, 1); break;
    case 0x06: ctx[0].tex0 = v; tex0_written(v); break;
    case 0x07: ctx[1].tex0 = v; tex0_written(v); break;
    case 0x08: ctx[0].clamp = v; break;
    case 0x09: ctx[1].clamp = v; break;
    case 0x0A: r_fog = v; break;
    case 0x0C: vertex_kick(v, 1, 0); break;
    case 0x0D: vertex_kick(v, 0, 0); break;
    case 0x14: ctx[0].tex1 = v; break;
    case 0x15: ctx[1].tex1 = v; break;
    case 0x16: case 0x17: { /* TEX2: update the format/CLUT fields of TEX0 */
        /* PSM (20-25) and CBP/CPSM/CSM/CSA/CLD (37-63) */
        const uint64_t mask = (0x3Full << 20) | (0x7FFFFFFull << 37);
        gs_ctx *c = &ctx[reg - 0x16];
        c->tex0 = (c->tex0 & ~mask) | (v & mask);
        tex0_written(c->tex0);
        break;
    }
    case 0x18: ctx[0].xyoffset = v; break;
    case 0x19: ctx[1].xyoffset = v; break;
    case 0x1A: r_prmodecont = v; break;
    case 0x1B: r_prmode = v; break;
    case 0x1C: r_texclut = v; break;
    case 0x22: r_scanmsk = v; break;
    case 0x34: ctx[0].miptbp1 = v; break;
    case 0x35: ctx[1].miptbp1 = v; break;
    case 0x36: case 0x37: break;
    case 0x3B: r_texa = v; break;
    case 0x3D: r_fogcol = v; break;
    case 0x3F: break;   /* TEXFLUSH */
    case 0x40: ctx[0].scissor = v; break;
    case 0x41: ctx[1].scissor = v; break;
    case 0x42: ctx[0].alpha = v; break;
    case 0x43: ctx[1].alpha = v; break;
    case 0x44: r_dimx = v; break;
    case 0x45: r_dthe = v; break;
    case 0x46: r_colclamp = v; break;
    case 0x47: ctx[0].test = v; break;
    case 0x48: ctx[1].test = v; break;
    case 0x49: r_pabe = v; break;
    case 0x4A: ctx[0].fba = v; break;
    case 0x4B: ctx[1].fba = v; break;
    case 0x4C: ctx[0].frame = v; break;
    case 0x4D: ctx[1].frame = v; break;
    case 0x4E: ctx[0].zbuf = v; break;
    case 0x4F: ctx[1].zbuf = v; break;
    case 0x50: r_bitbltbuf = v; break;
    case 0x51: r_trxpos = v; break;
    case 0x52: r_trxreg = v; break;
    case 0x53: r_trxdir = v; transfer_start(); break;
    case 0x54: { /* HWREG */
        uint8_t b[16];
        memcpy(b, &v, 8);
        memset(b + 8, 0, 8);
        if (xfer.active) {
            /* Only half a quadword: feed 8 bytes through the same path. */
            uint32_t dpsm = BITS(r_bitbltbuf, 56, 6);
            if (dpsm == PSMCT32 || dpsm == PSMZ32) {
                transfer_pixel(rd32le(b));
                transfer_pixel(rd32le(b + 4));
            } else {
                ps2_warn_once("hwreg", "HWREG transfers only supported for 32-bit formats");
            }
        }
        break;
    }
    case 0x60: { /* SIGNAL */
        uint32_t id = (uint32_t)v, mask = (uint32_t)(v >> 32);
        p_siglblid = (p_siglblid & ~(uint64_t)mask) | (id & mask);
        p_csr |= 1;
        if (!(p_imr & 0x100))
            hw_raise_intc(INTC_GS);
        break;
    }
    case 0x61: /* FINISH */
        p_csr |= 2;
        if (!(p_imr & 0x200))
            hw_raise_intc(INTC_GS);
        break;
    case 0x62: { /* LABEL */
        uint32_t id = (uint32_t)v, mask = (uint32_t)(v >> 32);
        uint64_t hi = p_siglblid >> 32;
        hi = (hi & ~(uint64_t)mask) | (id & mask);
        p_siglblid = (p_siglblid & 0xFFFFFFFFull) | (hi << 32);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------ GIF */
typedef struct gif_path {
    int active;
    uint32_t nloop, nreg, flg, eop;
    uint64_t regs;
    uint32_t reg_idx;
} gif_path;

static gif_path paths[4];

void gs_gif_tag(const u128 *tag)
{
    (void)tag;
    memset(paths, 0, sizeof paths);
}

static void gif_packed(const u128 *q, int reg)
{
    switch (reg) {
    case 0x0: gs_write_reg(0x00, q->UD[0] & 0x7FF); break;
    case 0x1: {
        uint32_t qb;
        memcpy(&qb, &packed_q, 4);
        uint64_t v = (q->UW[0] & 0xFF) | ((q->UW[1] & 0xFF) << 8) | ((q->UW[2] & 0xFF) << 16) |
                     ((uint64_t)(q->UW[3] & 0xFF) << 24) | ((uint64_t)qb << 32);
        gs_write_reg(0x01, v);
        break;
    }
    case 0x2:
        memcpy(&packed_q, &q->UW[2], 4);
        gs_write_reg(0x02, (uint64_t)q->UW[0] | ((uint64_t)q->UW[1] << 32));
        break;
    case 0x3:
        gs_write_reg(0x03, (q->UW[0] & 0x3FFF) | ((uint64_t)(q->UW[1] & 0x3FFF) << 16));
        break;
    case 0x4: {
        uint64_t v = (q->UW[0] & 0xFFFF) | ((uint64_t)(q->UW[1] & 0xFFFF) << 16) |
                     ((uint64_t)((q->UW[2] >> 4) & 0xFFFFFF) << 32) |
                     ((uint64_t)((q->UW[3] >> 4) & 0xFF) << 56);
        gs_write_reg((q->UW[3] >> 15) & 1 ? 0x0C : 0x04, v);
        break;
    }
    case 0x5: {
        uint64_t v = (q->UW[0] & 0xFFFF) | ((uint64_t)(q->UW[1] & 0xFFFF) << 16) |
                     ((uint64_t)q->UW[2] << 32);
        gs_write_reg((q->UW[3] >> 15) & 1 ? 0x0D : 0x05, v);
        break;
    }
    case 0xA:
        gs_write_reg(0x0A, (uint64_t)((q->UW[3] >> 4) & 0xFF) << 56);
        break;
    case 0xE:
        gs_write_reg((int)(q->UD[1] & 0xFF), q->UD[0]);
        break;
    case 0xF:
        break;
    default:
        gs_write_reg(reg, q->UD[0]);
        break;
    }
}

void gif_packet(const uint8_t *data, uint32_t qwc, int path)
{
    gif_path *p = &paths[path & 3];
    for (uint32_t i = 0; i < qwc; i++) {
        u128 q;
        memcpy(&q, data + i * 16, 16);
        if (!p->active) {
            p->nloop = (uint32_t)(q.UD[0] & 0x7FFF);
            p->eop = (uint32_t)(q.UD[0] >> 15) & 1;
            uint32_t pre = (uint32_t)(q.UD[0] >> 46) & 1;
            uint32_t prim = (uint32_t)(q.UD[0] >> 47) & 0x7FF;
            p->flg = (uint32_t)(q.UD[0] >> 58) & 3;
            p->nreg = (uint32_t)(q.UD[0] >> 60) & 15;
            if (!p->nreg)
                p->nreg = 16;
            p->regs = q.UD[1];
            p->reg_idx = 0;
            if (pre && p->flg == 0)
                gs_write_reg(0x00, prim);
            packed_q = 1.0f;
            p->active = p->nloop > 0;
            continue;
        }
        if (p->flg == 0) { /* PACKED */
            int reg = (int)((p->regs >> (p->reg_idx * 4)) & 15);
            gif_packed(&q, reg);
            if (++p->reg_idx >= p->nreg) {
                p->reg_idx = 0;
                if (--p->nloop == 0)
                    p->active = 0;
            }
        } else if (p->flg == 1) { /* REGLIST */
            for (int half = 0; half < 2 && p->active; half++) {
                int reg = (int)((p->regs >> (p->reg_idx * 4)) & 15);
                if (reg != 0xE && reg != 0xF)
                    gs_write_reg(reg, q.UD[half]);
                /* Loops are packed back to back; if the total word count is
                 * odd, the final half quadword is padding. */
                if (++p->reg_idx >= p->nreg) {
                    p->reg_idx = 0;
                    if (--p->nloop == 0)
                        p->active = 0;
                }
            }
        } else { /* IMAGE */
            gs_image_data((const uint8_t *)&q, 1);
            if (--p->nloop == 0)
                p->active = 0;
        }
    }
}

/* ------------------------------------------------------------ privileged */
void gs_priv_write64(uint32_t a, uint64_t v)
{
    if (a >= 0x12001000) {
        switch (a & 0xFF0) {
        case 0x000: /* CSR */
            p_csr &= ~(v & 0x1F);
            if (v & 0x200) { /* RESET */
                p_csr = 0;
                memset(paths, 0, sizeof paths);
            }
            break;
        case 0x010: p_imr = v; break;
        case 0x040: p_busdir = v; break;
        case 0x080: p_siglblid = v; break;
        default: break;
        }
        return;
    }
    switch (a & 0xFF0) {
    case 0x0000: p_pmode = v; break;
    case 0x0010: p_smode1 = v; break;
    case 0x0020: p_smode2 = v; break;
    case 0x0030: p_srfsh = v; break;
    case 0x0040: p_synch1 = v; break;
    case 0x0050: p_synch2 = v; break;
    case 0x0060: p_syncv = v; break;
    case 0x0070: p_dispfb[0] = v; break;
    case 0x0080: p_display[0] = v; break;
    case 0x0090: p_dispfb[1] = v; break;
    case 0x00A0: p_display[1] = v; break;
    case 0x00B0: p_extbuf = v; break;
    case 0x00C0: p_extdata = v; break;
    case 0x00D0: p_extwrite = v; break;
    case 0x00E0: p_bgcolor = v; break;
    default: break;
    }
}

uint64_t gs_priv_read64(uint32_t a)
{
    if (a >= 0x12001000) {
        switch (a & 0xFF0) {
        case 0x000:
            hw_update_time();
            return p_csr | (1u << 14) | (0x551Bull << 16);
        case 0x010: return p_imr;
        case 0x040: return p_busdir;
        case 0x080: return p_siglblid;
        default: return 0;
        }
    }
    switch (a & 0xFF0) {
    case 0x00: return p_pmode;
    case 0x10: return p_smode1;
    case 0x20: return p_smode2;
    case 0x70: return p_dispfb[0];
    case 0x80: return p_display[0];
    case 0x90: return p_dispfb[1];
    case 0xA0: return p_display[1];
    case 0xE0: return p_bgcolor;
    default: return 0;
    }
}

uint32_t gs_imr(void) { return (uint32_t)p_imr; }
void gs_set_imr(uint32_t v) { p_imr = v; }

void gs_set_crt(int interlace, int mode, int ffmd)
{
    extern void hw_set_video_rate(int pal);
    p_smode2 = (uint64_t)(interlace & 1) | ((uint64_t)(ffmd & 1) << 1);
    hw_set_video_rate(mode == 3);
}

void gs_vblank(int start)
{
    if (start) {
        p_csr |= 8;
        p_csr ^= 1u << 13; /* FIELD */
        if (!(p_imr & 0x800))
            hw_raise_intc(INTC_GS);
        display_present();
    }
}

int gs_read_display(uint32_t *dst, int *outw, int *outh, int maxw, int maxh)
{
    int circuit = -1;
    if (p_pmode & 1)
        circuit = 0;
    else if (p_pmode & 2)
        circuit = 1;
    if (circuit < 0)
        return 0;
    uint64_t fb = p_dispfb[circuit], disp = p_display[circuit];
    uint32_t fbp = BITS(fb, 0, 9) * 32, fbw = BITS(fb, 9, 6), psm = BITS(fb, 15, 5);
    uint32_t dbx = BITS(fb, 32, 11), dby = BITS(fb, 43, 11);
    uint32_t magh = BITS(disp, 23, 4), magv = BITS(disp, 27, 2);
    uint32_t dw = BITS(disp, 32, 12), dh = BITS(disp, 44, 11);
    int w = (int)((dw + 1) / (magh + 1));
    int h = (int)((dh + 1) / (magv + 1));
    if (w <= 0 || h <= 0 || !fbw)
        return 0;
    if ((p_smode2 & 3) == 3 && h > 300)
        h /= 2;  /* interlaced frame mode: each field shows half the lines */
    if (w > maxw) w = maxw;
    if (h > maxh) h = maxh;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = dbx + (uint32_t)x, py = dby + (uint32_t)y;
            uint32_t c;
            switch (psm) {
            case PSMCT32: case PSMCT24:
                c = pix_read(fbp, fbw, PSMCT32, px, py);
                break;
            case PSMCT16: case PSMCT16S:
                c = rgba16_to_32(pix_read(fbp, fbw, (int)psm, px, py), 0, 0, 0x80);
                break;
            default:
                c = pix_read(fbp, fbw, PSMCT32, px, py);
                break;
            }
            dst[y * maxw + x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
        }
    }
    *outw = w;
    *outh = h;
    return 1;
}

void gs_init(void)
{
    vram = calloc(VRAM_WORDS, 4);
    if (!vram)
        ps2_fatal("cannot allocate GS memory");
    memset(ctx, 0, sizeof ctx);
    for (int i = 0; i < 2; i++)
        ctx[i].scissor = (2047ull << 16) | (2047ull << 48);
}
