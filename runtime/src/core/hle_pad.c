/*
 * Function-level HLE of the pad libraries (native IOP, no BIOS): libpad2,
 * libvib and sceDbcInit (S1-T4).  With these replaced, dbcman (SIDs
 * 0x80000900 / 0x8000091B) receives no RPC at all.
 *
 * Cut level and layouts: build/analysis/s1-pad/README.md (S1-A2), checked
 * against the SH3 disassembly:
 *   0x11F3A0 scePad2CreateSocket(param, buf)   buf 64-aligned, else -1
 *   0x11F708 scePad2GetState(sock)             the game (0x193D70) treats
 *            1 as "stable"; 0/2/3/others reset its phase
 *   0x11F618 scePad2GetButtonProfile(sock, prof) -> profile size; builds the
 *            button order table with 0x11FA68 (ported below)
 *   0x11F540 scePad2Read(sock, dst)            -> data length, -1 if invalid
 *   0x120C78 sceVibGetProfile(sock, prof)       -> actuator profile size
 *   0x120CF8 sceVibSetActParam(sock, profsize, prof, datasize, data)
 *            the game retries while the result is <= 0
 * The game reads buttons through the guest function 0x11F848 (kept as
 * recompiled code), which uses the order table in the libpad2 socket table,
 * so that table is filled exactly as the original code does.
 *
 * Pad data is the 18-byte DualShock 2 report (active-low buttons, sticks,
 * pressures), see be_pad_read() in internal.h.  The host provides it
 * (S1-T2); until it does, a built-in fallback reports a connected pad on
 * port 0 driven by PS2NATIVE_PRESS with the default keyboard mapping.
 */
#include "../internal.h"

#define ARG(n) (c->r[4 + (n)].UW[0])
static void ret(R5900 *c, int32_t v) { c->r[2].SD[0] = v; }

/* libpad2 socket table (SH3 .bss, address from 0x11F540: lui 0x3D / -192):
 * 16 entries of 0x330 bytes {+0 used, +4 linked, +8 dep, +0xC buf,
 * +0x10 order table: 40 x {valid, type 1 (bit) / 8 (byte), byte, bit}}. */
#define PAD2_SOCKETS 0x003CFF40u
#define PAD2_INIT_FLAG 0x0034D0D0u   /* set by 0x11F2F8, cleared by 0x11F338 */
#define PAD2_SOCKET_SIZE 0x330u
#define PAD2_MAX_SOCKETS 16
#define DS2_LEN 18

static const uint8_t ds2_profile[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00}; /* digital + analog + pressure */
static const uint8_t vib_profile[1] = {0x03};                         /* two actuators */

static int sock_port[PAD2_MAX_SOCKETS];
static int last_state[PAD2_MAX_SOCKETS] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                           -1, -1, -1, -1, -1, -1, -1, -1};

static uint32_t sock_entry(uint32_t sock) { return PAD2_SOCKETS + sock * PAD2_SOCKET_SIZE; }

/* Port of 0x11FA68 (order table builder): profile bit i set -> entry i
 * valid; indices 16-31 and 35-38 are one analog byte each, the others one
 * bit each, packed 8 per byte in index order. */
static void pad2_build_order(uint32_t table, const uint8_t *profile)
{
    uint32_t byte = 0, bit = 0;
    for (uint32_t i = 0; i < 40; i++, table += 16) {
        if (!((profile[i >> 3] >> (i & 7)) & 1)) {
            ps2_write32(table + 0, 0);
            ps2_write32(table + 8, 0);
            ps2_write32(table + 12, 0);
            continue;
        }
        ps2_write32(table + 0, 1);
        ps2_write32(table + 8, byte);
        ps2_write32(table + 12, bit);
        if ((i >= 16 && i < 32) || (i >= 35 && i < 39)) {
            ps2_write32(table + 4, 8);
            byte++;
        } else {
            ps2_write32(table + 4, 1);
            if ((++bit & 7) == 0) {
                byte++;
                bit = 0;
            }
        }
    }
}

