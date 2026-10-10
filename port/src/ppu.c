// Scanline renderer for the GBA picture processing unit.
//
// Supports the features the game uses: text and affine backgrounds (modes
// 0-2, plus the bitmap modes 3-5 for completeness), regular and affine
// sprites, windows (WIN0, WIN1, OBJ window), mosaic and color special effects
// (alpha blending, brighten, darken, semi-transparent sprites).

#include "port_internal.h"

u32 gPortFramebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

#define TRANSPARENT 0x8000

// Layer ids used by the compositor.
enum
{
    LAYER_BG0,
    LAYER_BG1,
    LAYER_BG2,
    LAYER_BG3,
    LAYER_OBJ,
    LAYER_BD,
};

// Internal affine reference points (latched at the start of the frame and
// advanced every scanline).
static s32 sAffineX[2], sAffineY[2];
static u32 sLastRegX[2], sLastRegY[2];

static u16 sBgLine[4][DISPLAY_WIDTH];
static u16 sObjLine[DISPLAY_WIDTH];
static u8 sObjPrio[DISPLAY_WIDTH];
static u8 sObjSemiTrans[DISPLAY_WIDTH];
static u8 sObjWindow[DISPLAY_WIDTH];
static u8 sWindowMask[DISPLAY_WIDTH];

static u32 sColorLut[0x8000];
static bool8 sColorLutReady;

static const u8 sObjSizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

static void InitColorLut(void)
{
    u32 c;
    for (c = 0; c < 0x8000; c++)
    {
        u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        sColorLut[c] = 0xFF000000 | (b << 16) | (g << 8) | r; // RGBA in memory
    }
    sColorLutReady = TRUE;
}

static s32 SignExtend28(u32 value)
{
    return ((s32)(value << 4)) >> 4;
}

void Ppu_StartFrame(void)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        u32 x = REG32(REG_OFFSET_BG2X + i * 0x10);
        u32 y = REG32(REG_OFFSET_BG2Y + i * 0x10);
        sAffineX[i] = SignExtend28(x);
        sAffineY[i] = SignExtend28(y);
        sLastRegX[i] = x;
        sLastRegY[i] = y;
    }
}

// --------------------------------------------------------------------------
// Backgrounds

static void DrawTextBg(int bg, int line)
{
    u16 cnt = REG16(REG_OFFSET_BG0CNT + bg * 2);
    u16 hofs = REG16(REG_OFFSET_BG0HOFS + bg * 4) & 0x1FF;
    u16 vofs = REG16(REG_OFFSET_BG0VOFS + bg * 4) & 0x1FF;
    const u8 *charBase = VRAM_PTR + ((cnt >> 2) & 3) * BG_CHAR_SIZE;
    const u8 *screenBase = VRAM_PTR + ((cnt >> 8) & 0x1F) * BG_SCREEN_SIZE;
    bool32 is8bpp = cnt & BGCNT_256COLOR;
    u32 size = cnt >> 14;
    u32 widthMask = (size & 1) ? 511 : 255;
    u32 heightMask = (size & 2) ? 511 : 255;
    u16 *out = sBgLine[bg];
    u32 y, x, mosaicH = 1;
    const u16 *pltt = PLTT_PTR;

    if (cnt & BGCNT_MOSAIC)
    {
        u16 mosaic = REG_MOSAIC;
        u32 mosaicV = ((mosaic >> 4) & 0xF) + 1;
        mosaicH = (mosaic & 0xF) + 1;
        line -= line % mosaicV;
    }

    y = (line + vofs) & heightMask;

    for (x = 0; x < DISPLAY_WIDTH; x++)
    {
        u32 px = (x + hofs) & widthMask;
        u32 block = 0, entryAddr, tile, tileX, tileY;
        u16 entry;
        u32 colorIndex;

        if (px >= 256)
            block += 1;
        if (y >= 256)
            block += (size == 3) ? 2 : 1;
        entryAddr = block * 0x800 + ((y >> 3) & 31) * 64 + ((px >> 3) & 31) * 2;
        entry = *(const u16 *)(screenBase + entryAddr);
        tile = entry & 0x3FF;
        tileX = px & 7;
        tileY = y & 7;
        if (entry & 0x400)
            tileX = 7 - tileX;
        if (entry & 0x800)
            tileY = 7 - tileY;

        if (is8bpp)
        {
            u32 addr = (charBase - VRAM_PTR) + tile * 64 + tileY * 8 + tileX;
            colorIndex = addr < BG_VRAM_SIZE ? VRAM_PTR[addr] : 0;
            out[x] = colorIndex ? (pltt[colorIndex] & 0x7FFF) : TRANSPARENT;
        }
        else
        {
            u32 addr = (charBase - VRAM_PTR) + tile * 32 + tileY * 4 + (tileX >> 1);
            colorIndex = addr < BG_VRAM_SIZE ? VRAM_PTR[addr] : 0;
            colorIndex = (tileX & 1) ? (colorIndex >> 4) : (colorIndex & 0xF);
            out[x] = colorIndex ? (pltt[(entry >> 12) * 16 + colorIndex] & 0x7FFF) : TRANSPARENT;
        }
    }

    if (mosaicH > 1)
    {
        for (x = 0; x < DISPLAY_WIDTH; x++)
            out[x] = out[x - x % mosaicH];
    }
}

