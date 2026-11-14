/* Guest page map, bulk memory helpers, logging and time. */
#include "../internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint8_t *ps2_ram;
uint8_t *ps2_spr;
uint8_t *ps2_vu0_mem;
uint8_t *ps2_pagemap[1u << (32 - PS2_PAGE_BITS)];
int ps2_verbose;

void ps2_map_range(uint32_t vaddr, uint8_t *host, uint32_t size)
{
    for (uint32_t off = 0; off < size; off += 1u << PS2_PAGE_BITS)
        ps2_pagemap[(vaddr + off) >> PS2_PAGE_BITS] = host ? host + off : NULL;
}

/* ------------------------------------------------------ bulk helpers */
void ps2_mem_read(uint32_t a, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t chunk = 4096 - (a & 4095);
        uint8_t *p = ps2_hostptr(a);
        if (chunk > n)
            chunk = n;
        if (p)
            memcpy(d, p, chunk);
        else
            memset(d, 0, chunk);
        a += chunk;
        d += chunk;
        n -= chunk;
    }
}

void ps2_mem_write(uint32_t a, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    while (n) {
        uint32_t chunk = 4096 - (a & 4095);
        uint8_t *p = ps2_hostptr(a);
        if (chunk > n)
            chunk = n;
        if (p)
            memcpy(p, s, chunk);
        a += chunk;
        s += chunk;
        n -= chunk;
    }
}

uint32_t ps2_mem_strlen(uint32_t a, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && ps2_read8(a + n))
        n++;
    return n;
}

void ps2_mem_readstr(uint32_t a, char *dst, uint32_t max)
{
    uint32_t i = 0;
    if (!max)
        return;
    for (; i + 1 < max; i++) {
        char ch = (char)ps2_read8(a + i);
        dst[i] = ch;
        if (!ch)
            return;
    }
    dst[i] = 0;
}

/* -------------------------------------------------------------- time */
static LARGE_INTEGER t0, freq;

double ps2_host_time(void)
{
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

uint64_t ps2_ee_cycles(void)
{
    return (uint64_t)(ps2_host_time() * 294912000.0);
}

/* ----------------------------------------------------------- logging */
void ps2_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[ps2 %8.3f] ", ps2_host_time());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define WARN_SLOTS 512
static char warned[WARN_SLOTS][64];
static int nwarned;

void ps2_warn_once(const char *key, const char *fmt, ...)
{
    va_list ap;
    for (int i = 0; i < nwarned; i++)
        if (!strcmp(warned[i], key))
            return;
    if (nwarned < WARN_SLOTS) {
        strncpy(warned[nwarned], key, 63);
        nwarned++;
    }
    va_start(ap, fmt);
    fputs("[ps2] warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void ps2_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[ps2] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    sif_rpc_report(1);   /* RPC coverage up to the failure (HLE IOP only) */
    fflush(stdout);
    fflush(stderr);
    ExitProcess(3);
}
