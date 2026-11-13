/*
 * VU0 macro-mode (COP2) instructions executed inline on the EE.
 * Field layout: dest = bits 24..21 (x y z w), ft = 20..16, fs = 15..11,
 * fd = 10..6, bc = 1..0.  vf0 is the constant (0,0,0,1); vi0 is 0.
 * Micro-mode (VCALLMS) is not emulated.
 */
#ifndef PS2_VU0_H
#define PS2_VU0_H

#define VU_FT(w) (((w) >> 16) & 31)
#define VU_FS(w) (((w) >> 11) & 31)
#define VU_FD(w) (((w) >> 6) & 31)
#define VU_DEST(w) (((w) >> 21) & 15)
#define VU_BC(w) ((w) & 3)
#define VU_FSF(w) (((w) >> 21) & 3)
#define VU_FTF(w) (((w) >> 23) & 3)

extern PS2_IMPORT uint8_t *ps2_vu0_mem;     /* 4 KiB data memory (EE 0x11004000) */

void vu0_callms(R5900 *c, uint32_t addr);
uint32_t vu0_cfc2(R5900 *c, int reg);
void vu0_ctc2(R5900 *c, int reg, uint32_t v);

static inline int vu0_cpcond2(R5900 *c) { (void)c; return 0; }

static inline void vu_store(R5900 *c, int reg, uint32_t dest, const float *r)
{
    if (reg == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (dest & (8 >> i))
            c->vf[reg].F[i] = fpu_clamp(r[i]);
}

static inline void vu_store_acc(R5900 *c, uint32_t dest, const float *r)
{
    for (int i = 0; i < 4; i++)
        if (dest & (8 >> i))
            c->vacc.F[i] = fpu_clamp(r[i]);
}

enum { VOP_ADD, VOP_SUB, VOP_MUL, VOP_MADD, VOP_MSUB, VOP_MAX, VOP_MIN };
enum { VSRC_FT, VSRC_BC, VSRC_Q, VSRC_I };

static inline void vu_arith(R5900 *c, uint32_t w, int op, int src, int to_acc)
{
    const u128 s = c->vf[VU_FS(w)];
    const u128 t = c->vf[VU_FT(w)];
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = fpu_in(s.F[i]), b;
        switch (src) {
        case VSRC_FT: b = t.F[i]; break;
        case VSRC_BC: b = t.F[VU_BC(w)]; break;
        case VSRC_Q: b = c->vq; break;
        default: b = c->vi_reg; break;
        }
        b = fpu_in(b);
        switch (op) {
        case VOP_ADD: r[i] = a + b; break;
        case VOP_SUB: r[i] = a - b; break;
        case VOP_MUL: r[i] = a * b; break;
        case VOP_MADD: r[i] = fpu_in(c->vacc.F[i]) + fpu_clamp(a * b); break;
        case VOP_MSUB: r[i] = fpu_in(c->vacc.F[i]) - fpu_clamp(a * b); break;
        case VOP_MAX: r[i] = a > b ? a : b; break;
        default: r[i] = a < b ? a : b; break;
        }
    }
    if (to_acc)
        vu_store_acc(c, VU_DEST(w), r);
    else
        vu_store(c, VU_FD(w), VU_DEST(w), r);
}

#define VU_DEF(NAME, OP, SRC, ACC) \
    static inline void VU0_##NAME(R5900 *c, uint32_t w) { vu_arith(c, w, OP, SRC, ACC); }
