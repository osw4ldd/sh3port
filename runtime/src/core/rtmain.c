/* Runtime start-up shared by all backends: options, ELF image, guest boot. */
#include "../internal.h"
#include <xmmintrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

ps2_config ps2_cfg;
uint8_t ps2_opt_flags[PS2_MAX_OPTIONS];

static void CALLBACK tick(void *param, BOOLEAN fired)
{
    (void)param;
    (void)fired;
    ps2_event_flag = 1;
}

static uint32_t load_image(void)
{
    uint32_t size = 0;
    const unsigned char *img = ps2_get_elf_image(&size);
    if (!img || size < 52 || memcmp(img, "\x7f" "ELF", 4))
        ps2_fatal("embedded ELF image is invalid");
    uint32_t entry = rd32le(img + 24), phoff = rd32le(img + 28);
    uint16_t phentsize, phnum;
    memcpy(&phentsize, img + 42, 2);
    memcpy(&phnum, img + 44, 2);
    for (int i = 0; i < phnum; i++) {
        const unsigned char *ph = img + phoff + (uint32_t)i * phentsize;
        uint32_t type = rd32le(ph), off = rd32le(ph + 4), vaddr = rd32le(ph + 8);
        uint32_t filesz = rd32le(ph + 16), memsz = rd32le(ph + 20);
        if (type != 1 || !memsz)
            continue;
        if (off + filesz > size)
            ps2_fatal("ELF segment %d exceeds image", i);
        ps2_mem_write(vaddr, img + off, filesz);
        if (memsz > filesz) {
            uint8_t *zero = calloc(memsz - filesz, 1);
            ps2_mem_write(vaddr + filesz, zero, memsz - filesz);
            free(zero);
        }
        if (ps2_verbose)
            ps2_log("segment %08X-%08X (file %u bytes)", vaddr, vaddr + memsz, filesz);
    }
    return entry;
}

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "  --host DIR        root directory for host: paths (default: exe dir)\n"
           "  --cdrom DIR       root directory for cdrom0: (extracted disc)\n"
           "  --mc DIR          memory card root (default: <host>/memcards)\n"
           "  --renderer NAME   auto, vulkan, dx11, dx12, opengl, software (PCSX2 build)\n"
           "  --iso FILE        disc image (PCSX2 build: real CD/DVD drive emulation)\n"
           "  --bios PATH       PS2 BIOS file or folder (PCSX2 build: real IOP, audio, pad)\n"
           "  --boot PATH       guest argv[0], e.g. cdrom0:\\SLUS_123.45;1\n"
           "  --scale N         window scale factor (default 1)\n"
           "  --upscale N       internal resolution x N (default 3)\n"
           "  --widescreen      16:9 output (needs the game's widescreen patch)\n"
           "  --fullscreen      borderless fullscreen (Alt+Enter toggles)\n"
           "  --headless        no window\n"
           "  --frames N        exit after N vblanks\n"
           "  --dump-frame F    save the last displayed frame as BMP on exit\n"
           "  --trace           log syscalls and SIF traffic\n"
           "  -v                verbose\n", argv0);
}

static void dirname_of_exe(char *out, size_t n)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *slash = strrchr(path, '\\');
    if (slash)
        *slash = 0;
    snprintf(out, n, "%s", path);
}

/* Game-specific options generated from the game config (e.g. fps60). */
static int set_game_option(const char *name, const char *val)
{
    for (uint32_t i = 0; i < ps2_option_count && i < PS2_MAX_OPTIONS; i++) {
        if (!strcmp(name, ps2_option_names[i])) {
            ps2_opt_flags[i] = atoi(val) != 0;
            return 1;
        }
    }
    return 0;
}

/* Defaults written by the recompiler next to the exe (key=value lines):
 *   iso=<disc image>   bios=<BIOS file or folder>   boot=cdrom0:\SLUS_123.45;1 */
static void load_ini(void)
{
    char path[1100], line[1200];
    snprintf(path, sizeof path, "%s/ps2native.ini", ps2_cfg.host_root);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';')
            continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcmp(line, "iso"))
            snprintf(ps2_cfg.iso_path, sizeof ps2_cfg.iso_path, "%s", val);
        else if (!strcmp(line, "bios"))
            snprintf(ps2_cfg.bios_path, sizeof ps2_cfg.bios_path, "%s", val);
        else if (!strcmp(line, "boot"))
            snprintf(ps2_cfg.boot_path, sizeof ps2_cfg.boot_path, "%s", val);
        else if (!strcmp(line, "renderer"))
            snprintf(ps2_cfg.renderer, sizeof ps2_cfg.renderer, "%s", val);
        else if (!strcmp(line, "upscale"))
            ps2_cfg.upscale = atoi(val);
        else if (!strcmp(line, "widescreen"))
            ps2_cfg.widescreen = atoi(val);
        else if (!strcmp(line, "fullscreen"))
            ps2_cfg.fullscreen = atoi(val);
        else if (!strcmp(line, "fast_cdvd"))
            ps2_cfg.fast_cdvd = atoi(val);
        else if (!strcmp(line, "fxaa"))
            ps2_cfg.fxaa = atoi(val);
        else if (!strcmp(line, "sharpen"))
            ps2_cfg.cas = atoi(val);
        else if (!strcmp(line, "anisotropy"))
            ps2_cfg.aniso = atoi(val);
        else if (!strcmp(line, "vsync"))
            ps2_cfg.vsync = atoi(val);
        else if (set_game_option(line, val))
            ;
        else if (!strcmp(line, "gpu"))
            ps2_cfg.gpu = strcmp(val, "integrated") != 0 && strcmp(val, "0") != 0;
    }
    fclose(f);
}

