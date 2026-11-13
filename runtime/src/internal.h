/* Runtime-internal interfaces (not used by generated code). */
#ifndef PS2RT_INTERNAL_H
#define PS2RT_INTERNAL_H

#include "ps2rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ config */
typedef struct ps2_config {
    char host_root[1024];     /* host: device root directory */
    char cdrom_root[1024];    /* cdrom0: device root (extracted disc) */
    char mc_root[1024];       /* mc0:/mc1: root */
    char elf_name[256];
    char renderer[32];        /* PCSX2 backend: auto/vulkan/dx11/dx12/opengl/software */
    int scale;                /* window scale factor */
    int no_window;            /* headless (tests) */
    int max_frames;           /* exit after N vblanks (0 = never) */
    int trace_syscalls;
    char dump_frame[1024];    /* write last displayed frame as BMP on exit */
    /* --- fields below are also mirrored in host/NativeHost.h (keep order) */
    char iso_path[1024];      /* disc image: real IOP + CDVD (PCSX2 backend) */
    char bios_path[1024];     /* PS2 BIOS file or folder (enables the real IOP) */
    char boot_path[256];      /* argv[0] for the guest, e.g. cdrom0:\SLUS_123.45;1 */
    int hle_iop;              /* ignore the BIOS: HLE IOP (native, no PCSX2 IOP) */
    int reference;            /* PCSX2 build: run the disc fully emulated (comparison) */
    /* PCSX2 build: presentation (ps2native.ini / command line) */
    int upscale;              /* internal resolution multiplier (1 = native PS2) */
    int widescreen;           /* 16:9 output */
    int fullscreen;           /* borderless fullscreen (Alt+Enter toggles) */
    int fast_cdvd;            /* skip disc seek/read delays */
    int fxaa;
    int cas;                  /* contrast adaptive sharpening 0..100 (0 = off) */
    int aniso;                /* anisotropic filtering 0/2/4/8/16 */
    int vsync;
    int gpu;                  /* 1: prefer the discrete GPU */
} ps2_config;

extern ps2_config ps2_cfg;

/* Parse runtime options; returns 0 on success, >0 exit code otherwise. */
int ps2rt_parse_args(int argc, char **argv);
/* Initialise the runtime core and run the guest; does not return. */
void ps2rt_run(void);

/* ------------------------------------------------------------ memory */
void ps2_map_range(uint32_t vaddr, uint8_t *host, uint32_t size);

/* ------------------------------------------------------------ time */
double ps2_host_time(void);          /* seconds since start */
uint64_t ps2_ee_cycles(void);        /* emulated EE cycle counter */

/* ------------------------------------------------------------ constants */
#define INTC_GS 0
#define INTC_SBUS 1
#define INTC_VBLANK_S 2
#define INTC_VBLANK_E 3
#define INTC_VIF0 4
#define INTC_VIF1 5
#define INTC_VU0 6
#define INTC_VU1 7
#define INTC_IPU 8
#define INTC_TIM0 9
#define INTC_TIM1 10
#define INTC_TIM2 11
#define INTC_TIM3 12

#define DMAC_VIF0 0
#define DMAC_VIF1 1
#define DMAC_GIF 2
#define DMAC_FROM_IPU 3
#define DMAC_TO_IPU 4
#define DMAC_SIF0 5
#define DMAC_SIF1 6
#define DMAC_SIF2 7
#define DMAC_FROM_SPR 8
#define DMAC_TO_SPR 9

/* ================================================================
 * Backend interface.  Two implementations exist:
 *   - soft backend (runtime/src/soft_*.c): own DMA/VIF/GIF + software GS,
 *     builds with gcc, no dependencies.
 *   - PCSX2 backend (host/): PCSX2's DMAC, VIF, VU1, GIF and hardware GS
 *     renderers (Vulkan / D3D11 / D3D12 / OpenGL).
 * ================================================================ */
void be_init(void);                       /* allocate/map memory, video, ... */
uint32_t be_intc_pending(void);           /* INTC_STAT & INTC_MASK */
void be_intc_ack(uint32_t bits);
int be_intc_set_mask(int cause, int on);  /* returns previous mask bit */
uint32_t be_dmac_pending(void);           /* CIS & CIM, bits 0-9 */
void be_dmac_ack(uint32_t bits);
int be_dmac_set_mask(int ch, int on);     /* returns previous mask bit */
int be_dmac_masked_in(int ch);            /* CIM bit set */
void be_dmac_raise(int ch);               /* set CIS (used by SIF HLE) */
void be_update(void);                     /* advance time, run hardware events */
double be_next_event_time(void);          /* host time of next timed event */
void be_set_crt(int interlace, int mode, int ffmd);
uint32_t be_gs_imr(void);
void be_gs_set_imr(uint32_t v);
uint64_t be_gs_csr(void);
int be_should_quit(void);
void be_save_frame(const char *path);
void be_shutdown(int code);               /* called just before process exit */
int be_iop_lle(void);
void be_report(void);                     /* timing statistics (trace mode) */

/* VU0 register file exchanged with the backend for VCALLMS (micro mode).
 * Mirrored in host/NativeHost.h. */
typedef struct vu0_state {
    uint32_t vf[32][4];
    uint32_t acc[4];
    uint32_t vi[16];
    uint32_t q, i, p, r, status, mac, clip;
} vu0_state;
/* Runs the VU0 micro program at `addr` (instruction units) to completion.
 * Returns 0 if the backend has no VU0 micro mode. */
int be_vu0_exec(vu0_state *st, uint32_t addr);

/* Called by the backend at the start of every vblank. */
void kernel_on_vblank(void);

/* ------------------------------------------------------------ kernel */
void kernel_init(void);
void kernel_run_main(uint32_t entry);
void kernel_syscall(R5900 *c, int32_t num);
void kernel_deliver_interrupts(R5900 *c);
int kernel_interrupts_enabled(void);
PS2_NORETURN_PRE void kernel_exit(int code) PS2_NORETURN_POST;
extern int kernel_in_handler;
void kernel_idle_wait(void);
int kernel_thread_id(void);
void cop0_init(void);

/* ------------------------------------------------------------ SIF / IOP HLE */
void sif_init(void);
int sif_set_dma(uint32_t transfers, int count);
uint32_t sif_get_reg(uint32_t reg);
void sif_set_reg(uint32_t reg, uint32_t val);
void sif_dchain(void);
void sif_poll(void);
/* Real-IOP mode: EE kernel SIF services on top of the hardware SIF DMA. */
int sif_lle_set_dma(uint32_t transfers, int count);
int sif_lle_dma_stat(int id);
void sif_lle_dchain(void);
void sif_lle_stop_dma(void);
uint32_t sif_lle_get_reg(uint32_t reg);
void sif_lle_set_reg(uint32_t reg, uint32_t val);

/* ------------------------------------------------------------ helpers */
static inline uint32_t rd32le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline void wr32le(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

#ifdef __cplusplus
}
#endif

#endif