#define VU_DEF_BC(BASE, OP, ACC)                                            \
    VU_DEF(BASE##X, OP, VSRC_BC, ACC) VU_DEF(BASE##Y, OP, VSRC_BC, ACC)     \
    VU_DEF(BASE##Z, OP, VSRC_BC, ACC) VU_DEF(BASE##W, OP, VSRC_BC, ACC)

VU_DEF(VADD, VOP_ADD, VSRC_FT, 0)
VU_DEF(VSUB, VOP_SUB, VSRC_FT, 0)
VU_DEF(VMUL, VOP_MUL, VSRC_FT, 0)
VU_DEF(VMADD, VOP_MADD, VSRC_FT, 0)
VU_DEF(VMSUB, VOP_MSUB, VSRC_FT, 0)
VU_DEF(VMAX, VOP_MAX, VSRC_FT, 0)
VU_DEF(VMINI, VOP_MIN, VSRC_FT, 0)
VU_DEF_BC(VADD, VOP_ADD, 0)
VU_DEF_BC(VSUB, VOP_SUB, 0)
VU_DEF_BC(VMUL, VOP_MUL, 0)
VU_DEF_BC(VMADD, VOP_MADD, 0)
VU_DEF_BC(VMSUB, VOP_MSUB, 0)
VU_DEF_BC(VMAX, VOP_MAX, 0)
VU_DEF_BC(VMINI, VOP_MIN, 0)
VU_DEF(VADDQ, VOP_ADD, VSRC_Q, 0)
VU_DEF(VSUBQ, VOP_SUB, VSRC_Q, 0)
VU_DEF(VMULQ, VOP_MUL, VSRC_Q, 0)
VU_DEF(VMADDQ, VOP_MADD, VSRC_Q, 0)
VU_DEF(VMSUBQ, VOP_MSUB, VSRC_Q, 0)
VU_DEF(VADDI, VOP_ADD, VSRC_I, 0)
VU_DEF(VSUBI, VOP_SUB, VSRC_I, 0)
VU_DEF(VMULI, VOP_MUL, VSRC_I, 0)
VU_DEF(VMADDI, VOP_MADD, VSRC_I, 0)
VU_DEF(VMSUBI, VOP_MSUB, VSRC_I, 0)
VU_DEF(VMAXI, VOP_MAX, VSRC_I, 0)
VU_DEF(VMINII, VOP_MIN, VSRC_I, 0)
VU_DEF(VADDA, VOP_ADD, VSRC_FT, 1)
VU_DEF(VSUBA, VOP_SUB, VSRC_FT, 1)
VU_DEF(VMULA, VOP_MUL, VSRC_FT, 1)
VU_DEF(VMADDA, VOP_MADD, VSRC_FT, 1)
VU_DEF(VMSUBA, VOP_MSUB, VSRC_FT, 1)
VU_DEF_BC(VADDA, VOP_ADD, 1)
VU_DEF_BC(VSUBA, VOP_SUB, 1)
VU_DEF_BC(VMULA, VOP_MUL, 1)
VU_DEF_BC(VMADDA, VOP_MADD, 1)
VU_DEF_BC(VMSUBA, VOP_MSUB, 1)
VU_DEF(VADDAQ, VOP_ADD, VSRC_Q, 1)
VU_DEF(VSUBAQ, VOP_SUB, VSRC_Q, 1)
VU_DEF(VMULAQ, VOP_MUL, VSRC_Q, 1)
VU_DEF(VMADDAQ, VOP_MADD, VSRC_Q, 1)
VU_DEF(VMSUBAQ, VOP_MSUB, VSRC_Q, 1)
VU_DEF(VADDAI, VOP_ADD, VSRC_I, 1)
VU_DEF(VSUBAI, VOP_SUB, VSRC_I, 1)
VU_DEF(VMULAI, VOP_MUL, VSRC_I, 1)
VU_DEF(VMADDAI, VOP_MADD, VSRC_I, 1)
VU_DEF(VMSUBAI, VOP_MSUB, VSRC_I, 1)

static inline void VU0_VOPMULA(R5900 *c, uint32_t w)
{
    const u128 s = c->vf[VU_FS(w)], t = c->vf[VU_FT(w)];
    float r[4] = {s.F[1] * t.F[2], s.F[2] * t.F[0], s.F[0] * t.F[1], 0};
    vu_store_acc(c, VU_DEST(w) & 0xE, r);
}

static inline void VU0_VOPMSUB(R5900 *c, uint32_t w)
{
    const u128 s = c->vf[VU_FS(w)], t = c->vf[VU_FT(w)];
    float r[4] = {c->vacc.F[0] - s.F[1] * t.F[2], c->vacc.F[1] - s.F[2] * t.F[0],
                  c->vacc.F[2] - s.F[0] * t.F[1], 0};
    vu_store(c, VU_FD(w), VU_DEST(w) & 0xE, r);
}

static inline void VU0_VABS(R5900 *c, uint32_t w)
{
    const u128 s = c->vf[VU_FS(w)];
    float r[4];
    for (int i = 0; i < 4; i++)
        r[i] = fabsf(s.F[i]);
    vu_store(c, VU_FT(w), VU_DEST(w), r);
}

static inline void VU0_VMOVE(R5900 *c, uint32_t w)
{
    int ft = VU_FT(w);
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].UW[i] = c->vf[VU_FS(w)].UW[i];
}

static inline void VU0_VMR32(R5900 *c, uint32_t w)
{
    const u128 s = c->vf[VU_FS(w)];
    int ft = VU_FT(w);
    const uint32_t rot[4] = {s.UW[1], s.UW[2], s.UW[3], s.UW[0]};
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].UW[i] = rot[i];
}