static int pad_state(uint32_t sock, uint8_t *data)
{
    uint8_t tmp[DS2_LEN];
    if (sock >= PAD2_MAX_SOCKETS)
        return 0;
    memset(tmp, 0, sizeof tmp);
    int on = be_pad_read(sock_port[sock], data ? data : tmp);
    int st = on ? 1 : 0;
    if (st != last_state[sock]) {
        last_state[sock] = st;
        if (ps2_verbose || ps2_cfg.trace_syscalls)
            ps2_log("hle pad: socket %u (port %d) %s", sock, sock_port[sock],
                    st ? "connected" : "no controller");
    }
    return st;
}

/* ------------------------------------------------------------ libdbc */
void hle_sceDbcInit(R5900 *c) { ret(c, 1); }

/* ------------------------------------------------------------ libpad2 */
static void pad2_clear_socket(uint32_t sock)
{
    uint32_t e = sock_entry(sock);
    ps2_write32(e + 0, 0);
    ps2_write32(e + 4, 0);
    ps2_write32(e + 8, 0);
}

void hle_scePad2Init(R5900 *c)
{
    ps2_write32(PAD2_INIT_FLAG, 1);
    for (uint32_t s = 0; s < PAD2_MAX_SOCKETS; s++)   /* as 0x11F2F8: clear all entries */
        pad2_clear_socket(s);
    ret(c, 1);
}

void hle_scePad2End(R5900 *c)
{
    ps2_write32(PAD2_INIT_FLAG, 0);
    for (uint32_t s = 0; s < PAD2_MAX_SOCKETS; s++)
        if (ps2_read32(sock_entry(s)))
            pad2_clear_socket(s);
    ret(c, 1);
}

void hle_scePad2CreateSocket(R5900 *c)
{
    uint32_t param = ARG(0), buf = ARG(1);
    if (buf & 0x3F) {
        ret(c, -1);
        return;
    }
    /* param {w0 flags, w1 port, w2 slot, w3, name[16]}; SH3 passes port 0/1. */
    uint32_t port = param ? ps2_read32(param + 4) : 0;
    uint32_t sock = PAD2_MAX_SOCKETS;
    if (port < PAD2_MAX_SOCKETS && !ps2_read32(sock_entry(port)))
        sock = port;
    for (uint32_t s = 0; sock == PAD2_MAX_SOCKETS && s < PAD2_MAX_SOCKETS; s++)
        if (!ps2_read32(sock_entry(s)))
            sock = s;
    if (sock == PAD2_MAX_SOCKETS) {
        ret(c, -1);
        return;
    }
    sock_port[sock] = (int)(port & 1);
    last_state[sock] = -1;
    uint32_t e = sock_entry(sock);
    ps2_write32(e + 0, 1);           /* used */
    ps2_write32(e + 4, 1);           /* linked (no driver link RPC needed) */
    ps2_write32(e + 8, sock);        /* dep number */
    ps2_write32(e + 12, buf);
    for (uint32_t r = 0; r < 2; r++) {   /* the two records, as 0x11F3A0 */
        uint32_t rec = buf + r * 0x80;
        static const uint8_t ff[32] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        ps2_write32(rec + 0, 0);
        ps2_write32(rec + 4, 0);
        ps2_write32(rec + 0x7C, 0);
        ps2_mem_write(rec + 0x1C, ff, sizeof ff);
    }
    if (ps2_cfg.trace_syscalls)
        ps2_log("hle scePad2CreateSocket(port %u, buf %08X) = %u", port, buf, sock);
    ret(c, (int32_t)sock);
}

void hle_scePad2DeleteSocket(R5900 *c)
{
    uint32_t sock = ARG(0);
    if (sock < PAD2_MAX_SOCKETS)
        pad2_clear_socket(sock);
    ret(c, 1);
}

void hle_scePad2GetState(R5900 *c)
{
    uint32_t sock = ARG(0);
    if (sock >= PAD2_MAX_SOCKETS || !ps2_read32(sock_entry(sock))) {
        ret(c, 0);
        return;
    }
    ret(c, pad_state(sock, NULL));
}

void hle_scePad2GetButtonProfile(R5900 *c)
{
    uint32_t sock = ARG(0), prof = ARG(1);
    if (sock >= PAD2_MAX_SOCKETS || !ps2_read32(sock_entry(sock))) {
        ret(c, -1);
        return;
    }
    ps2_mem_write(prof, ds2_profile, sizeof ds2_profile);
    pad2_build_order(sock_entry(sock) + 0x10, ds2_profile);
    ret(c, (int32_t)sizeof ds2_profile);
}

