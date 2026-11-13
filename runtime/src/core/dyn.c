/* Guest code that only exists while the game runs (overlays).
 *
 * Some games load code from the disc into memory and jump to it (Silent Hill
 * 3 loads its stage code into an empty executable ELF segment).  Static
 * recompilation cannot see that code, so it is recompiled here the first time
 * it is reached: the region is captured, translated by the recompiler
 * (ps2recomp.dyn), built into a DLL (scripts/dyncompile.bat) and loaded.
 * DLLs are kept in <exe dir>/dyncache and loaded at start, so each piece of
 * code is compiled once.
 *
 * Several overlays share the same addresses, so a module entry is only used
 * while the first instruction words in memory match the ones it was compiled
 * from.  Same approach as ps2-native-recomp (MIT), which keeps a few original
 * instructions with each translated piece and checks them before use. */
#include "../internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_MODULES 512
#define MEMO_SIZE 4096 /* power of two */
#define MAX_ENTRIES 256

typedef struct dyn_module {
    const ps2_dyn_entry *tab;
    uint32_t n;
} dyn_module;

/* MIPS control transfer (its next word is a delay slot).  Idea from
 * ran-j/PS2Recomp's ELF analyzer: a code pointer that lands right after a
 * branch names a delay slot and must not start a function. */
static int is_branch(uint32_t w)
{
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    if (op == 0)
        return (w & 0x3F) == 8 || (w & 0x3F) == 9;          /* jr, jalr */
    if (op == 1)
        return (rt & 0x0C) == 0;                             /* bltz..bgezall */
    if (op >= 2 && op <= 7)
        return 1;                                            /* j, jal, beq..bgtz */
    if (op >= 20 && op <= 23)
        return 1;                                            /* beql..bgtzl */
    if ((op == 16 || op == 17 || op == 18) && rs == 8)
        return 1;                                            /* bc0/bc1/bc2 */
    return 0;
}

static dyn_module modules[MAX_MODULES];
static int nmodules;
static int initialized;
static char cache_dir[1100];

/* addr -> entry, re-validated on every use */
static const ps2_dyn_entry *memo[MEMO_SIZE];

/* Entries requested so far per region (so a rebuild keeps the earlier ones). */
static uint32_t req_entries[MAX_ENTRIES], req_words[MAX_ENTRIES];
static int req_count;


static int entry_valid(const ps2_dyn_entry *e)
{
    for (int i = 0; i < 4; i++)
        if (ps2_read32(e->addr + 4u * (uint32_t)i) != e->words[i])
            return 0;
    return 1;
}

static void add_module(const char *path)
{
    if (nmodules >= MAX_MODULES)
        return;
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        ps2_log("dyn: cannot load %s (error %lu)", path, GetLastError());
        return;
    }
    const ps2_dyn_entry *tab = (const ps2_dyn_entry *)GetProcAddress(h, "ps2dyn_table");
    const uint32_t *count = (const uint32_t *)GetProcAddress(h, "ps2dyn_count");
    if (!tab || !count) {
        ps2_log("dyn: %s has no entry table", path);
        FreeLibrary(h);
        return;
    }
    modules[nmodules].tab = tab;
    modules[nmodules].n = *count;
    nmodules++;
    if (ps2_verbose)
        ps2_log("dyn: loaded %s (%u entries)", path, *count);
}