static inline void vu_itof(R5900 *c, uint32_t w, int shift)
{
    const u128 s = c->vf[VU_FS(w)];
    int ft = VU_FT(w);
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].F[i] = (float)s.SW[i] / (float)(1 << shift);
}

static inline void vu_ftoi(R5900 *c, uint32_t w, int shift)
{
    const u128 s = c->vf[VU_FS(w)];
    int ft = VU_FT(w);
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].SW[i] = fpu_cvtw(fpu_in(s.F[i]) * (float)(1 << shift));
}

static inline void VU0_VITOF0(R5900 *c, uint32_t w) { vu_itof(c, w, 0); }
static inline void VU0_VITOF4(R5900 *c, uint32_t w) { vu_itof(c, w, 4); }
static inline void VU0_VITOF12(R5900 *c, uint32_t w) { vu_itof(c, w, 12); }
static inline void VU0_VITOF15(R5900 *c, uint32_t w) { vu_itof(c, w, 15); }
static inline void VU0_VFTOI0(R5900 *c, uint32_t w) { vu_ftoi(c, w, 0); }
static inline void VU0_VFTOI4(R5900 *c, uint32_t w) { vu_ftoi(c, w, 4); }
static inline void VU0_VFTOI12(R5900 *c, uint32_t w) { vu_ftoi(c, w, 12); }
static inline void VU0_VFTOI15(R5900 *c, uint32_t w) { vu_ftoi(c, w, 15); }

static inline void VU0_VCLIPW(R5900 *c, uint32_t w)
{
    const u128 s = c->vf[VU_FS(w)];
    float lim = fabsf(c->vf[VU_FT(w)].F[3]);
    uint32_t f = 0;
    if (s.F[0] > lim) f |= 0x01;
    if (s.F[0] < -lim) f |= 0x02;
    if (s.F[1] > lim) f |= 0x04;
    if (s.F[1] < -lim) f |= 0x08;
    if (s.F[2] > lim) f |= 0x10;
    if (s.F[2] < -lim) f |= 0x20;
    c->vu_clip = ((c->vu_clip << 6) | f) & 0xFFFFFF;
}

static inline void VU0_VDIV(R5900 *c, uint32_t w)
{
    float a = c->vf[VU_FS(w)].F[VU_FSF(w)], b = c->vf[VU_FT(w)].F[VU_FTF(w)];
    if (b == 0.0f) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        c->vq = fpu_signed_max(ua ^ ub);
        c->vu_status |= a == 0.0f ? 0x10 : 0x20;
    } else {
        c->vq = fpu_clamp(fpu_in(a) / fpu_in(b));
    }
}

static inline void VU0_VSQRT(R5900 *c, uint32_t w)
{
    c->vq = sqrtf(fabsf(fpu_in(c->vf[VU_FT(w)].F[VU_FTF(w)])));
}

static inline void VU0_VRSQRT(R5900 *c, uint32_t w)
{
    float a = c->vf[VU_FS(w)].F[VU_FSF(w)];
    float b = fabsf(c->vf[VU_FT(w)].F[VU_FTF(w)]);
    if (b == 0.0f) {
        uint32_t ua;
        memcpy(&ua, &a, 4);
        c->vq = fpu_signed_max(ua);
        c->vu_status |= 0x20;
    } else {
        c->vq = fpu_clamp(fpu_in(a) / sqrtf(fpu_in(b)));
    }
}

static inline void VU0_VWAITQ(R5900 *c, uint32_t w) { (void)c; (void)w; }
static inline void VU0_VNOP(R5900 *c, uint32_t w) { (void)c; (void)w; }

/* Integer unit: it = ft field, is = fs field, id = fd field. */
static inline void vu_setvi(R5900 *c, int r, uint32_t v)
{
    if (r)
        c->vi[r & 15] = (uint16_t)v;
}
static inline void VU0_VIADD(R5900 *c, uint32_t w) { vu_setvi(c, VU_FD(w), c->vi[VU_FS(w) & 15] + c->vi[VU_FT(w) & 15]); }
static inline void VU0_VISUB(R5900 *c, uint32_t w) { vu_setvi(c, VU_FD(w), c->vi[VU_FS(w) & 15] - c->vi[VU_FT(w) & 15]); }
static inline void VU0_VIAND(R5900 *c, uint32_t w) { vu_setvi(c, VU_FD(w), c->vi[VU_FS(w) & 15] & c->vi[VU_FT(w) & 15]); }
static inline void VU0_VIOR(R5900 *c, uint32_t w) { vu_setvi(c, VU_FD(w), c->vi[VU_FS(w) & 15] | c->vi[VU_FT(w) & 15]); }
static inline void VU0_VIADDI(R5900 *c, uint32_t w)
{
    int32_t imm = (int32_t)((w >> 6) & 31);
    if (imm & 16)
        imm -= 32;
    vu_setvi(c, VU_FT(w), (uint32_t)(c->vi[VU_FS(w) & 15] + imm));
}

