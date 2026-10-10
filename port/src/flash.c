// 128 KiB flash save memory. The frontend persists it (see web/emerald.js)
// whenever the game writes to it.

#include <emscripten.h>
#include "port_internal.h"
#include "gba/flash_internal.h"
#include "agb_flash.h"

#define FLASH_SIZE   0x20000
#define SECTOR_BYTES 0x1000

static u8 sFlash[FLASH_SIZE];
bool8 gPortFlashDirty;

static u16 EraseFlashSector_Port(u16 sectorNum);
static u16 ProgramFlashByte_Port(u16 sectorNum, u32 offset, u8 data);

u16 (*EraseFlashSector)(u16) = EraseFlashSector_Port;
u16 (*ProgramFlashByte)(u16, u32, u8) = ProgramFlashByte_Port;

static u8 *SectorPtr(u16 sectorNum)
{
    return sFlash + (sectorNum % (FLASH_SIZE / SECTOR_BYTES)) * SECTOR_BYTES;
}

u16 IdentifyFlash(void)
{
    return 0;
}

u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void))
{
    return 0;
}

void ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    memcpy(dest, SectorPtr(sectorNum) + offset, size);
}

static u16 EraseFlashSector_Port(u16 sectorNum)
{
    memset(SectorPtr(sectorNum), 0xFF, SECTOR_BYTES);
    gPortFlashDirty = TRUE;
    return 0;
}

static u16 ProgramFlashByte_Port(u16 sectorNum, u32 offset, u8 data)
{
    SectorPtr(sectorNum)[offset] = data;
    gPortFlashDirty = TRUE;
    return 0;
}

u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src)
{
    memcpy(SectorPtr(sectorNum), src, SECTOR_BYTES);
    gPortFlashDirty = TRUE;
    return 0;
}

EMSCRIPTEN_KEEPALIVE u8 *Port_GetFlash(void)
{
    return sFlash;
}

EMSCRIPTEN_KEEPALIVE u32 Port_GetFlashSize(void)
{
    return FLASH_SIZE;
}

EMSCRIPTEN_KEEPALIVE void Port_InitFlash(bool32 hasData)
{
    if (!hasData)
        memset(sFlash, 0xFF, FLASH_SIZE);
}

// Returns TRUE (once) if the save memory changed since the last call.
EMSCRIPTEN_KEEPALIVE bool32 Port_TakeFlashDirty(void)
{
    bool32 dirty = gPortFlashDirty;
    gPortFlashDirty = FALSE;
    return dirty;
}