/* Returns 0 to continue, -1 for "exit successfully", >0 for an error. */
int ps2rt_parse_args(int argc, char **argv)
{
    memset(&ps2_cfg, 0, sizeof ps2_cfg);
    dirname_of_exe(ps2_cfg.host_root, sizeof ps2_cfg.host_root);
    snprintf(ps2_cfg.cdrom_root, sizeof ps2_cfg.cdrom_root, "%s/cdrom", ps2_cfg.host_root);
    snprintf(ps2_cfg.renderer, sizeof ps2_cfg.renderer, "auto");
    ps2_cfg.scale = 1;
    ps2_cfg.upscale = 3;
    ps2_cfg.fast_cdvd = 1;
    ps2_cfg.aniso = 16;
    ps2_cfg.vsync = 0;
    ps2_cfg.gpu = 1;
    snprintf(ps2_cfg.elf_name, sizeof ps2_cfg.elf_name, "%s", ps2_game_name);
    load_ini();
    int mc_set = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && next) {
            snprintf(ps2_cfg.host_root, sizeof ps2_cfg.host_root, "%s", next);
            i++;
        } else if (!strcmp(a, "--cdrom") && next) {
            snprintf(ps2_cfg.cdrom_root, sizeof ps2_cfg.cdrom_root, "%s", next);
            i++;
        } else if (!strcmp(a, "--mc") && next) {
            snprintf(ps2_cfg.mc_root, sizeof ps2_cfg.mc_root, "%s", next);
            mc_set = 1;
            i++;
        } else if (!strcmp(a, "--iso") && next) {
            snprintf(ps2_cfg.iso_path, sizeof ps2_cfg.iso_path, "%s", next);
            i++;
        } else if (!strcmp(a, "--bios") && next) {
            snprintf(ps2_cfg.bios_path, sizeof ps2_cfg.bios_path, "%s", next);
            i++;
        } else if (!strcmp(a, "--boot") && next) {
            snprintf(ps2_cfg.boot_path, sizeof ps2_cfg.boot_path, "%s", next);
            i++;
        } else if (!strcmp(a, "--renderer") && next) {
            snprintf(ps2_cfg.renderer, sizeof ps2_cfg.renderer, "%s", next);
            i++;
        } else if (!strcmp(a, "--scale") && next) {
            ps2_cfg.scale = atoi(next);
            i++;
        } else if (!strcmp(a, "--headless")) {
            ps2_cfg.no_window = 1;
        } else if (!strcmp(a, "--frames") && next) {
            ps2_cfg.max_frames = atoi(next);
            i++;
        } else if (!strcmp(a, "--dump-frame") && next) {
            snprintf(ps2_cfg.dump_frame, sizeof ps2_cfg.dump_frame, "%s", next);
            i++;
        } else if (!strcmp(a, "--upscale") && next) {
            ps2_cfg.upscale = atoi(next);
            i++;
        } else if (!strcmp(a, "--widescreen")) {
            ps2_cfg.widescreen = 1;
        } else if (!strcmp(a, "--no-widescreen")) {
            ps2_cfg.widescreen = 0;
        } else if (!strcmp(a, "--fullscreen")) {
            ps2_cfg.fullscreen = 1;
        } else if (!strcmp(a, "--windowed")) {
            ps2_cfg.fullscreen = 0;
        } else if (!strcmp(a, "--reference")) {
            ps2_cfg.reference = 1;
        } else if (!strcmp(a, "--trace")) {
            ps2_cfg.trace_syscalls = 1;
            ps2_verbose = 1;
        } else if (!strcmp(a, "-v")) {
            ps2_verbose = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return -1;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }
    if (!mc_set)
        snprintf(ps2_cfg.mc_root, sizeof ps2_cfg.mc_root, "%s/memcards", ps2_cfg.host_root);
    return 0;
}

void kernel_on_vblank(void);

void ps2rt_run(void)
{
    /* PS2 FPU: denormals flush to zero and results round toward zero
     * (PCSX2 uses the same MXCSR setting for the EE). */
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IOLBF, 4096);

    ps2_host_time();
    cop0_init();
    be_init();
    if (!be_iop_lle()) {
        sif_init();
    } else {
        /* What the EE kernel does at boot: arm SIF0 reception and tell the
         * IOP (waiting in sceSifInit) that the EE side of the SIF is up. */
        sif_lle_dchain();
        sif_lle_set_reg(3, 0x10000);   /* MSFLAG |= SIF_STAT_SIFINIT */
    }
    kernel_init();

    uint32_t entry = load_image();
    if (ps2_verbose)
        ps2_log("%s: %u functions, entry %08X", ps2_game_name, ps2_func_count, entry);

    HANDLE timer;
    CreateTimerQueueTimer(&timer, NULL, tick, NULL, 1, 1, WT_EXECUTEDEFAULT);

    kernel_run_main(entry);
}
