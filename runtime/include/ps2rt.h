/*
 * ps2native runtime - core definitions shared by generated code and runtime.
 *
 * Guest state model: one R5900 context per PS2 thread.  Guest memory is
 * reached through a 4 KiB page map; unmapped pages (hardware registers)
 * fall back to ps2_io_* handlers.
 */
#ifndef PS2RT_H
#define PS2RT_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime data that generated code reads.  Code compiled at run time into a
 * DLL (PS2_DYN_MODULE, see runtime/src/core/dyn.c) imports it from the exe. */
#if defined(PS2_DYN_MODULE) && defined(_MSC_VER)
#define PS2_IMPORT __declspec(dllimport)
#else
#define PS2_IMPORT
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PS2_ALIGN16 __attribute__((aligned(16)))
#define PS2_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define PS2_NORETURN_PRE
#define PS2_NORETURN_POST __attribute__((noreturn))
static inline uint32_t ps2_clz32(uint32_t v) { return (uint32_t)__builtin_clz(v); }
#else /* MSVC */
#include <intrin.h>
#define PS2_LIKELY(x) (x)
#define PS2_UNLIKELY(x) (x)
#define PS2_ALIGN16 __declspec(align(16))
#define PS2_PRINTF(f, a)
#define PS2_NORETURN_PRE __declspec(noreturn)
#define PS2_NORETURN_POST
static __inline uint32_t ps2_clz32(uint32_t v)
{
    unsigned long idx;
    _BitScanReverse(&idx, v);
    return 31u - (uint32_t)idx;
}
#endif

typedef union PS2_ALIGN16 u128 {
    uint64_t UD[2];
    int64_t SD[2];
    uint32_t UW[4];
    int32_t SW[4];
    uint16_t US[8];
    int16_t SS[8];
    uint8_t UB[16];
    int8_t SB[16];
    float F[4];
} u128;

typedef union fpr {
    float f;
    uint32_t u;
    int32_t s;
} fpr;

struct ps2_thread;

typedef struct R5900 {
    u128 r[32];
    u128 hi, lo;            /* UD[0] = HI/LO, UD[1] = HI1/LO1 */
    uint32_t sa;            /* shift amount in bytes (QFSRV) */
    uint32_t pc;            /* last syscall pc (diagnostics) */
    uint32_t jump_pc;       /* secondary entry request for dispatcher */
    uint32_t pad0;
    fpr f[32];
    fpr facc;
    uint32_t fcr31;
    /* VU0 macro mode state */
    u128 vf[32];
    u128 vacc;
    uint16_t vi[16];
    float vq, vi_reg, vp;
    uint32_t vu_status, vu_mac, vu_clip, vu_r;
    uint32_t vu_cmsar0;
    struct ps2_thread *thread;
} R5900;

typedef void (*ps2_fn)(R5900 *c);

typedef struct ps2_func_entry {
    uint32_t addr;
    uint32_t secondary;     /* 1: entry is a label inside fn */
    ps2_fn fn;
} ps2_func_entry;

/* Entry of a module compiled at run time (overlay code, see dyn.c): the first
 * instruction words let the runtime check that the code in memory is still
 * the code that was compiled. */
typedef struct ps2_dyn_entry {
    uint32_t addr;
    uint32_t secondary;
    ps2_fn fn;
    uint32_t words[4];
} ps2_dyn_entry;
/* Original bodies of the functions replaced through the game config "hle". */
typedef struct ps2_hle_orig { const char *name; ps2_fn fn; } ps2_hle_orig;
extern const ps2_hle_orig ps2_hle_origs[];
/* Run the original (recompiled) code of an HLE'd function on the current context. */
int ps2_hle_original(R5900 *c, const char *name);
/* Call guest code at `addr` with up to 4 arguments; returns v0.  Registers the
 * callee may clobber are restored for the HLE caller. */