static void UpdateAffineRef(int idx)
{
    u32 x = REG32(REG_OFFSET_BG2X + idx * 0x10);
    u32 y = REG32(REG_OFFSET_BG2Y + idx * 0x10);
    // Writing the reference point registers mid-frame reloads the internal
    // counters on hardware.
    if (x != sLastRegX[idx])
    {
        sAffineX[idx] = SignExtend28(x);
        sLastRegX[idx] = x;
    }
    if (y != sLastRegY[idx])
    {
        sAffineY[idx] = SignExtend28(y);
        sLastRegY[idx] = y;
    }
}

static void DrawAffineBg(int bg, int line)
{
    int idx = bg - 2;
    u16 cnt = REG16(REG_OFFSET_BG0CNT + bg * 2);
    s32 pa = (s16)REG16(REG_OFFSET_BG2PA + idx * 0x10);
    s32 pc = (s16)REG16(REG_OFFSET_BG2PC + idx * 0x10);
    const u8 *charBase = VRAM_PTR + ((cnt >> 2) & 3) * BG_CHAR_SIZE;
    const u8 *screenBase = VRAM_PTR + ((cnt >> 8) & 0x1F) * BG_SCREEN_SIZE;
    u32 sizeTiles = 16 << (cnt >> 14);
    u32 sizePixels = sizeTiles * 8;
    bool32 wrap = cnt & BGCNT_WRAP;
    s32 refX = sAffineX[idx], refY = sAffineY[idx];
    u16 *out = sBgLine[bg];
    const u16 *pltt = PLTT_PTR;
    u32 x, mosaicH = 1;

    if (cnt & BGCNT_MOSAIC)
        mosaicH = (REG_MOSAIC & 0xF) + 1;

    for (x = 0; x < DISPLAY_WIDTH; x++)
    {
        s32 tx = (refX + pa * (s32)x) >> 8;
        s32 ty = (refY + pc * (s32)x) >> 8;
        u32 tile, addr;
        u8 colorIndex;

        if (wrap)
        {
            tx &= sizePixels - 1;
            ty &= sizePixels - 1;
        }
        else if (tx < 0 || ty < 0 || tx >= (s32)sizePixels || ty >= (s32)sizePixels)
        {
            out[x] = TRANSPARENT;
            continue;
        }

        tile = screenBase[(ty >> 3) * sizeTiles + (tx >> 3)];
        addr = (charBase - VRAM_PTR) + tile * 64 + (ty & 7) * 8 + (tx & 7);
        colorIndex = addr < BG_VRAM_SIZE ? VRAM_PTR[addr] : 0;
        out[x] = colorIndex ? (pltt[colorIndex] & 0x7FFF) : TRANSPARENT;
    }

    if (mosaicH > 1)
    {
        for (x = 0; x < DISPLAY_WIDTH; x++)
            out[x] = out[x - x % mosaicH];
    }
}