void hle_scePad2Read(R5900 *c)
{
    uint32_t sock = ARG(0), dst = ARG(1);
    uint8_t data[DS2_LEN];
    if (sock >= PAD2_MAX_SOCKETS || !ps2_read32(sock_entry(sock))) {
        ret(c, -1);
        return;
    }
    memset(data, 0, sizeof data);
    if (!pad_state(sock, data)) {
        ret(c, -1);
        return;
    }
    ps2_mem_write(dst, data, sizeof data);
    ret(c, DS2_LEN);
}

/* ------------------------------------------------------------ libvib */
void hle_sceVibGetProfile(R5900 *c)
{
    uint32_t sock = ARG(0), prof = ARG(1);
    if (sock >= PAD2_MAX_SOCKETS || !pad_state(sock, NULL)) {
        ret(c, 0);
        return;
    }
    ps2_mem_write(prof, vib_profile, sizeof vib_profile);
    ret(c, (int32_t)sizeof vib_profile);
}

/* data (game 0x193B80): bit 0 = small motor on/off, bits 1-8 = large motor. */
void hle_sceVibSetActParam(R5900 *c)
{
    uint32_t sock = ARG(0), datasize = ARG(3), data = c->r[8].UW[0];   /* 5th argument: t0 */
    uint32_t v = 0;
    if (datasize >= 1)
        v = ps2_read8(data);
    if (datasize >= 2)
        v |= (uint32_t)ps2_read8(data + 1) << 8;
    if (sock < PAD2_MAX_SOCKETS) {
        static uint32_t last[PAD2_MAX_SOCKETS];
        int small = (int)(v & 1), large = (int)((v >> 1) & 0xFF);
        be_pad_vibrate(sock_port[sock], small, large);
        if (ps2_cfg.trace_syscalls && v != last[sock])
            ps2_log("hle sceVibSetActParam(socket %u) small %d large %d", sock, small, large);
        last[sock] = v;
    }
    ret(c, 1);
}

/* ------------------------------------------------------------ host fallback */
/* Used when the host does not define be_pad_read / be_pad_vibrate (linker
 * /alternatename below).  Port 0: a connected DualShock 2 driven by
 * PS2NATIVE_PRESS="frame[-last[/step]]:Key,..." (same syntax and 6-frame
 * hold as the host's scripted input, frames = vblanks) with the host's
 * default keyboard mapping (host/Main.cpp SetKeyboardPadBindings; ini
 * key_<Button> overrides are not seen here).  Port 1: no controller. */
enum { PB_DIGITAL, PB_STICK };
static const struct {
    const char *key;
    int kind;
    int index;      /* digital: bit 0-15 of bytes 0-1; stick: report byte */
    int value;      /* stick value */
    int pressure;   /* report byte of the pressure value, or 0 */
} pad_keys[] = {
    {"Backspace", PB_DIGITAL, 0, 0, 0},     /* SELECT */
    {"Z", PB_DIGITAL, 1, 0, 0},             /* L3 */
    {"C", PB_DIGITAL, 2, 0, 0},             /* R3 */
    {"Return", PB_DIGITAL, 3, 0, 0},        /* START */
    {"Up", PB_DIGITAL, 4, 0, 8},
    {"Right", PB_DIGITAL, 5, 0, 6},
    {"Down", PB_DIGITAL, 6, 0, 9},
    {"Left", PB_DIGITAL, 7, 0, 7},
    {"Q", PB_DIGITAL, 8, 0, 16},            /* L2 */
    {"E", PB_DIGITAL, 9, 0, 17},            /* R2 */
    {"1", PB_DIGITAL, 10, 0, 14},           /* L1 */
    {"3", PB_DIGITAL, 11, 0, 15},           /* R1 */
    {"W", PB_DIGITAL, 12, 0, 10},           /* TRIANGLE */
    {"D", PB_DIGITAL, 13, 0, 11},           /* CIRCLE */
    {"S", PB_DIGITAL, 14, 0, 12},           /* CROSS */
    {"A", PB_DIGITAL, 15, 0, 13},           /* SQUARE */
    {"Numpad4", PB_STICK, 2, 0x00, 0},      /* right stick X */
    {"Numpad6", PB_STICK, 2, 0xFF, 0},
    {"Numpad8", PB_STICK, 3, 0x00, 0},      /* right stick Y */
    {"Numpad2", PB_STICK, 3, 0xFF, 0},
    {"J", PB_STICK, 4, 0x00, 0},            /* left stick X */
    {"L", PB_STICK, 4, 0xFF, 0},
    {"I", PB_STICK, 5, 0x00, 0},            /* left stick Y */
    {"K", PB_STICK, 5, 0xFF, 0},
};
#define PAD_KEYS ((int)(sizeof pad_keys / sizeof pad_keys[0]))