static void dyn_init(void)
{
    initialized = 1;
    snprintf(cache_dir, sizeof cache_dir, "%s\\dyncache", ps2_cfg.host_root);
    CreateDirectoryA(cache_dir, NULL);
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s\\*.dll", cache_dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        char path[1400];
        snprintf(path, sizeof path, "%s\\%s", cache_dir, fd.cFileName);
        add_module(path);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

/* Newest module first: a rebuild with more entries supersedes older ones. */
static const ps2_dyn_entry *find_entry(uint32_t addr)
{
    for (int m = nmodules - 1; m >= 0; m--) {
        const ps2_dyn_entry *tab = modules[m].tab;
        uint32_t lo = 0, hi = modules[m].n;
        while (lo < hi) { /* tables are sorted by address */
            uint32_t mid = (lo + hi) / 2;
            if (tab[mid].addr < addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < modules[m].n && tab[lo].addr == addr && entry_valid(&tab[lo]))
            return &tab[lo];
    }
    return NULL;
}

/* Executable ELF segments that are empty in the file: code is put there at
 * run time.  Overlay ELFs describe each overlay as its own segment, several
 * starting at the same address with different sizes, so the region is the
 * union of all such segments that overlap the one containing addr.
 * Returns 0 if addr is not in such a region. */
static int code_region(uint32_t addr, uint32_t *base, uint32_t *size)
{
    uint32_t len = 0;
    const unsigned char *img = ps2_get_elf_image(&len);
    if (!img || len < 52)
        return 0;
    uint32_t phoff = rd32le(img + 28);
    uint16_t phentsize, phnum;
    memcpy(&phentsize, img + 42, 2);
    memcpy(&phnum, img + 44, 2);
    uint32_t lo = 0, hi = 0;
    for (int pass = 0; pass < 8; pass++) { /* grow until stable */
        int grown = 0;
        for (int i = 0; i < phnum; i++) {
            const unsigned char *ph = img + phoff + (uint32_t)i * phentsize;
            uint32_t type = rd32le(ph), vaddr = rd32le(ph + 8), filesz = rd32le(ph + 16);
            uint32_t memsz = rd32le(ph + 20), flags = rd32le(ph + 24);
            if (type != 1 || !(flags & 1) || memsz <= filesz)
                continue;
            uint32_t s = vaddr + filesz, e = vaddr + memsz;
            int hit = hi ? (s < hi && e > lo) : (addr >= s && addr < e);
            if (!hit)
                continue;
            if (!hi) {
                lo = s;
                hi = e;
                grown = 1;
            } else {
                if (s < lo) { lo = s; grown = 1; }
                if (e > hi) { hi = e; grown = 1; }
            }
        }
        if (!grown)
            break;
    }
    if (!hi)
        return 0;
    *base = lo;
    *size = hi - lo;
    return 1;
}

static uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* Find the folder with scripts\dyncompile.bat: the ini key dyn_root, else
 * walk up from the exe directory (development builds live in the project). */
static int find_tools(char *root, size_t n)
{
    char path[1200];
    snprintf(root, n, "%s", ps2_cfg.host_root);
    for (int up = 0; up < 6; up++) {
        snprintf(path, sizeof path, "%s\\scripts\\dyncompile.bat", root);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return 1;
        char *slash = strrchr(root, '\\');
        if (!slash)
            slash = strrchr(root, '/');
        if (!slash)
            return 0;
        *slash = 0;
    }
    return 0;
}

static int compile_region(uint32_t base, uint32_t size)
{
    char root[1100];
    if (!find_tools(root, sizeof root)) {
        ps2_log("dyn: scripts\\dyncompile.bat not found above %s; cannot compile run-time code",
                ps2_cfg.host_root);
        return 0;
    }
    uint8_t *data = malloc(size);
    if (!data)
        return 0;
    ps2_mem_read(base, data, size);
    uint64_t h = fnv1a(data, size, 0xCBF29CE484222325ull);
    for (int i = 0; i < req_count; i++)
        h = fnv1a((const uint8_t *)&req_entries[i], 4, h);
    char name[64], blob[1300], dll[1300], lib[1300], entries[MAX_ENTRIES * 24 + 8] = "", cmd[9000];
    snprintf(name, sizeof name, "ovl_%08X_%016llX", base, (unsigned long long)h);
    snprintf(blob, sizeof blob, "%s\\%s.bin", cache_dir, name);
    snprintf(dll, sizeof dll, "%s\\%s.dll", cache_dir, name);
    FILE *f = fopen(blob, "wb");
    if (!f) {
        free(data);
        return 0;
    }
    fwrite(data, 1, size, f);
    fclose(f);
    free(data);
    for (int i = 0; i < req_count; i++) {
        char one[16];
        snprintf(one, sizeof one, "%s0x%08X", i ? "," : "", req_entries[i]);
        strcat(entries, one);
    }
    /* Seed more entries so one build covers most of the overlay: aligned
     * pointers into the region in the program's data whose target starts a
     * function (addiu sp, sp, -n). */
    {
        uint32_t len = 0;
        const unsigned char *img = ps2_get_elf_image(&len);
        uint32_t phoff = rd32le(img + 28);
        uint16_t phentsize, phnum;
        memcpy(&phentsize, img + 42, 2);
        memcpy(&phnum, img + 44, 2);
        int seeds = 0;
        for (int i = 0; i < phnum && seeds < MAX_ENTRIES; i++) {
            const unsigned char *ph = img + phoff + (uint32_t)i * phentsize;
            uint32_t vaddr = rd32le(ph + 8), filesz = rd32le(ph + 16);
            if (rd32le(ph) != 1 || !filesz)
                continue;
            for (uint32_t o = 0; o + 4 <= filesz && seeds < MAX_ENTRIES; o += 4) {
                uint32_t v = ps2_read32(vaddr + o); /* current data, not the file */
                if (v < base || v >= base + size || (v & 3))
                    continue;
                uint32_t w = ps2_read32(v);
                if ((w >> 16) != 0x27BDu || !(w & 0x8000u))
                    continue;
                if (v > base && is_branch(ps2_read32(v - 4)))
                    continue;   /* a delay slot, not a function start */
                char one[16];
                snprintf(one, sizeof one, "%s0x%08X", entries[0] ? "," : "", v);
                if (strstr(entries, one + (entries[0] ? 1 : 0)))
                    continue;
                strcat(entries, one);
                seeds++;
            }
        }
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(lib, sizeof lib, "%s", exe);
    char *dot = strrchr(lib, '.');
    if (dot)
        strcpy(dot, ".lib");
    snprintf(cmd, sizeof cmd, "cmd.exe /s /c \"\"%s\\scripts\\dyncompile.bat\" \"%s\" 0x%08X \"%s\" \"%s\" \"%s\" > \"%s\\%s.log\" 2>&1\"",
             root, blob, base, dll, lib, entries, cache_dir, name);
    ps2_log("dyn: compiling code at %08X (%u entries), the game waits until it is ready...", base, req_count);
    double t0 = ps2_host_time();
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ps2_log("dyn: cannot start the compiler (error %lu)", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0 || GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        ps2_log("dyn: compile failed (exit %lu), see %s\\%s.log", code, cache_dir, name);
        return 0;
    }
    ps2_log("dyn: compiled in %.1f s", ps2_host_time() - t0);
    add_module(dll);
    return 1;
}

ps2_fn ps2_dyn_lookup(uint32_t addr, int *secondary)
{
    if (!initialized)
        dyn_init();
    uint32_t slot = (addr >> 2) & (MEMO_SIZE - 1);
    const ps2_dyn_entry *e = memo[slot];
    if (!(e && e->addr == addr && entry_valid(e))) {
        e = find_entry(addr);
        if (!e) {
            uint32_t base, size;
            if (!code_region(addr, &base, &size))
                return NULL;
            /* Keep earlier entries that are still the same code in this region
             * (another overlay may have replaced it since). */
            int keep = 0, known = 0;
            for (int i = 0; i < req_count; i++) {
                if (req_entries[i] < base || req_entries[i] >= base + size ||
                    ps2_read32(req_entries[i]) != req_words[i])
                    continue;
                known |= req_entries[i] == addr;
                req_entries[keep] = req_entries[i];
                req_words[keep++] = req_words[i];
            }
            req_count = keep;
            if (!known && req_count < MAX_ENTRIES) {
                req_entries[req_count] = addr;
                req_words[req_count++] = ps2_read32(addr);
            }
            if (!compile_region(base, size))
                return NULL;
            e = find_entry(addr);
            if (!e)
                return NULL;
        }
        memo[slot] = e;
    }
    if (secondary)
        *secondary = (int)e->secondary;
    return e->fn;
}
