/* Soft backend: guest memory allocation and hardware I/O dispatch. */
#include "soft.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint8_t *ps2_vu0_code, *ps2_vu1_mem, *ps2_vu1_code;
static uint8_t *bios;

void soft_memory_init(void)
{
    ps2_ram = VirtualAlloc(NULL, PS2_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ps2_spr = _aligned_malloc(16384, 4096);
    ps2_vu0_code = _aligned_malloc(4096, 4096);
    ps2_vu0_mem = _aligned_malloc(4096, 4096);
    ps2_vu1_code = _aligned_malloc(16384, 4096);
    ps2_vu1_mem = _aligned_malloc(16384, 4096);
    bios = VirtualAlloc(NULL, 4u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ps2_ram || !ps2_spr || !bios)
        ps2_fatal("out of memory");
    memset(ps2_spr, 0, 16384);
    memset(ps2_vu0_code, 0, 4096);
    memset(ps2_vu0_mem, 0, 4096);
    memset(ps2_vu1_code, 0, 16384);
    memset(ps2_vu1_mem, 0, 16384);
    /* A recognisable ROMVER string for code that peeks at the BIOS. */
    memcpy(bios + 0x100, "0160EC20040929\0", 15);

    static const uint32_t ram_views[] = {0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xA0000000};
    for (unsigned i = 0; i < sizeof ram_views / sizeof ram_views[0]; i++)
        ps2_map_range(ram_views[i], ps2_ram, PS2_RAM_SIZE);
    ps2_map_range(0x70000000, ps2_spr, 16384);
    ps2_map_range(0x11000000, ps2_vu0_code, 4096);
    ps2_map_range(0x11004000, ps2_vu0_mem, 4096);
    ps2_map_range(0x11008000, ps2_vu1_code, 16384);
    ps2_map_range(0x1100C000, ps2_vu1_mem, 16384);
    static const uint32_t bios_views[] = {0x1FC00000, 0x9FC00000, 0xBFC00000};
    for (unsigned i = 0; i < 3; i++)
        ps2_map_range(bios_views[i], bios, 4u << 20);
}

/* -------------------------------------------------------------- I/O */
static uint32_t phys(uint32_t a)
{
    return a >= 0x80000000u ? (a & 0x1FFFFFFFu) : a;
}

static int is_hw(uint32_t p) { return (p >= 0x10000000u && p < 0x10010000u); }
static int is_gs(uint32_t p) { return (p >= 0x12000000u && p < 0x12002000u); }

static void unmapped(const char *what, uint32_t a)
{
    char key[64];
    snprintf(key, sizeof key, "%s%08X", what, a & ~0xFFFu);
    ps2_warn_once(key, "unmapped %s at %08X", what, a);
}

uint32_t ps2_io_read32(uint32_t a)
{
    uint32_t p = phys(a);
    if (is_hw(p))
        return hw_read32(p);
    if (is_gs(p))
        return (uint32_t)(gs_priv_read64(p & ~7u) >> ((p & 4) * 8));
    unmapped("read", a);
    return 0;
}

uint8_t ps2_io_read8(uint32_t a) { return (uint8_t)(ps2_io_read32(a & ~3u) >> ((a & 3) * 8)); }
uint16_t ps2_io_read16(uint32_t a) { return (uint16_t)(ps2_io_read32(a & ~3u) >> ((a & 2) * 8)); }

uint64_t ps2_io_read64(uint32_t a)
{
    uint32_t p = phys(a);
    if (is_hw(p))
        return hw_read64(p);
    if (is_gs(p))
        return gs_priv_read64(p);
    unmapped("read", a);
    return 0;
}

void ps2_io_read128(uint32_t a, u128 *v)
{
    uint32_t p = phys(a);
    if (is_hw(p)) {
        hw_read128(p, v);
        return;
    }
    v->UD[0] = ps2_io_read64(a);
    v->UD[1] = ps2_io_read64(a + 8);
}

void ps2_io_write32(uint32_t a, uint32_t v)
{
    uint32_t p = phys(a);
    if (is_hw(p)) {
        hw_write32(p, v);
        return;
    }
    if (is_gs(p)) {
        uint64_t old = gs_priv_read64(p & ~7u);
        int sh = (p & 4) * 8;
        old = (old & ~(0xFFFFFFFFull << sh)) | ((uint64_t)v << sh);
        gs_priv_write64(p & ~7u, old);
        return;
    }
    unmapped("write", a);
}

void ps2_io_write8(uint32_t a, uint8_t v)
{
    uint32_t p = phys(a);
    if (p == 0x1000F180u) {     /* SIO TX FIFO: EE serial console */
        hw_write32(p, v);
        return;
    }
    ps2_io_write32(a & ~3u, (uint32_t)v << ((a & 3) * 8));
}

void ps2_io_write16(uint32_t a, uint16_t v)
{
    ps2_io_write32(a & ~3u, (uint32_t)v << ((a & 2) * 8));
}

void ps2_io_write64(uint32_t a, uint64_t v)
{
    uint32_t p = phys(a);
    if (is_hw(p)) {
        hw_write64(p, v);
        return;
    }
    if (is_gs(p)) {
        gs_priv_write64(p, v);
        return;
    }
    unmapped("write", a);
}

void ps2_io_write128(uint32_t a, const u128 *v)
{
    uint32_t p = phys(a);
    if (is_hw(p)) {
        hw_write128(p, v);
        return;
    }
    ps2_io_write64(a, v->UD[0]);
    ps2_io_write64(a + 8, v->UD[1]);
}

