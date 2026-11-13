/* Soft backend internals: own hardware model + software GS. */
#ifndef PS2RT_SOFT_H
#define PS2RT_SOFT_H

#include "../internal.h"

extern uint8_t *ps2_vu1_mem;    /* 16 KiB data */
extern uint8_t *ps2_vu1_code;   /* 16 KiB micro */
extern uint8_t *ps2_vu0_code;   /* 4 KiB micro */

/* hardware registers / time */
extern uint32_t hw_intc_stat, hw_intc_mask;
extern uint32_t hw_dmac_stat;          /* D_STAT: CIS bits 0-9, CIM bits 16-25 */
void hw_init(void);
void hw_raise_intc(int cause);
void hw_dma_complete(int channel);
void hw_update_time(void);
double hw_next_event_time(void);
void hw_set_video_rate(int pal);
uint32_t hw_read32(uint32_t a);
void hw_write32(uint32_t a, uint32_t v);
uint64_t hw_read64(uint32_t a);
void hw_write64(uint32_t a, uint64_t v);
void hw_read128(uint32_t a, u128 *v);
void hw_write128(uint32_t a, const u128 *v);
extern uint64_t hw_vblank_count;

/* DMA / VIF / GIF */
typedef struct dma_chan {
    uint32_t chcr, madr, qwc, tadr, asr0, asr1, sadr;
} dma_chan;
extern dma_chan hw_dma[10];
void dma_start(int channel);
void gif_packet(const uint8_t *data, uint32_t qwc, int path);
void vif_process(int unit, const uint8_t *data, uint32_t qwc);
void vif_words(int unit, const uint32_t *w, uint32_t n);

/* GS */
void gs_init(void);
void gs_write_reg(int reg, uint64_t v);
void gs_gif_tag(const u128 *tag);
void gs_priv_write64(uint32_t a, uint64_t v);
uint64_t gs_priv_read64(uint32_t a);
void gs_vblank(int start);
uint32_t gs_imr(void);
void gs_set_imr(uint32_t v);
void gs_set_crt(int interlace, int mode, int ffmd);
int gs_read_display(uint32_t *dst, int *w, int *h, int maxw, int maxh);
void gs_image_data(const uint8_t *data, uint32_t qwc);

/* display */
void display_init(void);
void display_present(void);
void display_pump(void);
int display_should_quit(void);
uint16_t display_pad_buttons(void);
void display_save_bmp(const char *path);

#endif
