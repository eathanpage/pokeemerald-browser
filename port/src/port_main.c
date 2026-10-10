// Entry points used by the JavaScript frontend, plus the emulated video frame
// timing (scanlines, H-Blank/V-Blank/V-Count interrupts, timers).

#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include "port_internal.h"
#include "main.h"
#include "gba/m4a_internal.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "script.h"
#include "sound.h"
#include "task.h"
#include "m4a.h"
#include "gba/m4a_internal.h"
#include "palette.h"
#include "battle_setup.h"
#include "script_pokemon_util.h"
#include "constants/species.h"
#include "constants/items.h"

extern IntrFunc gIntrTable[];
void AgbMainLoop(void);

static bool8 sStarted;
static u16 sKeys;
static u32 sBusyPolls;

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

void Port_BusyWait(void)
{
    if (++sBusyPolls >= 64)
        Port_RunVideoFrame();
}

void Port_RunVideoFrame(void)
{
    int line;

    sBusyPolls = 0;
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
    Audio_BeginFrame();

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

static void Port_LogSePlayer(struct MusicPlayerInfo *mp, int song)
{
    int t;
    Port_Log("  SE1 song=%d tracks=%d tempoD=%d tempoI=%d clock=%u", song, mp->trackCount, mp->tempoD, mp->tempoI, mp->clock);
    for (t = 0; t < mp->trackCount; t++)
    {
        struct MusicPlayerTrack *tr = &mp->tracks[t];
        Port_Log("    track %d flags=%x wait=%d cmdPtr=%p [%02x %02x %02x %02x] chan=%p", t, tr->flags, tr->wait, tr->cmdPtr,
                 tr->cmdPtr ? tr->cmdPtr[0] : 0, tr->cmdPtr ? tr->cmdPtr[1] : 0, tr->cmdPtr ? tr->cmdPtr[2] : 0, tr->cmdPtr ? tr->cmdPtr[3] : 0, tr->chan);
    }
}

// Debugging aid for the test tools.
EMSCRIPTEN_KEEPALIVE void Port_DebugDump(void)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    Port_Log("cb1=%p cb2=%p state=%d vblank=%u map=%d.%d player=%d,%d facing=%d",
             gMain.callback1, gMain.callback2, gMain.state, gMain.vblankCounter1,
             gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum,
             player->currentCoords.x - MAP_OFFSET, player->currentCoords.y - MAP_OFFSET,
             player->facingDirection);
    Port_Log("  player: actionId=%d heldActive=%d heldFinished=%d sprite=%d actionFuncId=%d data3..7=%d %d %d %d %d singleMovementActive=%d",
             player->movementActionId, player->heldMovementActive, player->heldMovementFinished, player->spriteId,
             gSprites[player->spriteId].data[2], gSprites[player->spriteId].data[3], gSprites[player->spriteId].data[4],
             gSprites[player->spriteId].data[5], gSprites[player->spriteId].data[6], gSprites[player->spriteId].data[7],
             player->singleMovementActive);
    Port_Log("  fieldLocked=%d scriptEnabled=%d se=%d bgm=%x se1=%x se2=%x heldMovement=%d fade=%d tasks:",
             ArePlayerFieldControlsLocked(), ScriptContext_IsEnabled(), IsSEPlaying(),
             gMPlayInfo_BGM.status, gMPlayInfo_SE1.status, gMPlayInfo_SE2.status,
             player->heldMovementActive, gPaletteFade.active);
    {
        int i;
        struct MusicPlayerInfo *se = &gMPlayInfo_SE1;
        for (i = 0; i < 1000; i++)
            if (gSongTable[i].header == se->songHeader)
                break;
        Port_LogSePlayer(se, i);
        for (i = 0; i < NUM_TASKS; i++)
            if (gTasks[i].isActive)
                Port_Log("    task %d func=%p data0=%d", i, gTasks[i].func, gTasks[i].data[0]);
    }
}

// Test hook: starts a wild battle from the overworld (gives the player a
// Pokemon first if the party is empty).
EMSCRIPTEN_KEEPALIVE void Port_TestWildBattle(u16 species, u8 level)
{
    if (gPlayerPartyCount == 0)
        ScriptGiveMon(SPECIES_MUDKIP, 7, ITEM_NONE, 0, 0, 0);
    CreateScriptedWildMon(species, level, ITEM_NONE);
    BattleSetup_StartScriptedWildBattle();
}
