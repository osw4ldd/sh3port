/* Indirect control flow, COP0, VU0 control registers and fault reporting. */
#include "../internal.h"

static uint32_t cop0_status_default(void)
{
    /* EIE | IE | IM(INT0, INT1, timer) | user mode. */
    return 0x00010000u | 0x00000001u | 0x00008C00u | 0x00000010u;
}

uint32_t ps2_cop0[32];

static ps2_fn find_fn(uint32_t addr, int *secondary)
{
    uint32_t lo = 0, hi = ps2_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t a = ps2_func_table[mid].addr;
        if (a == addr) {
            if (secondary)
                *secondary = (int)ps2_func_table[mid].secondary;
            return ps2_func_table[mid].fn;
        }
        if (a < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

ps2_fn ps2_lookup(uint32_t addr, int *secondary)
{
    ps2_fn fn = find_fn(addr, secondary);
    if (!fn) {
        /* Uncached (0x2/0x3) and kernel (0x8/0xA) segment aliases of RAM. */
        uint32_t seg = addr >> 28;
        if (seg == 0x2 || seg == 0x3 || seg == 0x8 || seg == 0xA)
            fn = find_fn(addr & 0x0FFFFFFFu, secondary);
    }
    return fn;
}

static void dump_context(R5900 *c)
{
    static const char *names[32] = {
        "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  %s=%08X %s=%08X %s=%08X %s=%08X\n", names[i], c->r[i].UW[0],
                names[i + 1], c->r[i + 1].UW[0], names[i + 2], c->r[i + 2].UW[0],
                names[i + 3], c->r[i + 3].UW[0]);
}

static void unknown_target(R5900 *c, uint32_t addr, const char *how)
{
    /* Remember it: the recompiler adds these entries on the next build. */
    char path[1100];
    snprintf(path, sizeof path, "%s/ps2native_missing.txt", ps2_cfg.host_root);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "0x%08X\n", addr);
        fclose(f);
    }
    fprintf(stderr, "[ps2] fatal: %s to %08X, which was not recompiled.\n", how, addr);
    fprintf(stderr, "[ps2] It was recorded in ps2native_missing.txt; rebuilding the program "
                    "picks it up automatically (or pass --entry 0x%08X).\n", addr);
    dump_context(c);
    ps2_fatal("unrecoverable control flow");
}

