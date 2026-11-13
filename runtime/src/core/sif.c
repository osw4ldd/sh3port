/*
 * IOP high-level emulation at the SIF protocol level.
 *
 * The EE side (ps2sdk / Sony libraries) talks to the IOP with SIF command
 * packets sent through the SifSetDma syscall.  We play the IOP: packets
 * addressed to the IOP command buffer are decoded here, RPC servers are
 * implemented natively, and replies are queued as SIF0 packets that are
 * written into the EE receive buffer, followed by a DMAC SIF0 interrupt.
 *
 * Servers: fileio (0x80000001), iopheap (0x80000003), loadfile
 * (0x80000006).  Unknown servers bind successfully and answer zeros.
 */
#include "../internal.h"
#include <ctype.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IOP_RAM_SIZE (2u << 20)
#define IOP_CMDBUF 0x0001F000u
#define IOP_SERVER_BASE 0x00010000u
#define IOP_BUF_BASE 0x00100000u
#define IOP_BUF_SIZE 0x8000u
#define IOP_HEAP_BASE 0x00180000u
#define IOP_HEAP_END 0x001F0000u

#define SIF_CMD_CHANGE_SADDR 0x80000000u
#define SIF_CMD_SET_SREG 0x80000001u
#define SIF_CMD_INIT_CMD 0x80000002u
#define SIF_CMD_RESET_CMD 0x80000003u
#define SIF_CMD_RPC_END 0x80000008u
#define SIF_CMD_RPC_BIND 0x80000009u
#define SIF_CMD_RPC_CALL 0x8000000Au
#define SIF_CMD_RPC_RDATA 0x8000000Cu

static uint8_t *iop_ram;
static uint32_t ee_pktbuf;
static int ee_ready = 1;
static uint32_t sif_regs[8];          /* 1 mainaddr, 2 subaddr, 3 msflag, 4 smflag */
static uint32_t sif_sysregs[8];       /* 0x80000000 + n */
static uint32_t dma_id;
/* IOP heap (sceSifAllocIopHeap / sceSifFreeIopHeap): first fit over the
 * IOP RAM not used by the HLE (no IRX code lives there).  Games allocate and
 * free large stream buffers (SH3: 266 KB, 160 KB, ...). */
#define MAX_HEAP_BLOCKS 256
static struct { uint32_t addr, size; } heap_used[MAX_HEAP_BLOCKS];
static int heap_nused;
static const struct { uint32_t lo, hi; } heap_arenas[] = {
    {0x00020000u, 0x00100000u}, {IOP_HEAP_BASE, IOP_HEAP_END}};

static uint32_t iop_heap_alloc(uint32_t size)
{
    size = (size + 255) & ~255u;
    if (!size || heap_nused >= MAX_HEAP_BLOCKS)
        return 0;
    for (size_t a = 0; a < sizeof heap_arenas / sizeof heap_arenas[0]; a++) {
        uint32_t p = heap_arenas[a].lo;
        for (;;) {
            uint32_t next = p;   /* first used block overlapping [p, p+size) */
            int hit = 0;
            for (int i = 0; i < heap_nused; i++)
                if (heap_used[i].addr < p + size && heap_used[i].addr + heap_used[i].size > p) {
                    hit = 1;
                    if (heap_used[i].addr + heap_used[i].size > next)
                        next = heap_used[i].addr + heap_used[i].size;
                }
            if (!hit) {
                if (p + size > heap_arenas[a].hi)
                    break;
                heap_used[heap_nused].addr = p;
                heap_used[heap_nused].size = size;
                heap_nused++;
                return p;
            }
            p = next;
            if (p + size > heap_arenas[a].hi)
                break;
        }
    }
    return 0;
}

static int iop_heap_free(uint32_t addr)
{
    for (int i = 0; i < heap_nused; i++)
        if (heap_used[i].addr == addr) {
            heap_used[i] = heap_used[--heap_nused];
            return 0;
        }
    return -1;
}