static inline void VU0_VMTIR(R5900 *c, uint32_t w)
{
    vu_setvi(c, VU_FT(w), c->vf[VU_FS(w)].UW[VU_FSF(w)] & 0xFFFF);
}

static inline void VU0_VMFIR(R5900 *c, uint32_t w)
{
    int ft = VU_FT(w);
    int32_t v = (int16_t)c->vi[VU_FS(w) & 15];
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].SW[i] = v;
}

static inline uint8_t *vu0_qword(uint32_t index)
{
    return ps2_vu0_mem + ((index & 0xFF) << 4);
}

static inline void VU0_VILWR(R5900 *c, uint32_t w)
{
    uint8_t *q = vu0_qword(c->vi[VU_FS(w) & 15]);
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i)) {
            uint16_t v;
            memcpy(&v, q + i * 4, 2);
            vu_setvi(c, VU_FT(w), v);
            break;
        }
}

static inline void VU0_VISWR(R5900 *c, uint32_t w)
{
    uint8_t *q = vu0_qword(c->vi[VU_FS(w) & 15]);
    uint32_t v = c->vi[VU_FT(w) & 15];
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            memcpy(q + i * 4, &v, 4);
}

static inline void vu_lq(R5900 *c, uint32_t w, uint32_t index)
{
    int ft = VU_FT(w);
    uint8_t *q = vu0_qword(index);
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            memcpy(&c->vf[ft].UW[i], q + i * 4, 4);
}

static inline void vu_sq(R5900 *c, uint32_t w, uint32_t index)
{
    uint8_t *q = vu0_qword(index);
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            memcpy(q + i * 4, &c->vf[VU_FS(w)].UW[i], 4);
}

static inline void VU0_VLQI(R5900 *c, uint32_t w)
{
    int is = VU_FS(w) & 15;
    vu_lq(c, w, c->vi[is]);
    vu_setvi(c, is, c->vi[is] + 1u);
}
static inline void VU0_VLQD(R5900 *c, uint32_t w)
{
    int is = VU_FS(w) & 15;
    vu_setvi(c, is, c->vi[is] - 1u);
    vu_lq(c, w, c->vi[is]);
}
static inline void VU0_VSQI(R5900 *c, uint32_t w)
{
    int it = VU_FT(w) & 15;
    vu_sq(c, w, c->vi[it]);
    vu_setvi(c, it, c->vi[it] + 1u);
}
static inline void VU0_VSQD(R5900 *c, uint32_t w)
{
    int it = VU_FT(w) & 15;
    vu_setvi(c, it, c->vi[it] - 1u);
    vu_sq(c, w, c->vi[it]);
}

static inline void vu_rstore(R5900 *c, uint32_t w)
{
    int ft = VU_FT(w);
    if (ft == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (VU_DEST(w) & (8 >> i))
            c->vf[ft].UW[i] = 0x3F800000u | (c->vu_r & 0x7FFFFF);
}
static inline void VU0_VRINIT(R5900 *c, uint32_t w)
{
    c->vu_r = 0x3F800000u | (c->vf[VU_FS(w)].UW[VU_FSF(w)] & 0x7FFFFF);
}
static inline void VU0_VRXOR(R5900 *c, uint32_t w)
{
    c->vu_r = 0x3F800000u | ((c->vu_r ^ c->vf[VU_FS(w)].UW[VU_FSF(w)]) & 0x7FFFFF);
}
static inline void VU0_VRGET(R5900 *c, uint32_t w) { vu_rstore(c, w); }
static inline void VU0_VRNEXT(R5900 *c, uint32_t w)
{
    uint32_t x = c->vu_r;
    int bit = ((x >> 4) ^ (x >> 22)) & 1;
    c->vu_r = 0x3F800000u | (((x << 1) | (uint32_t)bit) & 0x7FFFFF);
    vu_rstore(c, w);
}

/* Micro address in 64-bit instruction units. */
static inline void VU0_VCALLMS(R5900 *c, uint32_t w) { vu0_callms(c, (w >> 6) & 0x7FFF); }
static inline void VU0_VCALLMSR(R5900 *c, uint32_t w) { (void)w; vu0_callms(c, c->vu_cmsar0); }

#endif /* PS2_VU0_H */
