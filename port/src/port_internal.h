#ifndef GUARD_PORT_INTERNAL_H
#define GUARD_PORT_INTERNAL_H

#include "global.h"

#define REG16(offset) (*(vu16 *)(REG_BASE + (offset)))
#define REG32(offset) (*(vu32 *)(REG_BASE + (offset)))

#define VRAM_PTR ((u8 *)VRAM)
#define PLTT_PTR ((u16 *)PLTT)
#define OAM_PTR  ((u16 *)OAM)

#define CYCLES_PER_LINE  1232
#define LINES_PER_FRAME  228
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)

// port_main.c
void Port_RaiseIrq(u16 flag);

// ppu.c
extern u32 gPortFramebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
void Ppu_StartFrame(void);
void Ppu_DrawLine(int line);

// dma.c
void Dma_OnHBlank(void);
void Dma_OnVBlank(void);
void Dma_Reset(void);

// flash.c
extern bool8 gPortFlashDirty;

// audio.c
void Audio_Init(void);
void Audio_BeginFrame(void);
void Audio_EndFrame(void);

#endif // GUARD_PORT_INTERNAL_H