typedef struct out_pkt {
    uint8_t pkt[128];
    uint32_t psize;
    uint8_t *payload;
    uint32_t payload_size;
    uint32_t payload_dest;
} out_pkt;

#define QUEUE_LEN 256
static out_pkt queue[QUEUE_LEN];
static int q_head, q_tail;

static uint32_t iop_r32(uint32_t a) { return rd32le(iop_ram + (a & (IOP_RAM_SIZE - 4))); }
static void iop_w32(uint32_t a, uint32_t v) { wr32le(iop_ram + (a & (IOP_RAM_SIZE - 4)), v); }
static uint8_t *iop_ptr(uint32_t a) { return iop_ram + (a & (IOP_RAM_SIZE - 1)); }

static void queue_packet(const uint8_t *pkt, uint32_t psize, uint32_t cid,
                         const void *payload, uint32_t payload_size, uint32_t dest)
{
    int next = (q_tail + 1) % QUEUE_LEN;
    if (next == q_head) {
        ps2_warn_once("sifq", "SIF0 packet queue overflow");
        return;
    }
    out_pkt *o = &queue[q_tail];
    memset(o, 0, sizeof *o);
    memcpy(o->pkt, pkt, psize);
    o->psize = psize;
    /* header: psize:8 | dsize:24, dest, cid, opt */
    wr32le(o->pkt + 0, (psize & 0xFF) | (payload_size << 8));
    wr32le(o->pkt + 4, payload_size ? dest : 0);
    wr32le(o->pkt + 8, cid);
    if (payload_size) {
        o->payload = malloc(payload_size);
        memcpy(o->payload, payload, payload_size);
        o->payload_size = payload_size;
        o->payload_dest = dest;
    }
    q_tail = next;
    ps2_event_flag = 1;
}

void sif_poll(void)
{
    if (q_head == q_tail || !ee_pktbuf)
        return;
    /* The EE re-arms SIF0 (SifSetDChain) after consuming each packet. */
    if (!ee_ready && ps2_read8(ee_pktbuf) != 0)
        return;
    if (!be_dmac_masked_in(DMAC_SIF0))
        return;
    out_pkt *o = &queue[q_head];
    if (o->payload_size) {
        ps2_mem_write(o->payload_dest, o->payload, o->payload_size);
        free(o->payload);
    }
    ps2_mem_write(ee_pktbuf, o->pkt, o->psize);
    q_head = (q_head + 1) % QUEUE_LEN;
    ee_ready = 0;
    be_dmac_raise(DMAC_SIF0);
}

void sif_dchain(void)
{
    ee_ready = 1;
    if (q_head != q_tail)
        ps2_event_flag = 1;
}

static int iop_rebooting;

uint32_t sif_get_reg(uint32_t reg)
{
    if (reg & 0x80000000u)
        return sif_sysregs[reg & 7];
    if ((reg & 7) == 4 && iop_rebooting) {
        /* The EE clears SIFINIT/CMDINIT/BOOTEND after a reset request and
         * waits for the IOP to raise them again once it has rebooted. */
        iop_rebooting = 0;
        sif_regs[4] |= 0x70000;
        ee_pktbuf = 0;
    }
    return sif_regs[reg & 7];
}

void sif_set_reg(uint32_t reg, uint32_t val)
{
    if (reg & 0x80000000u) {
        sif_sysregs[reg & 7] = val;
        return;
    }
    switch (reg & 7) {
    case 3: sif_regs[3] = val; break;           /* MSFLAG: EE sets bits */
    case 4: sif_regs[4] &= ~val; break;         /* SMFLAG: EE clears bits */
    default: sif_regs[reg & 7] = val; break;
    }
}

/* ============================================================ fileio */
#define MAX_FDS 64

typedef struct host_file {
    int used;
    int is_dir;
    int fd;
    HANDLE find;
    WIN32_FIND_DATAA fdata;
    int find_first;
    char path[1024];
} host_file;

static host_file files[MAX_FDS];