uint32_t ps2_hle_call_guest(R5900 *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
/* Function-level HLE hook (game config "hle"): returns 1 if handled natively. */
int ps2_hle(R5900 *c, int *id, const char *name);
ps2_fn ps2_dyn_lookup(uint32_t addr, int *secondary);

/* Generated tables (functable.c) */
extern const ps2_func_entry ps2_func_table[];
extern const uint32_t ps2_func_count;
extern const char ps2_game_name[];
/* Runtime pnach patches (data writes / conditional codes), applied each vblank. */
typedef struct ps2_patch {
    uint32_t addr, value;
    uint32_t cond_addr, cond_value;   /* cond_addr != 0: only if u16 at cond_addr == cond_value */
    uint32_t size;                    /* 1, 2, 4 */
    int32_t option;                   /* >= 0: only while ps2_opt_flags[option] */
} ps2_patch;
extern const ps2_patch ps2_patches[];
extern const uint32_t ps2_patch_count;
/* Game options switchable at run time (ps2native.ini: <name>=1). */
#define PS2_MAX_OPTIONS 16
extern const char *const ps2_option_names[];
extern const uint32_t ps2_option_count;
extern PS2_IMPORT uint8_t ps2_opt_flags[PS2_MAX_OPTIONS];
/* The original ELF, embedded in the executable (incbin or a Win32 resource). */
const unsigned char *ps2_get_elf_image(uint32_t *size);

/* ------------------------------------------------------------- memory */
#define PS2_RAM_SIZE  (32u * 1024 * 1024)
#define PS2_PAGE_BITS 12

extern PS2_IMPORT uint8_t *ps2_ram;
extern PS2_IMPORT uint8_t *ps2_spr;            /* 16 KiB scratchpad */
extern PS2_IMPORT uint8_t *ps2_pagemap[1u << (32 - PS2_PAGE_BITS)];

uint8_t ps2_io_read8(uint32_t a);
uint16_t ps2_io_read16(uint32_t a);
uint32_t ps2_io_read32(uint32_t a);
uint64_t ps2_io_read64(uint32_t a);
void ps2_io_read128(uint32_t a, u128 *v);
void ps2_io_write8(uint32_t a, uint8_t v);
void ps2_io_write16(uint32_t a, uint16_t v);
void ps2_io_write32(uint32_t a, uint32_t v);
void ps2_io_write64(uint32_t a, uint64_t v);
void ps2_io_write128(uint32_t a, const u128 *v);

static inline uint8_t *ps2_hostptr(uint32_t a)
{
    uint8_t *p = ps2_pagemap[a >> PS2_PAGE_BITS];
    return p ? p + (a & ((1u << PS2_PAGE_BITS) - 1)) : (uint8_t *)0;
}

#define PS2_MEM_RW(bits, type)                                              \
    static inline type ps2_read##bits(uint32_t a)                           \
    {                                                                       \
        uint8_t *p = ps2_pagemap[a >> PS2_PAGE_BITS];                       \
        if (PS2_LIKELY(p)) {                                                \
            type v;                                                         \
            memcpy(&v, p + (a & 0xFFFu), sizeof v);                         \
            return v;                                                       \
        }                                                                   \
        return ps2_io_read##bits(a);                                        \
    }                                                                       \
    static inline void ps2_write##bits(uint32_t a, type v)                  \
    {                                                                       \
        uint8_t *p = ps2_pagemap[a >> PS2_PAGE_BITS];                       \
        if (PS2_LIKELY(p)) {                                                \
            memcpy(p + (a & 0xFFFu), &v, sizeof v);                         \
            return;                                                         \
        }                                                                   \
        ps2_io_write##bits(a, v);                                           \
    }

PS2_MEM_RW(8, uint8_t)
PS2_MEM_RW(16, uint16_t)
PS2_MEM_RW(32, uint32_t)
PS2_MEM_RW(64, uint64_t)

static inline void ps2_read128(uint32_t a, u128 *v)
{
    uint8_t *p = ps2_pagemap[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p)) {
        memcpy(v, p + (a & 0xFFFu), 16);
        return;
    }
    ps2_io_read128(a, v);
}

static inline void ps2_write128(uint32_t a, const u128 *v)
{
    uint8_t *p = ps2_pagemap[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p)) {
        memcpy(p + (a & 0xFFFu), v, 16);
        return;
    }
    ps2_io_write128(a, v);
}

