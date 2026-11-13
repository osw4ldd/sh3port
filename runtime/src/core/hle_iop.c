/*
 * Function-level HLE of SDK IOP client libraries (native IOP, no BIOS).
 *
 * The recompiler puts `if (ps2_hle(c, &id, "name")) return;` at the start of
 * every function listed in the game config's "hle" map (addresses found with
 * an SDK signature scan, see NATIVE_PLAN.md).  In real-IOP mode (BIOS, LLE)
 * ps2_hle() returns 0 and the original code runs; with the HLE IOP the call
 * is answered here.
 *
 * Semantics follow the public SDK API as documented in ps2sdk
 * common/include/libcdvd-common.h (sceCd*) and the fileio API (sceOpen, ...,
 * same flags as ps2sdk's fileio / iomanX).
 */
#include "../internal.h"
#include "iso.h"
#include <time.h>

/* sif.c: ps2sdk-layout fileio call on host files (host:, mc:, cdrom0: dir) */
int32_t sif_fio(uint32_t fno, uint8_t *arg);

#define ARG(n) (c->r[4 + (n)].UW[0])
static void ret(R5900 *c, int32_t v) { c->r[2].SD[0] = v; }

/* ------------------------------------------------------------ disc files */
#define ISO_FD_BASE 0x100
#define MAX_ISO_FDS 64
static struct { int used; uint32_t lsn, size, pos; const char *mem; } iso_fds[MAX_ISO_FDS];

/* rom0: files the games read without a BIOS.  ROMVER format "VVVVRTYYYYMMDD\n"
 * (version, region A = America, type C = console, build date). */
static const struct { const char *name; const char *data; } rom_files[] = {
    {"rom0:ROMVER", "0200AC20040614\n"},
};

static int is_cdrom(const char *name)
{
    return !_strnicmp(name, "cdrom", 5) || !_strnicmp(name, "cdfs", 4);
}

static void read_name(uint32_t a, char *out, uint32_t n) { ps2_mem_readstr(a, out, n); }

static int32_t mem_fd_open(const char *data)
{
    for (int i = 0; i < MAX_ISO_FDS; i++)
        if (!iso_fds[i].used) {
            iso_fds[i].used = 1;
            iso_fds[i].mem = data;
            iso_fds[i].size = (uint32_t)strlen(data);
            iso_fds[i].pos = 0;
            return ISO_FD_BASE + i;
        }
    return -24;
}

static int32_t iso_fd_open(const char *name)
{
    iso_entry e;
    if (!iso_find(name, &e) || e.is_dir)
        return -2; /* ENOENT */
    for (int i = 0; i < MAX_ISO_FDS; i++)
        if (!iso_fds[i].used) {
            iso_fds[i].used = 1;
            iso_fds[i].mem = NULL;
            iso_fds[i].lsn = e.lsn;
            iso_fds[i].size = e.size;
            iso_fds[i].pos = 0;
            return ISO_FD_BASE + i;
        }
    return -24; /* EMFILE */
}

static int iso_fd(int32_t fd)
{
    int i = fd - ISO_FD_BASE;
    return (i >= 0 && i < MAX_ISO_FDS && iso_fds[i].used) ? i : -1;
}

static void hle_sceOpen(R5900 *c)
{
    char name[256];
    read_name(ARG(0), name, sizeof name);
    int32_t r = -2;
    int rom = 0;
    for (size_t k = 0; k < sizeof rom_files / sizeof rom_files[0]; k++)
        if (!_stricmp(name, rom_files[k].name)) {
            r = mem_fd_open(rom_files[k].data);
            rom = 1;
        }
    if (rom) {
    } else if (is_cdrom(name) && iso_is_open()) {
        r = iso_fd_open(name);
    } else {
        uint8_t arg[4 + 256] = {0};
        wr32le(arg, ARG(1));
        memcpy(arg + 4, name, strlen(name));
        r = sif_fio(0, arg);
    }
    if (ps2_cfg.trace_syscalls)
        ps2_log("hle sceOpen(%s, %X) = %d", name, ARG(1), r);
    ret(c, r);
}