static int map_path(const char *in, char *out, size_t outsz)
{
    const char *colon = strchr(in, ':');
    const char *root = ps2_cfg.host_root;
    const char *rest = in;
    char dev[16] = "";
    if (colon && colon - in < (int)sizeof dev) {
        memcpy(dev, in, (size_t)(colon - in));
        dev[colon - in] = 0;
        rest = colon + 1;
        for (char *p = dev; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        if (!strncmp(dev, "host", 4))
            root = ps2_cfg.host_root;
        else if (!strncmp(dev, "cdrom", 5) || !strcmp(dev, "cdfs"))
            root = ps2_cfg.cdrom_root;
        else if (!strncmp(dev, "mc", 2))
            root = ps2_cfg.mc_root;
        else if (!strncmp(dev, "tty", 3))
            return 1;
        else
            return -1;
    }
    char tmp[1024];
    size_t n = 0;
    for (const char *p = rest; *p && n + 1 < sizeof tmp; p++) {
        if (*p == ';')
            break;  /* ISO9660 version suffix */
        tmp[n++] = (*p == '\\') ? '/' : *p;
    }
    tmp[n] = 0;
    const char *rel = tmp;
    while (*rel == '/')
        rel++;
    if (!strncmp(dev, "mc", 2))
        snprintf(out, outsz, "%s/%s/%s", root, dev, rel);
    else
        snprintf(out, outsz, "%s/%s", root, rel);
    return 0;
}

static int alloc_fd(void)
{
    for (int i = 3; i < MAX_FDS; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof files[i]);
            files[i].used = 1;
            return i;
        }
    return -24; /* EMFILE */
}

static void ensure_parent_dirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char ch = *p;
            *p = 0;
            _mkdir(tmp);
            *p = ch;
        }
    }
}

static int fio_open(const char *name, uint32_t mode)
{
    char path[1024];
    int r = map_path(name, path, sizeof path);
    if (r == 1)
        return 1;   /* tty: stdout */
    if (r < 0)
        return -19; /* ENODEV */
    int flags = _O_BINARY;
    switch (mode & 3) {
    case 1: flags |= _O_RDONLY; break;
    case 2: flags |= _O_WRONLY; break;
    default: flags |= _O_RDWR; break;
    }
    if (mode & 0x100) flags |= _O_APPEND;
    if (mode & 0x200) flags |= _O_CREAT;
    if (mode & 0x400) flags |= _O_TRUNC;
    if (mode & 0x800) flags |= _O_EXCL;
    if (mode & 0x200)
        ensure_parent_dirs(path);
    int hfd = _open(path, flags, _S_IREAD | _S_IWRITE);
    if (ps2_cfg.trace_syscalls)
        ps2_log("fio open %s -> %s (%d)", name, path, hfd);
    if (hfd < 0)
        return -2;  /* ENOENT */
    int fd = alloc_fd();
    if (fd < 0) {
        _close(hfd);
        return fd;
    }
    files[fd].fd = hfd;
    snprintf(files[fd].path, sizeof files[fd].path, "%s", path);
    return fd;
}

static host_file *get_file(int fd)
{
    if (fd < 3 || fd >= MAX_FDS || !files[fd].used)
        return NULL;
    return &files[fd];
}

static void write_stat(uint32_t ee_addr, const struct _stat64 *st, int is_dir)
{
    uint8_t buf[40];
    memset(buf, 0, sizeof buf);
    uint32_t mode = is_dir ? (0x1000u | 0x1FF) : (0x2000u | 0x1FF);
    wr32le(buf + 0, mode);
    wr32le(buf + 4, is_dir ? 0x8427 : 0x8497);
    wr32le(buf + 8, (uint32_t)st->st_size);
    wr32le(buf + 36, (uint32_t)((uint64_t)st->st_size >> 32));
    ps2_mem_write(ee_addr, buf, sizeof buf);
}