static void DrawBitmapBg(int mode, int line)
{
    s32 pa = (s16)REG_BG2PA, pc = (s16)REG_BG2PC;
    s32 refX = sAffineX[0], refY = sAffineY[0];
    u32 width = (mode == 5) ? 160 : DISPLAY_WIDTH;
    u32 height = (mode == 5) ? 128 : DISPLAY_HEIGHT;
    u32 frame = (REG_DISPCNT & 0x10) ? 0xA000 : 0;
    u16 *out = sBgLine[2];
    u32 x;

    for (x = 0; x < DISPLAY_WIDTH; x++)
    {
        s32 tx = (refX + pa * (s32)x) >> 8;
        s32 ty = (refY + pc * (s32)x) >> 8;
        if (tx < 0 || ty < 0 || tx >= (s32)width || ty >= (s32)height)
        {
            out[x] = TRANSPARENT;
            continue;
        }
        if (mode == 4)
        {
            u8 index = VRAM_PTR[frame + ty * width + tx];
            out[x] = index ? (PLTT_PTR[index] & 0x7FFF) : TRANSPARENT;
        }
        else
        {
            u32 base = (mode == 5) ? frame : 0;
            out[x] = *(const u16 *)(VRAM_PTR + base + (ty * width + tx) * 2) & 0x7FFF;
        }
    }
}

// --------------------------------------------------------------------------
// Sprites

static void DrawObjPixel(int x, u16 color, u8 prio, u8 mode)
{
    if (mode == ST_OAM_OBJ_WINDOW)
    {
        sObjWindow[x] = 1;
        return;
    }
    if (sObjLine[x] != TRANSPARENT && sObjPrio[x] <= prio)
        return;
    sObjLine[x] = color;
    sObjPrio[x] = prio;
    sObjSemiTrans[x] = (mode == ST_OAM_OBJ_BLEND);
}

