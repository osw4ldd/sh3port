/*
 * EE kernel SIF services for the real-IOP mode (PCSX2 backend with a BIOS).
 *
 * Mirrors what the EE kernel does on hardware:
 *   sceSifSetDma   - builds a source-chain DMA list for channel 6 (SIF1,
 *                    EE->IOP).  Every transfer is preceded by a 4-word SIF
 *                    header consumed by the IOP's destination chain:
 *                      w0 = IOP address | 0x40000000 (IOP irq) | 0x80000000 (end)
 *                      w1 = size in words, w2/w3 = unused (EE tag slot)
 *   sceSifSetDChain- arms channel 5 (SIF0, IOP->EE) in destination chain mode.
 *   sceSifGet/SetReg - SIF mailbox registers / kernel software registers.
 * The format matches the IOP side (ps2sdk sifman) and PCSX2's SIF model.
 */
#include "../internal.h"

#define D5_CHCR 0x1000C000u
#define D5_QWC 0x1000C020u
#define D6_CHCR 0x1000C400u
#define D6_MADR 0x1000C410u
#define D6_QWC 0x1000C420u
#define D6_TADR 0x1000C430u

#define SIF_DMA_INT_I 0x02
#define SIF_DMA_INT_O 0x04

/* Kernel RAM used for the DMA list (below the interrupt stack area). */
#define SIF_LIST_BASE 0x00079000u
#define SIF_LIST_SIZE 0x2000u
#define MAX_XFERS 64

static uint32_t list_half;      /* double buffer: 0 or 1 */
static int last_id, running_id;
static uint32_t sysregs[8];

static int sif1_busy(void)
{
    return (ps2_io_read32(D6_CHCR) & 0x100) != 0;
}

static void wait_sif1(void)
{
    /* The IOP drains SIF1 on its own; keep hardware events running. */
    for (int guard = 0; sif1_busy() && guard < 5000000; guard++) {
        be_update();
        if (be_should_quit())
            kernel_exit(0);
    }
}

static void put_tag(uint32_t a, uint32_t qwc, uint32_t id, int irq, uint32_t addr)
{
    uint32_t lo = (qwc & 0xFFFF) | (id << 28) | (irq ? 0x80000000u : 0);
    ps2_write32(a + 0, lo);
    ps2_write32(a + 4, addr);
    ps2_write32(a + 8, 0);
    ps2_write32(a + 12, 0);
}

static uint32_t dma_addr(uint32_t a)
{
    /* DMAC addresses are physical; scratchpad is selected with bit 31. */
    if ((a >> 28) == 7)
        return (a & 0x3FF0) | 0x80000000u;
    return a & 0x1FFFFFF0u;
}

int sif_lle_set_dma(uint32_t transfers, int count)
{
    if (count <= 0)
        return 0;
    if (count > MAX_XFERS)
        count = MAX_XFERS;
    wait_sif1();

    uint32_t base = SIF_LIST_BASE + list_half * (SIF_LIST_SIZE / 2);
    list_half ^= 1;
    /* Layout: [headers: count qwords][tags: 2 * count qwords] */
    uint32_t hdr = base, tag = base + (uint32_t)count * 16;
    int ee_irq = 0;
    for (int i = 0; i < count; i++) {
        uint32_t t = transfers + (uint32_t)i * 16;
        uint32_t src = ps2_read32(t), dest = ps2_read32(t + 4);
        uint32_t size = ps2_read32(t + 8), attr = ps2_read32(t + 12);
        uint32_t qwc = (size + 15) / 16;
        uint32_t w0 = (dest & 0x00FFFFFFu) | ((attr & SIF_DMA_INT_O) ? 0x40000000u : 0);
        if (i == count - 1)
            w0 |= 0x80000000u;
        if (attr & SIF_DMA_INT_I)
            ee_irq = 1;
        uint32_t h = hdr + (uint32_t)i * 16;
        ps2_write32(h + 0, w0);
        ps2_write32(h + 4, qwc * 4);
        ps2_write32(h + 8, 0);
        ps2_write32(h + 12, 0);
        put_tag(tag, 1, 3 /* REF */, 0, dma_addr(h));
        tag += 16;
        int last = i == count - 1;
        put_tag(tag, qwc, last ? 0 /* REFE */ : 3 /* REF */, last && ee_irq, dma_addr(src));
        tag += 16;
    }
    ps2_io_write32(D6_QWC, 0);
    ps2_io_write32(D6_TADR, dma_addr(hdr + (uint32_t)count * 16));
    /* DIR=from memory, MOD=chain, TIE, STR */
    ps2_io_write32(D6_CHCR, 0x1u | 0x4u | 0x80u | 0x100u);
    ps2_event_flag = 1;
    running_id = ++last_id;
    if (last_id >= 0x7FFF0000)
        last_id = 1;
    return running_id;
}

int sif_lle_dma_stat(int id)
{
    return (id == running_id && sif1_busy()) ? 1 : -1;
}

void sif_lle_dchain(void)
{
    /* SIF0: DIR=to memory, MOD=chain (destination), TIE, STR */
    ps2_io_write32(D5_QWC, 0);
    ps2_io_write32(D5_CHCR, 0x4u | 0x80u | 0x100u);
}

void sif_lle_stop_dma(void)
{
    ps2_io_write32(D5_CHCR, ps2_io_read32(D5_CHCR) & ~0x100u);
}

uint32_t sif_lle_get_reg(uint32_t reg)
{
    if (reg & 0x80000000u)
        return sysregs[reg & 7];
    switch (reg) {
    case 1: return ps2_io_read32(0x1000F200);   /* MSCOM */
    case 2: return ps2_io_read32(0x1000F210);   /* SMCOM */
    case 3: return ps2_io_read32(0x1000F220);   /* MSFLG */
    case 4: return ps2_io_read32(0x1000F230);   /* SMFLG */
    default: return 0;
    }
}

void sif_lle_set_reg(uint32_t reg, uint32_t val)
{
    if (reg & 0x80000000u) {
        sysregs[reg & 7] = val;
        return;
    }
    switch (reg) {
    case 1: ps2_io_write32(0x1000F200, val); break;
    case 2: ps2_io_write32(0x1000F210, val); break;
    case 3: ps2_io_write32(0x1000F220, val); break;   /* sets bits */
    case 4: ps2_io_write32(0x1000F230, val); break;   /* clears bits */
    default: break;
    }
}