/* Returns the 32-bit result placed at the start of the receive buffer. */
static int32_t fio_call(uint32_t fno, uint8_t *arg, uint32_t ssize)
{
    (void)ssize;
    switch (fno) {
    case 0: { /* open {mode, name[256]} */
        char name[257];
        memcpy(name, arg + 4, 256);
        name[256] = 0;
        return fio_open(name, rd32le(arg));
    }
    case 1: { /* close {fd} */
        int fd = (int32_t)rd32le(arg);
        host_file *f = get_file(fd);
        if (!f)
            return fd <= 2 ? 0 : -9;
        if (f->is_dir)
            FindClose(f->find);
        else
            _close(f->fd);
        f->used = 0;
        return 0;
    }
    case 2: { /* read {fd, ptr, size, read_data} */
        int fd = (int32_t)rd32le(arg);
        uint32_t ptr = rd32le(arg + 4), size = rd32le(arg + 8), rdata = rd32le(arg + 12);
        host_file *f = get_file(fd);
        if (rdata) {   /* we write everything directly: no head/tail fixups */
            ps2_write32(rdata + 0, 0);
            ps2_write32(rdata + 4, 0);
        }
        if (!f || f->is_dir)
            return -9;
        uint8_t *tmp = malloc(size ? size : 1);
        int n = _read(f->fd, tmp, size);
        if (n > 0)
            ps2_mem_write(ptr, tmp, (uint32_t)n);
        free(tmp);
        return n < 0 ? -5 : n;
    }
    case 3: { /* write {fd, ptr, size, mis, aligned[16]} */
        int fd = (int32_t)rd32le(arg);
        uint32_t ptr = rd32le(arg + 4), size = rd32le(arg + 8);
        uint8_t *tmp = malloc(size ? size : 1);
        ps2_mem_read(ptr, tmp, size);
        int n;
        if (fd == 1 || fd == 2) {
            fwrite(tmp, 1, size, stdout);
            fflush(stdout);
            n = (int)size;
        } else {
            host_file *f = get_file(fd);
            n = (f && !f->is_dir) ? _write(f->fd, tmp, size) : -9;
        }
        free(tmp);
        return n;
    }
    case 4: { /* lseek {fd, offset, whence} */
        host_file *f = get_file((int32_t)rd32le(arg));
        if (!f || f->is_dir)
            return -9;
        return (int32_t)_lseek(f->fd, (int32_t)rd32le(arg + 4), (int)rd32le(arg + 8));
    }
    case 5: /* ioctl */
        return -48;
    case 6: { /* remove {path} */
        char name[257], path[1024];
        memcpy(name, arg, 256);
        name[256] = 0;
        if (map_path(name, path, sizeof path))
            return -19;
        return remove(path) == 0 ? 0 : -2;
    }
    case 7: case 8: { /* mkdir / rmdir */
        char name[257], path[1024];
        memcpy(name, arg + (fno == 7 ? 4 : 0), 256);
        name[256] = 0;
        if (map_path(name, path, sizeof path))
            return -19;
        if (fno == 7)
            return _mkdir(path) == 0 ? 0 : -17;
        return _rmdir(path) == 0 ? 0 : -2;
    }
    case 9: { /* dopen {name} */
        char name[257], path[1024], pattern[1100];
        memcpy(name, arg, 256);
        name[256] = 0;
        if (map_path(name, path, sizeof path))
            return -19;
        snprintf(pattern, sizeof pattern, "%s/*", path);
        int fd = alloc_fd();
        if (fd < 0)
            return fd;
        files[fd].is_dir = 1;
        files[fd].find = FindFirstFileA(pattern, &files[fd].fdata);
        if (files[fd].find == INVALID_HANDLE_VALUE) {
            files[fd].used = 0;
            return -2;
        }
        files[fd].find_first = 1;
        return fd;
    }
    case 10: { /* dclose */
        host_file *f = get_file((int32_t)rd32le(arg));
        if (!f || !f->is_dir)
            return -9;
        FindClose(f->find);
        f->used = 0;
        return 0;
    }
    case 11: { /* dread {fd, io_dirent_t *buf} */
        host_file *f = get_file((int32_t)rd32le(arg));
        uint32_t buf = rd32le(arg + 4);
        if (!f || !f->is_dir)
            return -9;
        if (!f->find_first && !FindNextFileA(f->find, &f->fdata))
            return 0;
        f->find_first = 0;
        struct _stat64 st;
        memset(&st, 0, sizeof st);
        st.st_size = ((int64_t)f->fdata.nFileSizeHigh << 32) | f->fdata.nFileSizeLow;
        write_stat(buf, &st, (f->fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        char name[256];
        snprintf(name, sizeof name, "%s", f->fdata.cFileName);
        ps2_mem_write(buf + 40, name, (uint32_t)strlen(name) + 1);
        return 1;
    }
    case 12: { /* getstat {buf, name[256]} */
        uint32_t buf = rd32le(arg);
        char name[257], path[1024];
        memcpy(name, arg + 4, 256);
        name[256] = 0;
        if (map_path(name, path, sizeof path))
            return -19;
        struct _stat64 st;
        if (_stat64(path, &st) != 0)
            return -2;
        write_stat(buf, &st, (st.st_mode & _S_IFDIR) != 0);
        return 0;
    }
    default: {
        char key[32];
        snprintf(key, sizeof key, "fio%u", fno);
        ps2_warn_once(key, "fileio RPC function %u not implemented", fno);
        return -48;
    }
    }
}

/* ============================================================ servers */
/* Function-level HLE (hle_iop.c) reuses the host file layer. */
int32_t sif_fio(uint32_t fno, uint8_t *arg) { return fio_call(fno, arg, 0); }

typedef int32_t (*server_fn)(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply,
                             uint32_t *reply_size);

server_fn iop_service_for_sid(uint32_t sid);   /* iop_services.c */
int iop_dtx_dma(uint32_t ee_src, uint32_t iop_dest, uint32_t size);

typedef struct server {
    uint32_t sid;
    server_fn fn;
    uint32_t sd, buf;
} server;

static int32_t srv_fileio(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply, uint32_t *rsz)
{
    int32_t r = fio_call(fno, arg, ssize);
    wr32le(reply, (uint32_t)r);
    *rsz = 4;
    return r;
}

static int32_t srv_iopheap(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply, uint32_t *rsz)
{
    (void)ssize;
    uint32_t r = 0;
    switch (fno) {
    case 1: { /* alloc(size) */
        r = iop_heap_alloc(rd32le(arg));
        if (ps2_cfg.trace_syscalls || !r)
            ps2_log("iop heap alloc %X -> %X", rd32le(arg), r);
        break;
    }
    case 2: /* free(addr) */
        r = (uint32_t)iop_heap_free(rd32le(arg));
        break;
    case 3: { /* load file into IOP memory {addr, name[]} */
        uint32_t addr = rd32le(arg);
        char name[257], path[1024];
        memcpy(name, arg + 4, 252);
        name[252] = 0;
        if (map_path(name, path, sizeof path) == 0) {
            FILE *f = fopen(path, "rb");
            if (f) {
                size_t n = fread(iop_ptr(addr), 1, IOP_RAM_SIZE - (addr & (IOP_RAM_SIZE - 1)), f);
                fclose(f);
                r = (uint32_t)n;
            } else {
                r = (uint32_t)-2;
            }
        }
        break;
    }
    default:
        break;
    }
    wr32le(reply, r);
    *rsz = 4;
    return (int32_t)r;
}

static int32_t srv_loadfile(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply, uint32_t *rsz)
{
    (void)ssize;
    static int next_mod = 2;
    memset(reply, 0, 16);
    *rsz = 16;
    switch (fno) {
    case 0: case 4: case 6: { /* module load (file / buffer) */
        char path[253];
        memcpy(path, arg + 8, 252);
        path[252] = 0;
        if (ps2_verbose)
            ps2_log("IOP module load ignored (HLE): %s", fno == 6 ? "<buffer>" : path);
        wr32le(reply, (uint32_t)next_mod++);
        wr32le(reply + 4, 0);
        return 0;
    }
    case 1: case 5: /* ELF load onto EE */
        wr32le(reply, (uint32_t)-1);
        return -1;
    case 2: { /* SifIopSetVal {addr, value, type} */
        uint32_t addr = rd32le(arg), val = rd32le(arg + 4), type = rd32le(arg + 8);
        if (type == 1)
            iop_ptr(addr)[0] = (uint8_t)val;
        else if (type == 2)
            memcpy(iop_ptr(addr), &val, 2);
        else
            iop_w32(addr, val);
        return 0;
    }
    case 3: { /* SifIopGetVal {addr, type} */
        uint32_t addr = rd32le(arg);
        wr32le(reply, iop_r32(addr));
        return 0;
    }
    case 9: /* search module by name: pretend it is not loaded */
        wr32le(reply, (uint32_t)-203);
        return -203;
    default:
        return 0;
    }
}

static int32_t srv_dummy(uint32_t fno, uint8_t *arg, uint32_t ssize, uint8_t *reply, uint32_t *rsz)
{
    (void)fno; (void)arg; (void)ssize;
    memset(reply, 0, 64);
    *rsz = 0;   /* reply with zeros of the requested size */
    return 0;
}

#define MAX_SERVERS 64
static server servers[MAX_SERVERS];
static int nservers;

static server *server_for_sid(uint32_t sid)
{
    for (int i = 0; i < nservers; i++)
        if (servers[i].sid == sid)
            return &servers[i];
    if (nservers >= MAX_SERVERS)
        return NULL;
    server *s = &servers[nservers];
    s->sid = sid;
    s->sd = IOP_SERVER_BASE + (uint32_t)nservers * 0x100;
    s->buf = IOP_BUF_BASE + (uint32_t)nservers * IOP_BUF_SIZE;
    switch (sid) {
    case 0x80000001: s->fn = srv_fileio; break;
    case 0x80000003: s->fn = srv_iopheap; break;
    case 0x80000006: s->fn = srv_loadfile; break;
    default:
        s->fn = iop_service_for_sid(sid);
        if (!s->fn) {
            s->fn = srv_dummy;
            ps2_log("IOP RPC server 0x%08X is not emulated; calls will return zeros", sid);
        }
        break;
    }
    nservers++;
    return s;
}

static server *server_for_sd(uint32_t sd)
{
    for (int i = 0; i < nservers; i++)
        if (servers[i].sd == sd)
            return &servers[i];
    return NULL;
}

/* ============================================================ commands */
static void send_rpc_end(const uint8_t *req, uint32_t orig_cid, uint32_t sd, uint32_t buf,
                         const void *payload, uint32_t psize, uint32_t dest)
{
    uint8_t p[64];
    memset(p, 0, sizeof p);
    wr32le(p + 16, rd32le(req + 16));   /* rec_id */
    wr32le(p + 20, rd32le(req + 20));   /* pkt_addr */
    wr32le(p + 24, rd32le(req + 24));   /* rpc_id */
    wr32le(p + 28, rd32le(req + 28));   /* cd */
    wr32le(p + 32, orig_cid);
    wr32le(p + 36, sd);
    wr32le(p + 40, buf);
    wr32le(p + 44, 0);
    queue_packet(p, 64, SIF_CMD_RPC_END, payload, psize, dest);
}

static void iop_command(const uint8_t *pkt)
{
    uint32_t cid = rd32le(pkt + 8);
    uint32_t opt = rd32le(pkt + 12);
    if (ps2_cfg.trace_syscalls)
        ps2_log("SIF cmd %08X opt %u", cid, opt);
    switch (cid) {
    case SIF_CMD_CHANGE_SADDR:
        ee_pktbuf = rd32le(pkt + 16);
        break;
    case SIF_CMD_INIT_CMD:
        if (opt == 0) {
            ee_pktbuf = rd32le(pkt + 16);
        } else {
            uint8_t p[24];
            memset(p, 0, sizeof p);
            wr32le(p + 16, 0);   /* SIF_SREG_RPCINIT */
            wr32le(p + 20, 1);
            queue_packet(p, 24, SIF_CMD_SET_SREG, NULL, 0, 0);
        }
        break;
    case SIF_CMD_RESET_CMD:
        if (ps2_verbose)
            ps2_log("IOP reset requested (HLE: ignored)");
        nservers = 0;
        iop_rebooting = 1;
        break;
    case SIF_CMD_RPC_BIND: {
        uint32_t sid = rd32le(pkt + 32);
        server *s = server_for_sid(sid);
        send_rpc_end(pkt, SIF_CMD_RPC_BIND, s ? s->sd : 0, s ? s->buf : 0, NULL, 0, 0);
        break;
    }
    case SIF_CMD_RPC_CALL: {
        uint32_t fno = rd32le(pkt + 32), ssize = rd32le(pkt + 36);
        uint32_t recvbuf = rd32le(pkt + 40), rsize = rd32le(pkt + 44);
        uint32_t rmode = rd32le(pkt + 48), sd = rd32le(pkt + 52);
        server *s = server_for_sd(sd);
        static uint8_t reply[0x10000];
        uint32_t rlen = 0;
        memset(reply, 0, rsize < sizeof reply ? rsize : sizeof reply);
        if (s)
            s->fn(fno, iop_ptr(s->buf), ssize, reply, &rlen);
        if (rsize > sizeof reply)
            rsize = sizeof reply;
        if (rmode) {
            send_rpc_end(pkt, SIF_CMD_RPC_CALL, sd, 0, reply, rsize, recvbuf);
        } else {
            if (rsize)
                ps2_mem_write(recvbuf, reply, rsize);
            uint32_t pkt_addr = rd32le(pkt + 20);
            ps2_write32(pkt_addr + 24, 0);                               /* rpc_id */
            ps2_write32(pkt_addr + 16, ps2_read32(pkt_addr + 16) & ~1u); /* rec_id */
        }
        break;
    }
    case SIF_CMD_RPC_RDATA: {
        uint32_t src = rd32le(pkt + 32), dest = rd32le(pkt + 36), size = rd32le(pkt + 40);
        uint8_t p[64];
        memset(p, 0, sizeof p);
        wr32le(p + 16, rd32le(pkt + 16));
        wr32le(p + 20, rd32le(pkt + 20));
        wr32le(p + 24, rd32le(pkt + 24));
        wr32le(p + 28, rd32le(pkt + 28));
        wr32le(p + 32, SIF_CMD_RPC_RDATA);
        if (size > IOP_RAM_SIZE)
            size = 0;
        queue_packet(p, 64, SIF_CMD_RPC_END, iop_ptr(src), size, dest);
        break;
    }
    default: {
        char key[32];
        snprintf(key, sizeof key, "sifcmd%08X", cid);
        ps2_warn_once(key, "SIF command %08X ignored", cid);
        break;
    }
    }
}

int sif_set_dma(uint32_t transfers, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t t = transfers + (uint32_t)i * 16;
        uint32_t src = ps2_read32(t), dest = ps2_read32(t + 4), size = ps2_read32(t + 8);
        dest &= IOP_RAM_SIZE - 1;
        if (size > IOP_RAM_SIZE - dest)
            size = IOP_RAM_SIZE - dest;
        ps2_mem_read(src, iop_ptr(dest), size);
        if (iop_dtx_dma(src, dest, size))
            continue;
        if (dest == IOP_CMDBUF)
            iop_command(iop_ptr(IOP_CMDBUF));
    }
    return (int)++dma_id;
}

void sif_init(void)
{
    iop_ram = calloc(IOP_RAM_SIZE, 1);
    if (!iop_ram)
        ps2_fatal("cannot allocate IOP memory");
    sif_regs[2] = IOP_CMDBUF;                  /* SUBADDR */
    sif_regs[4] = 0x10000 | 0x20000 | 0x40000; /* SIFINIT | CMDINIT | BOOTEND */
}