/* Debugging: PS2NATIVE_WATCH=hex,hex logs indirect calls to those addresses. */
static void watch_call(R5900 *c, uint32_t addr)
{
    static int init;
    static uint32_t watch[16];
    static int nwatch;
    if (!init) {
        init = 1;
        const char *env = getenv("PS2NATIVE_WATCH");
        while (env && *env && nwatch < 16) {
            char *end;
            unsigned long v = strtoul(env, &end, 16);
            if (end == env)
                break;
            watch[nwatch++] = (uint32_t)v;
            env = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < nwatch; i++)
        if (watch[i] == addr)
            ps2_log("watch: call %08X a0=%08X a1=%08X ra=%08X", addr, c->r[4].UW[0], c->r[5].UW[0],
                    c->r[31].UW[0]);
}

void ps2_call(R5900 *c, uint32_t addr)
{
    int sec = 0;
    watch_call(c, addr);
    ps2_fn fn = ps2_lookup(addr, &sec);
    if (PS2_UNLIKELY(!fn))
        unknown_target(c, addr, "indirect call");
    if (sec)
        c->jump_pc = addr;
    fn(c);
}

void ps2_jump(R5900 *c, uint32_t addr)
{
    int sec = 0;
    ps2_fn fn = ps2_lookup(addr, &sec);
    if (PS2_UNLIKELY(!fn))
        unknown_target(c, addr, "jump");
    if (sec)
        c->jump_pc = addr;
    fn(c);
}

void ps2_bad_entry(R5900 *c, uint32_t addr)
{
    unknown_target(c, addr, "secondary entry");
}

void ps2_syscall(R5900 *c, uint32_t code)
{
    (void)code;
    kernel_syscall(c, c->r[3].SW[0]);
}

void ps2_break(R5900 *c, uint32_t pc, uint32_t code)
{
    ps2_log("BREAK %u at %08X", code, pc);
    dump_context(c);
    if (code == 7)
        ps2_fatal("guest division by zero trap (break 7) at %08X", pc);
    ps2_fatal("guest break instruction at %08X", pc);
}

void ps2_trap(R5900 *c, uint32_t pc)
{
    dump_context(c);
    ps2_fatal("guest trap at %08X", pc);
}

void ps2_eret(R5900 *c)
{
    (void)c;
    ps2_warn_once("eret", "ERET executed by guest code (kernel-mode code is not supported)");
}

void ps2_unimplemented(R5900 *c, uint32_t pc, uint32_t word)
{
    dump_context(c);
    ps2_fatal("unimplemented/invalid instruction %08X at %08X", word, pc);
}

/* -------------------------------------------------------------- COP0 */
void cop0_init(void)
{
    memset(ps2_cop0, 0, sizeof ps2_cop0);
    ps2_cop0[12] = cop0_status_default();
    ps2_cop0[15] = 0x2E20;      /* PRId: R5900 */
    ps2_cop0[16] = 0x00000440;  /* Config */
}

uint32_t ps2_mfc0(R5900 *c, int reg, int sel)
{
    (void)c;
    switch (reg) {
    case 9:
        return (uint32_t)ps2_ee_cycles();
    case 25: /* performance counters */
        if ((sel & 1) == 0)
            return ps2_cop0[25];
        return (uint32_t)(ps2_ee_cycles() >> ((sel & 2) ? 1 : 0));
    default:
        return ps2_cop0[reg & 31];
    }
}

void ps2_mtc0(R5900 *c, int reg, int sel, uint32_t v)
{
    (void)sel;
    if (reg == 9 || reg == 15)
        return;
    ps2_cop0[reg & 31] = v;
    if (reg == 12)
        PS2_POLL(c);
}

void ps2_ei(R5900 *c)
{
    ps2_cop0[12] |= 0x10000u;
    ps2_event_flag = 1;
    PS2_POLL(c);
}

void ps2_di(R5900 *c)
{
    (void)c;
    ps2_cop0[12] &= ~0x10000u;
}

int kernel_interrupts_enabled(void)
{
    uint32_t s = ps2_cop0[12];
    return (s & 0x10001u) == 0x10001u && !(s & 6u) && !kernel_in_handler;
}

/* ------------------------------------------------------------- VU0 */
uint32_t vu0_cfc2(R5900 *c, int reg)
{
    if (reg < 16)
        return c->vi[reg];
    switch (reg) {
    case 16: return c->vu_status;
    case 17: return c->vu_mac;
    case 18: return c->vu_clip;
    case 20: return c->vu_r & 0x7FFFFF;
    case 21: { uint32_t u; memcpy(&u, &c->vi_reg, 4); return u; }
    case 22: { uint32_t u; memcpy(&u, &c->vq, 4); return u; }
    case 27: return c->vu_cmsar0;
    case 29: return 0;   /* VPU-STAT: VU0/VU1 idle */
    default: return 0;
    }
}

void vu0_ctc2(R5900 *c, int reg, uint32_t v)
{
    if (reg < 16) {
        if (reg)
            c->vi[reg] = (uint16_t)v;
        return;
    }
    switch (reg) {
    case 16: c->vu_status = v & 0xFC0; break;
    case 18: c->vu_clip = v & 0xFFFFFF; break;
    case 20: c->vu_r = 0x3F800000u | (v & 0x7FFFFF); break;
    case 21: memcpy(&c->vi_reg, &v, 4); break;
    case 22: memcpy(&c->vq, &v, 4); break;
    case 27: c->vu_cmsar0 = v & 0xFFFF; break;
    case 28: break;     /* FBRST: reset/force-break requests */
    case 31:
        ps2_warn_once("vu1start", "CTC2 CMSAR1: VU1 micro programs are not emulated");
        break;
    default: break;
    }
}

void vu0_callms(R5900 *c, uint32_t addr)
{
    vu0_state st;
    memcpy(st.vf, c->vf, sizeof st.vf);
    memcpy(st.acc, &c->vacc, sizeof st.acc);
    for (int i = 0; i < 16; i++)
        st.vi[i] = c->vi[i];
    memcpy(&st.q, &c->vq, 4);
    memcpy(&st.i, &c->vi_reg, 4);
    memcpy(&st.p, &c->vp, 4);
    st.r = c->vu_r;
    st.status = c->vu_status;
    st.mac = c->vu_mac;
    st.clip = c->vu_clip;
    if (!be_vu0_exec(&st, addr)) {
        char key[32];
        snprintf(key, sizeof key, "vcallms%X", addr);
        ps2_warn_once(key, "VCALLMS %04X: VU0 micro mode needs the PCSX2 backend", addr);
        return;
    }
    memcpy(&c->vf[1], st.vf[1], sizeof st.vf - sizeof st.vf[0]);   /* vf0 is constant */
    memcpy(&c->vacc, st.acc, sizeof st.acc);
    for (int i = 1; i < 16; i++)
        c->vi[i] = (uint16_t)st.vi[i];
    memcpy(&c->vq, &st.q, 4);
    memcpy(&c->vi_reg, &st.i, 4);
    memcpy(&c->vp, &st.p, 4);
    c->vu_r = st.r;
    c->vu_status = st.status;
    c->vu_mac = st.mac;
    c->vu_clip = st.clip;
}
