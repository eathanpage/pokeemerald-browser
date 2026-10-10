// Entry points used by the JavaScript frontend, plus the emulated video frame
// timing (scanlines, H-Blank/V-Blank/V-Count interrupts, timers).

#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include "port_internal.h"
#include "main.h"
#include "gba/m4a_internal.h"

extern IntrFunc gIntrTable[];
void AgbMainLoop(void);

static bool8 sStarted;
static u16 sKeys;

// Order in which crt0's IntrMain checks the interrupt flags; the position is
// the index into gIntrTable.
static const u16 sIntrOrder[] = {
    INTR_FLAG_VCOUNT, INTR_FLAG_SERIAL, INTR_FLAG_TIMER3, INTR_FLAG_HBLANK,
    INTR_FLAG_VBLANK, INTR_FLAG_TIMER0, INTR_FLAG_TIMER1, INTR_FLAG_TIMER2,
    INTR_FLAG_DMA0, INTR_FLAG_DMA1, INTR_FLAG_DMA2, INTR_FLAG_DMA3,
    INTR_FLAG_KEYPAD, INTR_FLAG_GAMEPAK,
};

void Port_Halt(const char *reason)
{
    EM_ASM({ throw new Error('Game halted: ' + UTF8ToString($0)); }, reason);
}

void Port_Log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emscripten_log(EM_LOG_CONSOLE, "%s", buf);
}

static void DispatchIrqs(void)
{
    u32 i;
    u16 pending;

    if (!(REG_IME & 1))
        return;

    pending = REG_IE & REG_IF;
    if (pending == 0)
        return;

    for (i = 0; i < ARRAY_COUNT(sIntrOrder); i++)
    {
        if (pending & sIntrOrder[i])
        {
            u16 savedIe = REG_IE;
            REG_IF &= ~sIntrOrder[i];
            // Like IntrMain, block the interrupt while its handler runs.
            REG_IE = savedIe & ~sIntrOrder[i];
            if (gIntrTable[i] != NULL)
                gIntrTable[i]();
            REG_IE |= savedIe & sIntrOrder[i];
        }
    }
}

void Port_RaiseIrq(u16 flag)
{
    REG_IF |= flag;
    DispatchIrqs();
}

static void UpdateTimers(void)
{
    static const u8 sPrescalerShift[] = {0, 6, 8, 10};
    int i;

    for (i = 0; i < 4; i++)
    {
        u16 control = REG16(REG_OFFSET_TM0CNT_H + i * 4);
        if (control & TIMER_ENABLE)
        {
            // Count-up (cascade) timers are not used by the game outside of
            // link code, so treat every timer as free-running.
            REG16(REG_OFFSET_TM0CNT_L + i * 4) += CYCLES_PER_LINE >> sPrescalerShift[control & 3];
        }
    }
}

void Port_RunVideoFrame(void)
{
    int line;

    Ppu_StartFrame();

    for (line = 0; line < LINES_PER_FRAME; line++)
    {
        u16 dispstat = REG_DISPSTAT & ~(DISPSTAT_VBLANK | DISPSTAT_HBLANK | DISPSTAT_VCOUNT);

        REG_VCOUNT = line;
        if (line >= DISPLAY_HEIGHT && line < LINES_PER_FRAME - 1)
            dispstat |= DISPSTAT_VBLANK;
        if (line == (dispstat >> 8))
            dispstat |= DISPSTAT_VCOUNT;
        REG_DISPSTAT = dispstat;

        if ((dispstat & DISPSTAT_VCOUNT) && (dispstat & DISPSTAT_VCOUNT_INTR))
            Port_RaiseIrq(INTR_FLAG_VCOUNT);

        if (line == DISPLAY_HEIGHT)
        {
            Dma_OnVBlank();
            if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
                Port_RaiseIrq(INTR_FLAG_VBLANK);
        }

        if (line < DISPLAY_HEIGHT)
            Ppu_DrawLine(line);

        REG_DISPSTAT |= DISPSTAT_HBLANK;
        if (line < DISPLAY_HEIGHT)
            Dma_OnHBlank();
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
            Port_RaiseIrq(INTR_FLAG_HBLANK);
        REG_DISPSTAT &= ~DISPSTAT_HBLANK;

        UpdateTimers();
    }

    // The main loop normally resumes right after the V-Blank interrupt, so
    // report a V-Blank scanline while it runs (see SetGpuReg).
    REG_VCOUNT = DISPLAY_HEIGHT + 1;
    REG_DISPSTAT |= DISPSTAT_VBLANK;

    Audio_EndFrame();
}

// ---------------------------------------------------------------------------
// JavaScript interface

EMSCRIPTEN_KEEPALIVE void Port_SetKeys(u16 keys)
{
    sKeys = keys & KEYS_MASK;
}

EMSCRIPTEN_KEEPALIVE void Port_Frame(void)
{
    REG_KEYINPUT = ~sKeys & KEYS_MASK;

    if (!sStarted)
    {
        sStarted = TRUE;
        // State the BIOS leaves the hardware in at boot.
        RegisterRamReset(RESET_ALL);
        REG_VCOUNT = DISPLAY_HEIGHT + 1;
        Audio_Init();
        AgbMain();
    }
    AgbMainLoop();
}

EMSCRIPTEN_KEEPALIVE u32 *Port_GetFramebuffer(void)
{
    return gPortFramebuffer;
}

int main(void)
{
    // Nothing to do; the frontend drives the game with Port_Frame().
    return 0;
}

EMSCRIPTEN_KEEPALIVE void Port_DebugDump(void)
{
    Port_Log("cb1=%p cb2=%p state=%d vblank=%u intrCheck=%x vblankCb=%p",
             gMain.callback1, gMain.callback2, gMain.state, gMain.vblankCounter1, gMain.intrCheck, gMain.vblankCallback);
}