static void DrawSprites(int line)
{
    const u16 *oam = OAM_PTR;
    bool32 mapping1D = REG_DISPCNT & DISPCNT_OBJ_1D_MAP;
    const u8 *objVram = VRAM_PTR + 0x10000;
    const u16 *objPltt = PLTT_PTR + 256;
    u16 mosaic = REG_MOSAIC;
    u32 objMosaicH = ((mosaic >> 8) & 0xF) + 1;
    u32 objMosaicV = ((mosaic >> 12) & 0xF) + 1;
    int i;

    for (i = 0; i < DISPLAY_WIDTH; i++)
    {
        sObjLine[i] = TRANSPARENT;
        sObjPrio[i] = 4;
        sObjSemiTrans[i] = 0;
        sObjWindow[i] = 0;
    }

    if (!(REG_DISPCNT & DISPCNT_OBJ_ON))
        return;

    for (i = 0; i < 128; i++)
    {
        u16 attr0 = oam[i * 4 + 0];
        u16 attr1 = oam[i * 4 + 1];
        u16 attr2 = oam[i * 4 + 2];
        u32 affineMode = (attr0 >> 8) & 3;
        u32 objMode = (attr0 >> 10) & 3;
        u32 shape = (attr0 >> 14) & 3;
        u32 size = (attr1 >> 14) & 3;
        bool32 is8bpp = attr0 & (1 << 13);
        bool32 mosaicOn = attr0 & (1 << 12);
        s32 width, height, boxWidth, boxHeight, y, x, spriteLine;
        u32 tileBase = attr2 & 0x3FF;
        u8 prio = (attr2 >> 10) & 3;
        u32 palBank = (attr2 >> 12) & 0xF;
        u32 rowStride;
        s32 sx;

        if (affineMode == ST_OAM_AFFINE_ERASE || shape == 3)
            continue;

        width = sObjSizes[shape][size][0];
        height = sObjSizes[shape][size][1];
        boxWidth = width;
        boxHeight = height;
        if (affineMode == ST_OAM_AFFINE_DOUBLE)
        {
            boxWidth *= 2;
            boxHeight *= 2;
        }

        y = attr0 & 0xFF;
        x = attr1 & 0x1FF;
        if (y + boxHeight > 256)
            y -= 256;
        if (x >= DISPLAY_WIDTH)
            x -= 512;

        spriteLine = line - y;
        if (spriteLine < 0 || spriteLine >= boxHeight)
            continue;
        if (x + boxWidth <= 0 || x >= DISPLAY_WIDTH)
            continue;

        if (mosaicOn)
            spriteLine -= spriteLine % objMosaicV;

        // Tile row stride (in 32-byte tile units).
        if (mapping1D)
            rowStride = (width / 8) * (is8bpp ? 2 : 1);
        else
            rowStride = 32;

        if (affineMode & ST_OAM_AFFINE_ON_MASK)
        {
            const u16 *params = oam + ((attr1 >> 9) & 0x1F) * 16;
            s32 pa = (s16)params[3], pb = (s16)params[7];
            s32 pc = (s16)params[11], pd = (s16)params[15];
            s32 halfW = boxWidth / 2, halfH = boxHeight / 2;
            s32 dy = spriteLine - halfH;

            for (sx = 0; sx < boxWidth; sx++)
            {
                s32 screenX = x + sx;
                s32 dx = sx - halfW;
                s32 texX, texY;
                u32 tile, colorIndex;

                if (screenX < 0 || screenX >= DISPLAY_WIDTH)
                    continue;
                texX = ((pa * dx + pb * dy) >> 8) + width / 2;
                texY = ((pc * dx + pd * dy) >> 8) + height / 2;
                if (texX < 0 || texY < 0 || texX >= width || texY >= height)
                    continue;

                if (is8bpp)
                {
                    tile = tileBase + (texY >> 3) * rowStride + (texX >> 3) * 2;
                    colorIndex = objVram[((tile * 32) & 0x7FFF) + (texY & 7) * 8 + (texX & 7)];
                    if (colorIndex)
                        DrawObjPixel(screenX, objPltt[colorIndex] & 0x7FFF, prio, objMode);
                }
                else
                {
                    tile = tileBase + (texY >> 3) * rowStride + (texX >> 3);
                    colorIndex = objVram[((tile * 32) & 0x7FFF) + (texY & 7) * 4 + ((texX & 7) >> 1)];
                    colorIndex = (texX & 1) ? (colorIndex >> 4) : (colorIndex & 0xF);
                    if (colorIndex)
                        DrawObjPixel(screenX, objPltt[palBank * 16 + colorIndex] & 0x7FFF, prio, objMode);
                }
            }
        }
        else
        {
            bool32 hflip = attr1 & (1 << 12);
            bool32 vflip = attr1 & (1 << 13);
            s32 texY = vflip ? (height - 1 - spriteLine) : spriteLine;

            for (sx = 0; sx < width; sx++)
            {
                s32 screenX = x + sx;
                s32 texX;
                u32 tile, colorIndex;

                if (screenX < 0 || screenX >= DISPLAY_WIDTH)
                    continue;
                texX = hflip ? (width - 1 - sx) : sx;

                if (is8bpp)
                {
                    tile = tileBase + (texY >> 3) * rowStride + (texX >> 3) * 2;
                    colorIndex = objVram[((tile * 32) & 0x7FFF) + (texY & 7) * 8 + (texX & 7)];
                    if (colorIndex)
                        DrawObjPixel(screenX, objPltt[colorIndex] & 0x7FFF, prio, objMode);
                }
                else
                {
                    tile = tileBase + (texY >> 3) * rowStride + (texX >> 3);
                    colorIndex = objVram[((tile * 32) & 0x7FFF) + (texY & 7) * 4 + ((texX & 7) >> 1)];
                    colorIndex = (texX & 1) ? (colorIndex >> 4) : (colorIndex & 0xF);
                    if (colorIndex)
                        DrawObjPixel(screenX, objPltt[palBank * 16 + colorIndex] & 0x7FFF, prio, objMode);
                }
            }
        }
    }

    if (objMosaicH > 1)
    {
        // Horizontal sprite mosaic. Applying it to the whole line is a
        // simplification (hardware only affects sprites with mosaic enabled).
        bool32 anyMosaic = FALSE;
        for (i = 0; i < 128; i++)
        {
            u16 attr0 = oam[i * 4];
            if ((attr0 & (1 << 12)) && ((attr0 >> 8) & 3) != ST_OAM_AFFINE_ERASE)
            {
                anyMosaic = TRUE;
                break;
            }
        }
        if (anyMosaic)
        {
            for (i = 0; i < DISPLAY_WIDTH; i++)
            {
                int src = i - i % objMosaicH;
                sObjLine[i] = sObjLine[src];
                sObjPrio[i] = sObjPrio[src];
                sObjSemiTrans[i] = sObjSemiTrans[src];
            }
        }
    }
}

