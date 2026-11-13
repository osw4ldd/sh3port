/*
 * ISO9660 disc image reader for the HLE IOP (--hle-iop with --iso).
 *
 * Layout per ECMA-119 (same as recompiler/ps2recomp/iso.py): the primary
 * volume descriptor is sector 16 ("CD001" at offset 1), its root directory
 * record is at offset 156.  A directory record holds the extent LBA at +2,
 * the data length at +10 (little-endian halves), flags at +25 (bit 1 =
 * directory), the name length at +32 and the name at +33.  Records never
 * cross a sector boundary; a zero length byte skips to the next sector.
 */
#include "../internal.h"
#include "iso.h"
#include <windows.h>
#include <ctype.h>

#define SECTOR 2048u

static FILE *iso_fp;
static uint32_t root_lsn, root_size;
static CRITICAL_SECTION iso_lock;

int iso_open(const char *path)
{
    if (!path || !path[0])
        return 0;
    iso_fp = fopen(path, "rb");
    if (!iso_fp) {
        ps2_log("iso: cannot open %s", path);
        return 0;
    }
    InitializeCriticalSection(&iso_lock);
    uint8_t pvd[SECTOR];
    if (!iso_read(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) {
        ps2_log("iso: %s is not an ISO9660 image", path);
        fclose(iso_fp);
        iso_fp = NULL;
        return 0;
    }
    root_lsn = rd32le(pvd + 156 + 2);
    root_size = rd32le(pvd + 156 + 10);
    ps2_log("iso: %s (root at %u)", path, root_lsn);
    return 1;
}

int iso_is_open(void) { return iso_fp != NULL; }

int iso_read(uint32_t lsn, uint32_t count, void *dst)
{
    if (!iso_fp)
        return 0;
    EnterCriticalSection(&iso_lock);
    int ok = _fseeki64(iso_fp, (int64_t)lsn * SECTOR, SEEK_SET) == 0 &&
             fread(dst, SECTOR, count, iso_fp) == count;
    LeaveCriticalSection(&iso_lock);
    return ok;
}

int iso_read_bytes(uint64_t offset, uint32_t size, void *dst)
{
    if (!iso_fp)
        return 0;
    EnterCriticalSection(&iso_lock);
    int ok = _fseeki64(iso_fp, (int64_t)offset, SEEK_SET) == 0 &&
             fread(dst, 1, size, iso_fp) == size;
    LeaveCriticalSection(&iso_lock);
    return ok;
}

/* Compare one path component (no version) with a record name ("NAME.EXT;1"). */
static int name_eq(const char *comp, size_t n, const uint8_t *rec_name, size_t rn)
{
    size_t m = 0;
    while (m < rn && rec_name[m] != ';')
        m++;
    if (m && rec_name[m - 1] == '.')   /* "NAME." for names without extension */
        m--;
    if (m != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)comp[i]) != toupper(rec_name[i]))
            return 0;
    return 1;
}

static int find_in_dir(uint32_t dir_lsn, uint32_t dir_size, const char *comp, size_t n,
                       iso_entry *out)
{
    uint32_t sectors = (dir_size + SECTOR - 1) / SECTOR;
    uint8_t buf[SECTOR];
    for (uint32_t s = 0; s < sectors; s++) {
        if (!iso_read(dir_lsn + s, 1, buf))
            return 0;
        for (uint32_t o = 0; o < SECTOR;) {
            uint8_t len = buf[o];
            if (len == 0 || o + len > SECTOR)
                break;
            uint8_t nl = buf[o + 32];
            if (!(nl == 1 && (buf[o + 33] == 0 || buf[o + 33] == 1)) &&   /* skip . and .. */
                name_eq(comp, n, buf + o + 33, nl)) {
                out->lsn = rd32le(buf + o + 2);
                out->size = rd32le(buf + o + 10);
                out->is_dir = (buf[o + 25] & 2) != 0;
                memcpy(out->date, buf + o + 18, 7);
                size_t k = 0;
                for (; k < nl && k < sizeof out->name - 1; k++)
                    out->name[k] = (char)buf[o + 33 + k];
                out->name[k] = 0;
                return 1;
            }
            o += len;
        }
    }
    return 0;
}

int iso_find(const char *path, iso_entry *out)
{
    if (!iso_fp)
        return 0;
    const char *colon = strchr(path, ':');
    if (colon)
        path = colon + 1;
    uint32_t lsn = root_lsn, size = root_size;
    iso_entry e = {0};
    e.lsn = lsn;
    e.size = size;
    e.is_dir = 1;
    while (*path) {
        while (*path == '\\' || *path == '/')
            path++;
        if (!*path)
            break;
        const char *end = path;
        while (*end && *end != '\\' && *end != '/' && *end != ';')
            end++;
        if (!e.is_dir || !find_in_dir(e.lsn, e.size, path, (size_t)(end - path), &e))
            return 0;
        path = end;
        if (*path == ';')   /* version suffix ends the path */
            break;
    }
    *out = e;
    return 1;
}