static void hle_sceClose(R5900 *c)
{
    int32_t fd = (int32_t)ARG(0);
    int i = iso_fd(fd);
    if (i >= 0) {
        iso_fds[i].used = 0;
        ret(c, 0);
        return;
    }
    uint8_t arg[4];
    wr32le(arg, (uint32_t)fd);
    ret(c, sif_fio(1, arg));
}

static void hle_sceRead(R5900 *c)
{
    int32_t fd = (int32_t)ARG(0);
    uint32_t buf = ARG(1), n = ARG(2);
    int i = iso_fd(fd);
    if (i >= 0) {
        uint32_t left = iso_fds[i].size - iso_fds[i].pos;
        if (n > left)
            n = left;
        if (iso_fds[i].mem) {
            ps2_mem_write(buf, iso_fds[i].mem + iso_fds[i].pos, n);
            iso_fds[i].pos += n;
            ret(c, (int32_t)n);
            return;
        }
        uint8_t *tmp = malloc(n ? n : 1);
        int ok = iso_read_bytes((uint64_t)iso_fds[i].lsn * 2048 + iso_fds[i].pos, n, tmp);
        if (ok) {
            ps2_mem_write(buf, tmp, n);
            iso_fds[i].pos += n;
        }
        free(tmp);
        ret(c, ok ? (int32_t)n : -5);
        return;
    }
    uint8_t arg[16];
    wr32le(arg, (uint32_t)fd);
    wr32le(arg + 4, buf);
    wr32le(arg + 8, n);
    wr32le(arg + 12, 0);
    ret(c, sif_fio(2, arg));
}

static void hle_sceWrite(R5900 *c)
{
    uint8_t arg[32] = {0};
    wr32le(arg, ARG(0));
    wr32le(arg + 4, ARG(1));
    wr32le(arg + 8, ARG(2));
    ret(c, iso_fd((int32_t)ARG(0)) >= 0 ? -9 : sif_fio(3, arg));
}

static void hle_sceLseek(R5900 *c)
{
    int32_t fd = (int32_t)ARG(0), off = (int32_t)ARG(1);
    int whence = (int)ARG(2);
    int i = iso_fd(fd);
    if (i >= 0) {
        int64_t p = whence == 0 ? off : whence == 1 ? (int64_t)iso_fds[i].pos + off
                                                    : (int64_t)iso_fds[i].size + off;
        if (p < 0 || p > iso_fds[i].size) {
            ret(c, -22); /* EINVAL */
            return;
        }
        iso_fds[i].pos = (uint32_t)p;
        ret(c, (int32_t)p);
        return;
    }
    uint8_t arg[12];
    wr32le(arg, (uint32_t)fd);
    wr32le(arg + 4, (uint32_t)off);
    wr32le(arg + 8, (uint32_t)whence);
    ret(c, sif_fio(4, arg));
}

static void hle_sceIoctl(R5900 *c) { ret(c, 0); }

/* Observed with the real IOP: every devctl on "cdrom0:" returns 0 (SH3). */
static void hle_sceDevctl(R5900 *c)
{
    if (ps2_cfg.trace_syscalls) {
        char name[64];
        read_name(ARG(0), name, sizeof name);
        ps2_log("hle sceDevctl(%s, %X)", name, ARG(1));
    }
    ret(c, 0);
}

static void hle_sceGetstat(R5900 *c)
{
    char name[256];
    read_name(ARG(0), name, sizeof name);
    if (is_cdrom(name) && iso_is_open()) {
        iso_entry e;
        if (!iso_find(name, &e)) {
            ret(c, -2);
            return;
        }
        uint8_t st[40] = {0};   /* same layout as sif.c write_stat (io_stat_t) */
        wr32le(st + 0, e.is_dir ? (0x1000u | 0x16D) : (0x2000u | 0x124));
        wr32le(st + 8, e.size);
        ps2_mem_write(ARG(1), st, sizeof st);
        ret(c, 0);
        return;
    }
    uint8_t arg[4 + 256] = {0};
    wr32le(arg, ARG(1));
    memcpy(arg + 4, name, strlen(name));
    ret(c, sif_fio(12, arg));
}