// --------------------------------------------------------------------------
// Windows

static bool32 InWindowRange(u32 value, u32 start, u32 end, u32 limit)
{
    if (end > limit || start > end)
    {
        if (end > limit)
            end = limit;
        if (start > end)
            return value >= start || value < end;
    }
    return value >= start && value < end;
}

// Fills sWindowMask with the WININ/WINOUT control bits for each pixel.
static void ComputeWindows(int line)
{
    u16 dispcnt = REG_DISPCNT;
    u16 winin = REG_WININ, winout = REG_WINOUT;
    bool32 win0 = FALSE, win1 = FALSE;
    int x;

    if (!(dispcnt & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON)))
    {
        memset(sWindowMask, 0x3F, sizeof(sWindowMask));
        return;
    }

    if (dispcnt & DISPCNT_WIN0_ON)
        win0 = InWindowRange(line, REG_WIN0V >> 8, REG_WIN0V & 0xFF, DISPLAY_HEIGHT);
    if (dispcnt & DISPCNT_WIN1_ON)
        win1 = InWindowRange(line, REG_WIN1V >> 8, REG_WIN1V & 0xFF, DISPLAY_HEIGHT);

    for (x = 0; x < DISPLAY_WIDTH; x++)
    {
        if (win0 && InWindowRange(x, REG_WIN0H >> 8, REG_WIN0H & 0xFF, DISPLAY_WIDTH))
            sWindowMask[x] = winin & 0x3F;
        else if (win1 && InWindowRange(x, REG_WIN1H >> 8, REG_WIN1H & 0xFF, DISPLAY_WIDTH))
            sWindowMask[x] = (winin >> 8) & 0x3F;
        else if ((dispcnt & DISPCNT_OBJWIN_ON) && sObjWindow[x])
            sWindowMask[x] = (winout >> 8) & 0x3F;
        else
            sWindowMask[x] = winout & 0x3F;
    }
}

// --------------------------------------------------------------------------
// Compositing