/* Copy helpers used by the runtime (HLE code) on guest buffers. */
void ps2_mem_read(uint32_t a, void *dst, uint32_t n);
void ps2_mem_write(uint32_t a, const void *src, uint32_t n);
uint32_t ps2_mem_strlen(uint32_t a, uint32_t max);
void ps2_mem_readstr(uint32_t a, char *dst, uint32_t max);

/* ------------------------------------------------------ control flow */
void ps2_call(R5900 *c, uint32_t addr);
void ps2_jump(R5900 *c, uint32_t addr);
void ps2_syscall(R5900 *c, uint32_t code);
void ps2_break(R5900 *c, uint32_t pc, uint32_t code);
void ps2_trap(R5900 *c, uint32_t pc);
void ps2_eret(R5900 *c);
void ps2_bad_entry(R5900 *c, uint32_t addr);
void ps2_unimplemented(R5900 *c, uint32_t pc, uint32_t word);
ps2_fn ps2_lookup(uint32_t addr, int *secondary);

/* Events (interrupts, timers, vblank) are delivered at safe points: backward
 * branches, syscalls and hardware register accesses. */
extern PS2_IMPORT volatile int ps2_event_flag;
void ps2_service(R5900 *c);
#define PS2_POLL(c)                                     \
    do {                                                \
        if (PS2_UNLIKELY(ps2_event_flag))               \
            ps2_service(c);                             \
    } while (0)
/* Same, recording the branch address (shown by --trace state reports). */
#define PS2_POLL_AT(c, addr)                            \
    do {                                                \
        if (PS2_UNLIKELY(ps2_event_flag)) {             \
            (c)->pc = (addr);                           \
            ps2_service(c);                             \
        }                                               \
    } while (0)

/* ------------------------------------------------------------ COP0 */
extern PS2_IMPORT uint32_t ps2_cop0[32];
uint32_t ps2_mfc0(R5900 *c, int reg, int sel);
void ps2_mtc0(R5900 *c, int reg, int sel, uint32_t v);
void ps2_ei(R5900 *c);
void ps2_di(R5900 *c);
int ps2_cpcond0(R5900 *c);

/* ------------------------------------------------------ integer ops */
static inline void ps2_mult(R5900 *c, int rd, int rs, int rt, int unit)
{
    int64_t p = (int64_t)c->r[rs].SW[0] * (int64_t)c->r[rt].SW[0];
    c->lo.SD[unit] = (int32_t)(uint32_t)p;
    c->hi.SD[unit] = (int32_t)(uint32_t)((uint64_t)p >> 32);
    if (rd)
        c->r[rd].SD[0] = c->lo.SD[unit];
}

static inline void ps2_multu(R5900 *c, int rd, int rs, int rt, int unit)
{
    uint64_t p = (uint64_t)c->r[rs].UW[0] * (uint64_t)c->r[rt].UW[0];
    c->lo.SD[unit] = (int32_t)(uint32_t)p;
    c->hi.SD[unit] = (int32_t)(uint32_t)(p >> 32);
    if (rd)
        c->r[rd].SD[0] = c->lo.SD[unit];
}

static inline void ps2_madd(R5900 *c, int rd, int rs, int rt, int unit)
{
    int64_t acc = (int64_t)(((uint64_t)c->hi.UW[unit * 2] << 32) | c->lo.UW[unit * 2]);
    acc += (int64_t)c->r[rs].SW[0] * (int64_t)c->r[rt].SW[0];
    c->lo.SD[unit] = (int32_t)(uint32_t)acc;
    c->hi.SD[unit] = (int32_t)(uint32_t)((uint64_t)acc >> 32);
    if (rd)
        c->r[rd].SD[0] = c->lo.SD[unit];
}

static inline void ps2_maddu(R5900 *c, int rd, int rs, int rt, int unit)
{
    uint64_t acc = ((uint64_t)c->hi.UW[unit * 2] << 32) | c->lo.UW[unit * 2];
    acc += (uint64_t)c->r[rs].UW[0] * (uint64_t)c->r[rt].UW[0];
    c->lo.SD[unit] = (int32_t)(uint32_t)acc;
    c->hi.SD[unit] = (int32_t)(uint32_t)(acc >> 32);
    if (rd)
        c->r[rd].SD[0] = c->lo.SD[unit];
}