/* ------------------------------------------------------------ libcdvd */
static int32_t cd_error;

static void hle_sceCdInit(R5900 *c) { ret(c, 1); }
static void hle_sceCdMmode(R5900 *c) { ret(c, 1); }
static void hle_sceCdSync(R5900 *c) { ret(c, 0); }          /* 0 = completed */
static void hle_sceCdGetError(R5900 *c) { ret(c, cd_error); }
static void hle_sceCdBreak(R5900 *c) { ret(c, 1); }
static void hle_sceCdDiskReady(R5900 *c) { ret(c, 2); }     /* SCECdComplete */

static void hle_sceCdRead(R5900 *c)
{
    uint32_t lbn = ARG(0), n = ARG(1), buf = ARG(2);
    uint8_t *tmp = malloc((size_t)n * 2048 + 1);
    int ok = iso_read(lbn, n, tmp);
    if (ok)
        ps2_mem_write(buf, tmp, n * 2048);
    free(tmp);
    cd_error = ok ? 0 : 0x32; /* SCECdErREAD */
    if (ps2_cfg.trace_syscalls)
        ps2_log("hle sceCdRead(%u, %u, %08X) = %d", lbn, n, buf, ok);
    ret(c, ok);
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void hle_sceCdReadClock(R5900 *c)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_s(&tm, &t);
    uint8_t clk[8] = {0, bcd(tm.tm_sec), bcd(tm.tm_min), bcd(tm.tm_hour), 0,
                      bcd(tm.tm_mday), bcd(tm.tm_mon + 1), bcd(tm.tm_year % 100)};
    ps2_mem_write(ARG(0), clk, sizeof clk);
    ret(c, 1);
}

static void hle_sceCdLayerSearchFile(R5900 *c)
{
    char name[256];
    read_name(ARG(1), name, sizeof name);
    iso_entry e;
    int ok = iso_find(name, &e) && !e.is_dir;
    if (ok) {
        uint8_t f[32] = {0};   /* sceCdlFILE: lsn, size, name[16], date[8] */
        wr32le(f + 0, e.lsn);
        wr32le(f + 4, e.size);
        memcpy(f + 8, e.name, strlen(e.name) < 15 ? strlen(e.name) : 15);
        f[25] = e.date[5];          /* sec */
        f[26] = e.date[4];          /* min */
        f[27] = e.date[3];          /* hour */
        f[28] = e.date[2];          /* day */
        f[29] = e.date[1];          /* month */
        uint16_t y = (uint16_t)(1900 + e.date[0]);
        memcpy(f + 30, &y, 2);
        ps2_mem_write(ARG(0), f, sizeof f);
    }
    if (ps2_cfg.trace_syscalls)
        ps2_log("hle sceCdLayerSearchFile(%s) = %d (lsn %u, %u bytes)", name, ok, ok ? e.lsn : 0,
                ok ? e.size : 0);
    ret(c, ok);
}

/* ------------------------------------------------------------ helpers */
int ps2_hle_original(R5900 *c, const char *name)
{
    for (const ps2_hle_orig *o = ps2_hle_origs; o->name; o++)
        if (!strcmp(o->name, name)) {
            o->fn(c);
            return 1;
        }
    return 0;
}