static inline u16 Blend(u16 a, u16 b, u32 eva, u32 evb)
{
    u32 r = ((a & 0x1F) * eva + (b & 0x1F) * evb) >> 4;
    u32 g = (((a >> 5) & 0x1F) * eva + ((b >> 5) & 0x1F) * evb) >> 4;
    u32 bl = (((a >> 10) & 0x1F) * eva + ((b >> 10) & 0x1F) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

static inline u16 Brighten(u16 c, u32 evy)
{
    u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

static inline u16 Darken(u16 c, u32 evy)
{
    u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

void Ppu_DrawLine(int line)
{
    u16 dispcnt = REG_DISPCNT;
    u32 mode = dispcnt & 7;
    u32 *out = gPortFramebuffer + line * DISPLAY_WIDTH;
    bool8 bgOn[4] = {FALSE, FALSE, FALSE, FALSE};
    u8 bgOrder[4];
    u8 bgPrio[4];
    int numBgs = 0;
    u16 bldcnt, backdrop;
    u32 effect, eva, evb, evy;
    int i, x;

    if (!sColorLutReady)
        InitColorLut();

    UpdateAffineRef(0);
    UpdateAffineRef(1);

    if (dispcnt & DISPCNT_FORCED_BLANK)
    {
        for (x = 0; x < DISPLAY_WIDTH; x++)
            out[x] = 0xFFFFFFFF;
        goto advance;
    }

    switch (mode)
    {
    case 0:
        bgOn[0] = bgOn[1] = bgOn[2] = bgOn[3] = TRUE;
        break;
    case 1:
        bgOn[0] = bgOn[1] = bgOn[2] = TRUE;
        break;
    case 2:
        bgOn[2] = bgOn[3] = TRUE;
        break;
    default:
        bgOn[2] = TRUE;
        break;
    }

    for (i = 0; i < 4; i++)
    {
        if (!bgOn[i] || !(dispcnt & (DISPCNT_BG0_ON << i)))
        {
            bgOn[i] = FALSE;
            continue;
        }
        if (mode == 0 || (mode == 1 && i < 2))
            DrawTextBg(i, line);
        else if (mode <= 2)
            DrawAffineBg(i, line);
        else
            DrawBitmapBg(mode, line);
    }

    // Sort the enabled backgrounds by priority (ties: lower index first).
    for (i = 0; i < 4; i++)
    {
        if (bgOn[i])
        {
            int j = numBgs++;
            u8 prio = REG16(REG_OFFSET_BG0CNT + i * 2) & 3;
            while (j > 0 && bgPrio[j - 1] > prio)
            {
                bgOrder[j] = bgOrder[j - 1];
                bgPrio[j] = bgPrio[j - 1];
                j--;
            }
            bgOrder[j] = i;
            bgPrio[j] = prio;
        }
    }

    DrawSprites(line);
    ComputeWindows(line);

    bldcnt = REG_BLDCNT;
    effect = (bldcnt >> 6) & 3;
    eva = REG_BLDALPHA & 0x1F;
    evb = (REG_BLDALPHA >> 8) & 0x1F;
    evy = REG_BLDY & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    backdrop = PLTT_PTR[0] & 0x7FFF;

    for (x = 0; x < DISPLAY_WIDTH; x++)
    {
        u8 winMask = sWindowMask[x];
        u16 colors[2];
        u8 layers[2];
        int found = 0;
        u32 prio;
        int b = 0;
        bool32 objVisible = (winMask & (1 << LAYER_OBJ)) && sObjLine[x] != TRANSPARENT;
        u16 color;

        // Find the top two visible layers.
        for (prio = 0; prio < 4 && found < 2; prio++)
        {
            if (objVisible && sObjPrio[x] == prio)
            {
                colors[found] = sObjLine[x];
                layers[found] = LAYER_OBJ;
                found++;
            }
            while (b < numBgs && bgPrio[b] == prio)
            {
                int bg = bgOrder[b++];
                if (found < 2 && (winMask & (1 << bg)) && sBgLine[bg][x] != TRANSPARENT)
                {
                    colors[found] = sBgLine[bg][x];
                    layers[found] = bg;
                    found++;
                }
            }
        }
        while (found < 2)
        {
            colors[found] = backdrop;
            layers[found] = LAYER_BD;
            found++;
        }

        color = colors[0];
        if (winMask & (1 << 5))
        {
            bool32 topIsTarget1 = bldcnt & (1 << layers[0]);
            bool32 secondIsTarget2 = bldcnt & (1 << (layers[1] + 8));

            if (layers[0] == LAYER_OBJ && sObjSemiTrans[x] && secondIsTarget2)
                color = Blend(colors[0], colors[1], eva, evb);
            else if (topIsTarget1)
            {
                switch (effect)
                {
                case 1:
                    if (secondIsTarget2)
                        color = Blend(colors[0], colors[1], eva, evb);
                    break;
                case 2:
                    color = Brighten(color, evy);
                    break;
                case 3:
                    color = Darken(color, evy);
                    break;
                }
            }
        }
        out[x] = sColorLut[color];
    }

advance:
    // Advance the affine reference points to the next scanline.
    for (i = 0; i < 2; i++)
    {
        sAffineX[i] += (s16)REG16(REG_OFFSET_BG2PB + i * 0x10);
        sAffineY[i] += (s16)REG16(REG_OFFSET_BG2PD + i * 0x10);
    }
}
