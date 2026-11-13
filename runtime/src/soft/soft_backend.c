/* Soft backend: backend interface on top of the built-in hardware model. */
#include "soft.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void soft_memory_init(void);

void be_init(void)
{
    soft_memory_init();
    hw_init();
    gs_init();
    display_init();
}

uint32_t be_intc_pending(void) { return hw_intc_stat & hw_intc_mask; }
void be_intc_ack(uint32_t bits) { hw_intc_stat &= ~bits; }

int be_intc_set_mask(int cause, int on)
{
    uint32_t bit = 1u << (cause & 15);
    int old = (hw_intc_mask & bit) != 0;
    if (on)
        hw_intc_mask |= bit;
    else
        hw_intc_mask &= ~bit;
    ps2_event_flag = 1;
    return old;
}

uint32_t be_dmac_pending(void) { return hw_dmac_stat & (hw_dmac_stat >> 16) & 0x3FF; }
void be_dmac_ack(uint32_t bits) { hw_dmac_stat &= ~bits; }

int be_dmac_set_mask(int ch, int on)
{
    uint32_t bit = 1u << (16 + (ch & 15));
    int old = (hw_dmac_stat & bit) != 0;
    if (on)
        hw_dmac_stat |= bit;
    else
        hw_dmac_stat &= ~bit;
    ps2_event_flag = 1;
    return old;
}

int be_dmac_masked_in(int ch) { return (hw_dmac_stat >> (16 + ch)) & 1; }
void be_dmac_raise(int ch) { hw_dma_complete(ch); }
void be_update(void) { hw_update_time(); }
double be_next_event_time(void) { return hw_next_event_time(); }
void be_set_crt(int interlace, int mode, int ffmd) { gs_set_crt(interlace, mode, ffmd); }
uint32_t be_gs_imr(void) { return gs_imr(); }
void be_gs_set_imr(uint32_t v) { gs_set_imr(v); }
uint64_t be_gs_csr(void) { return gs_priv_read64(0x12001000); }
int be_should_quit(void) { return display_should_quit(); }
void be_save_frame(const char *path) { display_save_bmp(path); }
void be_shutdown(int code) { (void)code; }
int be_vu0_exec(vu0_state *st, uint32_t addr) { (void)st; (void)addr; return 0; }
int be_iop_lle(void) { return 0; }
void be_report(void) {}

int main(int argc, char **argv)
{
    int rc = ps2rt_parse_args(argc, argv);
    if (rc)
        return rc == -1 ? 0 : rc;
    ps2rt_run();
    return 0;
}