uint32_t ps2_hle_call_guest(R5900 *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    /* The HLE runs at the entry of a guest function, so caller-saved
     * registers are ours to use; keep them anyway (and ra/sp, which the
     * callee restores) so the HLE code can rely on its arguments. */
    u128 save[32];
    memcpy(save, c->r, sizeof save);
    c->r[4].SD[0] = (int32_t)a0;
    c->r[5].SD[0] = (int32_t)a1;
    c->r[6].SD[0] = (int32_t)a2;
    c->r[7].SD[0] = (int32_t)a3;
    ps2_call(c, addr);
    uint32_t v0 = c->r[2].UW[0];
    memcpy(c->r, save, sizeof save);
    return v0;
}

/* ------------------------------------------------------------ dispatch */
typedef void (*hle_fn)(R5900 *c);
static const struct { const char *name; hle_fn fn; } hle_table[] = {
    {"sceOpen", hle_sceOpen},
    {"sceClose", hle_sceClose},
    {"sceRead", hle_sceRead},
    {"sceWrite", hle_sceWrite},
    {"sceLseek", hle_sceLseek},
    {"sceIoctl", hle_sceIoctl},
    {"sceDevctl", hle_sceDevctl},
    {"sceGetstat", hle_sceGetstat},
    {"sceCdInit", hle_sceCdInit},
    {"sceCdMmode", hle_sceCdMmode},
    {"sceCdRead", hle_sceCdRead},
    {"sceCdSync", hle_sceCdSync},
    {"sceCdSyncS", hle_sceCdSync},
    {"sceCdGetError", hle_sceCdGetError},
    {"sceCdBreak", hle_sceCdBreak},
    {"sceCdNcmdDiskReady", hle_sceCdDiskReady},
    {"sceCdDiskReady", hle_sceCdDiskReady},
    {"sceCdReadClock", hle_sceCdReadClock},
    {"sceCdLayerSearchFile", hle_sceCdLayerSearchFile},
};

/* Replacements that decide themselves whether to run (any IOP mode); they
 * return 0 to run the original code. */
int hle_sceMpegGetPictureRAW8(R5900 *c);   /* hle_mpeg.c */
int hle_sceMpegAddCallback(R5900 *c);
int hle_sceMpegIsEnd(R5900 *c);
int hle_sceMpegReset(R5900 *c);
static const struct { const char *name; int (*fn)(R5900 *c); } hle_any[] = {
    {"sceMpegGetPictureRAW8", hle_sceMpegGetPictureRAW8},
    {"sceMpegAddCallback", hle_sceMpegAddCallback},
    {"sceMpegIsEnd", hle_sceMpegIsEnd},
    {"sceMpegReset", hle_sceMpegReset},
};
#define HLE_ANY_BASE 1000

int ps2_hle(R5900 *c, int *id, const char *name)
{
    if (!strncmp(name, "obs_", 4)) {
        /* Observer entries (debugging): log the call, run the original. */
        if (ps2_verbose)
            ps2_log("obs %s(%08X, %08X, %08X, %08X) ra %08X thread %d", name + 4, c->r[4].UW[0], c->r[5].UW[0],
                    c->r[6].UW[0], c->r[7].UW[0], c->r[31].UW[0], kernel_thread_id());
        return 0;
    }
    if (*id == -1) {
        *id = -2;
        for (int i = 0; i < (int)(sizeof hle_table / sizeof hle_table[0]); i++)
            if (!strcmp(hle_table[i].name, name))
                *id = i;
        for (int i = 0; i < (int)(sizeof hle_any / sizeof hle_any[0]); i++)
            if (!strcmp(hle_any[i].name, name))
                *id = HLE_ANY_BASE + i;
        if (*id == -2)
            ps2_log("hle: no native implementation of %s, running the original", name);
    }
    if (*id >= HLE_ANY_BASE)
        return hle_any[*id - HLE_ANY_BASE].fn(c);
    static int mode = -1;
    if (mode < 0) {
        mode = !be_iop_lle();
        if (mode && ps2_cfg.iso_path[0] && !iso_is_open())
            iso_open(ps2_cfg.iso_path);
    }
    if (*id < 0 || !mode)
        return 0;
    hle_table[*id].fn(c);
    return 1;
}