static inline void ps2_div(R5900 *c, int rd, int rs, int rt, int unit)
{
    int32_t a = c->r[rs].SW[0], b = c->r[rt].SW[0];
    (void)rd;
    if (b == 0) {
        c->lo.SD[unit] = a < 0 ? 1 : -1;
        c->hi.SD[unit] = a;
    } else if (a == INT32_MIN && b == -1) {
        c->lo.SD[unit] = INT32_MIN;
        c->hi.SD[unit] = 0;
    } else {
        c->lo.SD[unit] = a / b;
        c->hi.SD[unit] = a % b;
    }
}

static inline void ps2_divu(R5900 *c, int rd, int rs, int rt, int unit)
{
    uint32_t a = c->r[rs].UW[0], b = c->r[rt].UW[0];
    (void)rd;
    if (b == 0) {
        c->lo.SD[unit] = -1;
        c->hi.SD[unit] = (int32_t)a;
    } else {
        c->lo.SD[unit] = (int32_t)(a / b);
        c->hi.SD[unit] = (int32_t)(a % b);
    }
}

/* Unaligned load/store (little-endian semantics). */
static inline void ps2_lwl(R5900 *c, int rt, uint32_t addr)
{
    static const uint32_t mask[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
    static const uint8_t shift[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3, mem = ps2_read32(addr & ~3u);
    c->r[rt].SD[0] = (int32_t)((c->r[rt].UW[0] & mask[s]) | (mem << shift[s]));
}

static inline void ps2_lwr(R5900 *c, int rt, uint32_t addr)
{
    static const uint32_t mask[4] = {0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
    static const uint8_t shift[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3, mem = ps2_read32(addr & ~3u);
    mem = (c->r[rt].UW[0] & mask[s]) | (mem >> shift[s]);
    if (s == 0)
        c->r[rt].SD[0] = (int32_t)mem;
    else
        c->r[rt].UW[0] = mem;
}

static inline void ps2_ldl(R5900 *c, int rt, uint32_t addr)
{
    uint32_t s = addr & 7;
    uint64_t mem = ps2_read64(addr & ~7u);
    uint64_t mask = s == 7 ? 0 : (0x00FFFFFFFFFFFFFFull >> (s * 8));
    c->r[rt].UD[0] = (c->r[rt].UD[0] & mask) | (mem << (56 - s * 8));
}

static inline void ps2_ldr(R5900 *c, int rt, uint32_t addr)
{
    uint32_t s = addr & 7;
    uint64_t mem = ps2_read64(addr & ~7u);
    uint64_t mask = s == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull << (64 - s * 8));
    c->r[rt].UD[0] = (c->r[rt].UD[0] & mask) | (mem >> (s * 8));
}

static inline void ps2_swl(R5900 *c, int rt, uint32_t addr)
{
    static const uint32_t mask[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};
    static const uint8_t shift[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3, mem = ps2_read32(addr & ~3u);
    ps2_write32(addr & ~3u, (c->r[rt].UW[0] >> shift[s]) | (mem & mask[s]));
}

static inline void ps2_swr(R5900 *c, int rt, uint32_t addr)
{
    static const uint32_t mask[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
    static const uint8_t shift[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3, mem = ps2_read32(addr & ~3u);
    ps2_write32(addr & ~3u, (c->r[rt].UW[0] << shift[s]) | (mem & mask[s]));
}

static inline void ps2_sdl(R5900 *c, int rt, uint32_t addr)
{
    uint32_t s = addr & 7;
    uint64_t mem = ps2_read64(addr & ~7u);
    uint64_t mask = s == 7 ? 0 : (0xFFFFFFFFFFFFFF00ull << (s * 8));
    ps2_write64(addr & ~7u, (c->r[rt].UD[0] >> (56 - s * 8)) | (mem & mask));
}

static inline void ps2_sdr(R5900 *c, int rt, uint32_t addr)
{
    uint32_t s = addr & 7;
    uint64_t mem = ps2_read64(addr & ~7u);
    uint64_t mask = s == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull >> (64 - s * 8));
    ps2_write64(addr & ~7u, (c->r[rt].UD[0] << (s * 8)) | (mem & mask));
}

/* ------------------------------------------------------------- FPU */
#define FPU_C (1u << 23)
#define FPU_FLAG_I (1u << 17)     /* invalid (sticky SI at bit 6) */
#define FPU_FLAG_D (1u << 16)     /* divide by zero (sticky SD at bit 5) */

static inline float fpu_clamp(float x)
{
    fpr v;
    v.f = x;
    if (PS2_UNLIKELY((v.u & 0x7F800000u) == 0x7F800000u))
        v.u = (v.u & 0x80000000u) | 0x7F7FFFFFu;
    return v.f;
}

static inline float fpu_in(float x)
{
    /* PS2 has no Inf/NaN: exponent 255 is just a large number. */
    return fpu_clamp(x);
}

static inline float fpu_add(float a, float b) { return fpu_clamp(fpu_in(a) + fpu_in(b)); }
static inline float fpu_sub(float a, float b) { return fpu_clamp(fpu_in(a) - fpu_in(b)); }
static inline float fpu_mul(float a, float b) { return fpu_clamp(fpu_in(a) * fpu_in(b)); }

static inline float fpu_signed_max(uint32_t sign)
{
    uint32_t u = (sign & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline float fpu_div(R5900 *c, float a, float b)
{
    if (PS2_UNLIKELY(b == 0.0f)) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        c->fcr31 |= (a == 0.0f) ? (FPU_FLAG_I | 0x40) : (FPU_FLAG_D | 0x20);
        return fpu_signed_max(ua ^ ub);
    }
    return fpu_clamp(fpu_in(a) / fpu_in(b));
}

static inline float fpu_sqrt(R5900 *c, float x)
{
    if (x < 0.0f) {
        c->fcr31 |= FPU_FLAG_I | 0x40;
        x = -x;
    }
    return sqrtf(fpu_in(x));
}

static inline float fpu_rsqrt(R5900 *c, float a, float b)
{
    if (b < 0.0f) {
        c->fcr31 |= FPU_FLAG_I | 0x40;
        b = -b;
    }
    if (PS2_UNLIKELY(b == 0.0f)) {
        uint32_t ua;
        memcpy(&ua, &a, 4);
        c->fcr31 |= FPU_FLAG_D | 0x20;
        return fpu_signed_max(ua);
    }
    return fpu_clamp(fpu_in(a) / sqrtf(fpu_in(b)));
}

static inline int32_t fpu_cvtw(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    if (((u >> 23) & 0xFF) >= 158)
        return (u & 0x80000000u) ? INT32_MIN : INT32_MAX;
    return (int32_t)f;
}

static inline float fpu_max(float a, float b) { return a > b ? a : b; }
static inline float fpu_min(float a, float b) { return a < b ? a : b; }

static inline void fpu_setc(R5900 *c, int cond)
{
    if (cond)
        c->fcr31 |= FPU_C;
    else
        c->fcr31 &= ~FPU_C;
}

static inline uint32_t ps2_cfc1(R5900 *c, int reg)
{
    if (reg == 0)
        return 0x2E30;
    if (reg == 31)
        return c->fcr31;
    return 0;
}

static inline void ps2_ctc1(R5900 *c, int reg, uint32_t v)
{
    if (reg == 31)
        c->fcr31 = v;
}

/* -------------------------------------------------------- logging */
void ps2_log(const char *fmt, ...) PS2_PRINTF(1, 2);
void ps2_warn_once(const char *key, const char *fmt, ...) PS2_PRINTF(2, 3);
PS2_NORETURN_PRE void ps2_fatal(const char *fmt, ...) PS2_PRINTF(1, 2) PS2_NORETURN_POST;
extern PS2_IMPORT int ps2_verbose;

#include "ps2_mmi.h"
#include "ps2_vu0.h"

#ifdef __cplusplus
}
#endif

#endif /* PS2RT_H */
