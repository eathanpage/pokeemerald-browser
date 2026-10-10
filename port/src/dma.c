// Software DMA controller. Immediate transfers happen in Port_DmaSet; H-Blank
// and V-Blank transfers are replayed by the frame loop in port_main.c.
// Sound FIFO ("special" timing) transfers are ignored: the audio code reads
// the m4a mixing buffer directly.

#include "port_internal.h"

struct DmaChannel
{
    u32 src;
    u32 dest;
    u32 count;
    u16 control;
    u32 destStart;
};

static struct DmaChannel sDma[4];

static void Transfer(struct DmaChannel *dma)
{
    u32 unit = (dma->control & DMA_32BIT) ? 4 : 2;
    s32 srcStep, destStep;
    u32 i;

    switch (dma->control & (DMA_SRC_DEC | DMA_SRC_FIXED))
    {
    case DMA_SRC_DEC:
        srcStep = -(s32)unit;
        break;
    case DMA_SRC_FIXED:
        srcStep = 0;
        break;
    default:
        srcStep = unit;
        break;
    }
    switch (dma->control & DMA_DEST_RELOAD)
    {
    case DMA_DEST_DEC:
        destStep = -(s32)unit;
        break;
    case DMA_DEST_FIXED:
        destStep = 0;
        break;
    default:
        destStep = unit;
        break;
    }

    if (unit == 4)
    {
        u32 src = dma->src & ~3, dest = dma->dest & ~3;
        for (i = 0; i < dma->count; i++)
        {
            *(u32 *)dest = *(const u32 *)src;
            src += srcStep;
            dest += destStep;
        }
        dma->src = src;
        dma->dest = dest;
    }
    else
    {
        u32 src = dma->src & ~1, dest = dma->dest & ~1;
        for (i = 0; i < dma->count; i++)
        {
            *(u16 *)dest = *(const u16 *)src;
            src += srcStep;
            dest += destStep;
        }
        dma->src = src;
        dma->dest = dest;
    }

    if ((dma->control & DMA_DEST_RELOAD) == DMA_DEST_RELOAD)
        dma->dest = dma->destStart;
}

static void Finish(int num)
{
    struct DmaChannel *dma = &sDma[num];

    if (dma->control & DMA_INTR_ENABLE)
        Port_RaiseIrq(INTR_FLAG_DMA0 << num);
    if (!(dma->control & DMA_REPEAT) || (dma->control & DMA_START_MASK) == DMA_START_NOW)
    {
        dma->control &= ~DMA_ENABLE;
        REG16(REG_OFFSET_DMA0CNT_H + num * 12) = dma->control;
    }
}

void Port_DmaSet(int num, const void *src, void *dest, u32 control)
{
    struct DmaChannel *dma = &sDma[num & 3];
    u32 count = control & 0xFFFF;

    num &= 3;
    REG32(REG_OFFSET_DMA0SAD + num * 12) = (uintptr_t)src;
    REG32(REG_OFFSET_DMA0DAD + num * 12) = (uintptr_t)dest;
    REG32(REG_OFFSET_DMA0CNT + num * 12) = control;

    dma->src = (uintptr_t)src;
    dma->dest = (uintptr_t)dest;
    dma->destStart = dma->dest;
    dma->control = control >> 16;
    if (count == 0)
        count = (num == 3) ? 0x10000 : 0x4000;
    dma->count = count;

    if (!(dma->control & DMA_ENABLE))
        return;

    if ((dma->control & DMA_START_MASK) == DMA_START_NOW)
    {
        Transfer(dma);
        Finish(num);
    }
}

void Port_DmaStop(int num)
{
    num &= 3;
    sDma[num].control &= ~(DMA_START_MASK | DMA_DREQ_ON | DMA_REPEAT | DMA_ENABLE);
    REG16(REG_OFFSET_DMA0CNT_H + num * 12) = sDma[num].control;
}

static void RunTimed(u16 timing)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        struct DmaChannel *dma = &sDma[i];
        if ((dma->control & DMA_ENABLE) && (dma->control & DMA_START_MASK) == timing)
        {
            Transfer(dma);
            Finish(i);
        }
    }
}

void Dma_OnHBlank(void)
{
    RunTimed(DMA_START_HBLANK);
}

void Dma_OnVBlank(void)
{
    RunTimed(DMA_START_VBLANK);
}

void Dma_Reset(void)
{
    memset(sDma, 0, sizeof(sDma));
}