#define MAX_PRESS 32
static struct { uint32_t first, last, step; int key; } presses[MAX_PRESS];
static int npresses = -1;

static void parse_presses(void)
{
    const char *p = getenv("PS2NATIVE_PRESS");
    npresses = 0;
    while (p && *p && npresses < MAX_PRESS) {
        char item[64];
        size_t n = strcspn(p, ",");
        if (n >= sizeof item)
            n = sizeof item - 1;
        memcpy(item, p, n);
        item[n] = 0;
        p += strcspn(p, ",");
        if (*p == ',')
            p++;
        char *colon = strchr(item, ':');
        if (!colon)
            continue;
        *colon = 0;
        int key = -1;
        for (int k = 0; k < PAD_KEYS; k++)
            if (!_stricmp(pad_keys[k].key, colon + 1))
                key = k;
        if (key < 0)
            continue;
        char *end;
        uint32_t first = (uint32_t)strtoul(item, &end, 10), last = first, step = 1;
        if (*end == '-')
            last = (uint32_t)strtoul(end + 1, &end, 10);
        if (*end == '/')
            step = (uint32_t)strtoul(end + 1, &end, 10);
        if (!step)
            step = 1;
        presses[npresses].first = first;
        presses[npresses].last = last;
        presses[npresses].step = step;
        presses[npresses].key = key;
        npresses++;
    }
}

static int key_held(int key, uint64_t frame)
{
    for (int i = 0; i < npresses; i++) {
        if (presses[i].key != key)
            continue;
        for (uint64_t d = 0; d < 6 && d <= frame; d++) {   /* held for 6 frames */
            uint64_t f = frame - d;
            if (f >= presses[i].first && f <= presses[i].last && (f - presses[i].first) % presses[i].step == 0)
                return 1;
        }
    }
    return 0;
}

int ps2_pad_default_read(int port, uint8_t *ds2)
{
    static int told;
    if (!told) {
        told = 1;
        ps2_log("hle pad: host has no be_pad_read; port 0 = built-in pad (PS2NATIVE_PRESS, default keys)");
    }
    if (port != 0)
        return 0;
    if (npresses < 0)
        parse_presses();
    uint16_t buttons = 0xFFFF;
    memset(ds2, 0, DS2_LEN);
    ds2[2] = ds2[3] = ds2[4] = ds2[5] = 0x80;
    uint64_t frame = kernel_vblank_count();
    for (int k = 0; k < PAD_KEYS; k++) {
        if (!npresses || !key_held(k, frame))
            continue;
        if (pad_keys[k].kind == PB_DIGITAL) {
            buttons &= (uint16_t)~(1u << pad_keys[k].index);
            if (pad_keys[k].pressure)
                ds2[pad_keys[k].pressure] = 0xFF;
        } else {
            ds2[pad_keys[k].index] = (uint8_t)pad_keys[k].value;
        }
    }
    ds2[0] = (uint8_t)buttons;
    ds2[1] = (uint8_t)(buttons >> 8);
    return 1;
}

void ps2_pad_default_vibrate(int port, int small, int large)
{
    (void)port; (void)small; (void)large;
}

#if defined(_MSC_VER)
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_be_pad_read=_ps2_pad_default_read")
#pragma comment(linker, "/alternatename:_be_pad_vibrate=_ps2_pad_default_vibrate")
#else
#pragma comment(linker, "/alternatename:be_pad_read=ps2_pad_default_read")
#pragma comment(linker, "/alternatename:be_pad_vibrate=ps2_pad_default_vibrate")
#endif
#endif
