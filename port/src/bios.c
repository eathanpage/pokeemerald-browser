// Reimplementation of the GBA BIOS calls used by the game.

#include <emscripten.h>
#include <math.h>
#include "port_internal.h"

#undef CpuSet
#undef CpuFastSet

void SoftReset(u32 resetFlags)
{
    // The frontend restarts the game when it sees this exception.
    EM_ASM({ throw 'PortSoftReset'; });
}

void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & RESET_EWRAM)
        memset((void *)EWRAM_START, 0, EWRAM_SIZE);
    if (resetFlags & RESET_PALETTE)
        memset((void *)PLTT, 0, PLTT_SIZE);
    if (resetFlags & RESET_VRAM)
        memset((void *)VRAM, 0, VRAM_SIZE);
    if (resetFlags & RESET_OAM)
        memset((void *)OAM, 0, OAM_SIZE);
    if (resetFlags & RESET_REGS)
    {
        memset((void *)REG_BASE, 0, 0x60);
        REG_DISPCNT = DISPCNT_FORCED_BLANK;
        REG_BG2PA = 0x100;
        REG_BG2PD = 0x100;
        REG_BG3PA = 0x100;
        REG_BG3PD = 0x100;
    }
}

void VBlankIntrWait(void)
{
    Port_RunVideoFrame();
}

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    u32 i;

    if (control & CPU_SET_32BIT)
    {
        const u32 *s = (const u32 *)((uintptr_t)src & ~3);
        u32 *d = (u32 *)((uintptr_t)dest & ~3);
        if (control & CPU_SET_SRC_FIXED)
        {
            u32 value = *s;
            for (i = 0; i < count; i++)
                d[i] = value;
        }
        else
        {
            for (i = 0; i < count; i++)
                d[i] = s[i];
        }
    }
    else
    {
        const u16 *s = (const u16 *)((uintptr_t)src & ~1);
        u16 *d = (u16 *)((uintptr_t)dest & ~1);
        if (control & CPU_SET_SRC_FIXED)
        {
            u16 value = *s;
            for (i = 0; i < count; i++)
                d[i] = value;
        }
        else
        {
            for (i = 0; i < count; i++)
                d[i] = s[i];
        }
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    // CpuFastSet works in blocks of 8 words.
    u32 count = ((control & 0x1FFFFF) + 7) & ~7;
    const u32 *s = (const u32 *)((uintptr_t)src & ~3);
    u32 *d = (u32 *)((uintptr_t)dest & ~3);
    u32 i;

    if (control & CPU_FAST_SET_SRC_FIXED)
    {
        u32 value = *s;
        for (i = 0; i < count; i++)
            d[i] = value;
    }
    else
    {
        for (i = 0; i < count; i++)
            d[i] = s[i];
    }
}

static void LZ77UnComp(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = dest;
    u32 size = in[1] | (in[2] << 8) | (in[3] << 16);
    u32 written = 0;

    in += 4;
    while (written < size)
    {
        u8 flags = *in++;
        int bit;
        for (bit = 0; bit < 8 && written < size; bit++, flags <<= 1)
        {
            if (flags & 0x80)
            {
                u32 length = (in[0] >> 4) + 3;
                u32 disp = (((in[0] & 0xF) << 8) | in[1]) + 1;
                in += 2;
                while (length-- && written < size)
                {
                    out[written] = out[written - disp];
                    written++;
                }
            }
            else
            {
                out[written++] = *in++;
            }
        }
    }
}

void LZ77UnCompWram(const u32 *src, void *dest)
{
    LZ77UnComp(src, dest);
}

void LZ77UnCompVram(const u32 *src, void *dest)
{
    LZ77UnComp(src, dest);
}

static void RLUnComp(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = dest;
    u32 size = in[1] | (in[2] << 8) | (in[3] << 16);
    u32 written = 0;

    in += 4;
    while (written < size)
    {
        u8 flag = *in++;
        if (flag & 0x80)
        {
            u32 length = (flag & 0x7F) + 3;
            u8 value = *in++;
            while (length-- && written < size)
                out[written++] = value;
        }
        else
        {
            u32 length = (flag & 0x7F) + 1;
            while (length-- && written < size)
                out[written++] = *in++;
        }
    }
}

void RLUnCompWram(const u32 *src, void *dest)
{
    RLUnComp(src, dest);
}

void RLUnCompVram(const u32 *src, void *dest)
{
    RLUnComp(src, dest);
}

s32 Div(s32 num, s32 denom)
{
    if (denom == 0)
        return 0;
    return num / denom;
}

u16 Sqrt(u32 num)
{
    u32 root = 0, bit = 1u << 30;

    while (bit > num)
        bit >>= 2;
    while (bit != 0)
    {
        if (num >= root + bit)
        {
            num -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static s32 ArcTan(s32 i)
{
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 x, s16 y)
{
    if (y == 0)
        return x >= 0 ? 0 : 0x8000;
    if (x == 0)
        return y >= 0 ? 0x4000 : 0xC000;

    if (y >= 0)
    {
        if (x >= 0)
        {
            if (x >= y)
                return ArcTan(((s32)y << 14) / x);
        }
        else if (-x >= y)
        {
            return ArcTan(((s32)y << 14) / x) + 0x8000;
        }
        return 0x4000 - ArcTan(((s32)x << 14) / y);
    }
    else
    {
        if (x <= 0)
        {
            if (-x > -y)
                return ArcTan(((s32)y << 14) / x) + 0x8000;
        }
        else if (x >= -y)
        {
            return ArcTan(((s32)y << 14) / x) + 0x10000;
        }
        return 0xC000 - ArcTan(((s32)x << 14) / y);
    }
}

// The BIOS affine functions use a 256-entry sine table in 2.14 fixed point.
static s16 sSineTable[256];
static bool8 sSineTableReady;

static void InitSineTable(void)
{
    int i;
    for (i = 0; i < 256; i++)
        sSineTable[i] = (s16)lround(sin(i * M_PI / 128.0) * 0x4000);
    sSineTableReady = TRUE;
}

#define SIN(theta) (sSineTable[(theta) & 0xFF])
#define COS(theta) (sSineTable[((theta) + 64) & 0xFF])

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    if (!sSineTableReady)
        InitSineTable();

    while (count-- > 0)
    {
        u8 theta = src->alpha >> 8;
        s32 sx = src->sx, sy = src->sy;
        s32 pa = (sx * COS(theta)) >> 14;
        s32 pb = -((sx * SIN(theta)) >> 14);
        s32 pc = (sy * SIN(theta)) >> 14;
        s32 pd = (sy * COS(theta)) >> 14;

        dest->pa = pa;
        dest->pb = pb;
        dest->pc = pc;
        dest->pd = pd;
        dest->dx = src->texX - (pa * src->scrX + pb * src->scrY);
        dest->dy = src->texY - (pc * src->scrX + pd * src->scrY);
        src++;
        dest++;
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    const u8 *in = (const u8 *)src;
    u8 *out = dest;

    if (!sSineTableReady)
        InitSineTable();

    while (count-- > 0)
    {
        s32 sx = *(const s16 *)(in + 0);
        s32 sy = *(const s16 *)(in + 2);
        u8 theta = *(const u16 *)(in + 4) >> 8;

        *(s16 *)(out + offset * 0) = (sx * COS(theta)) >> 14;
        *(s16 *)(out + offset * 1) = -((sx * SIN(theta)) >> 14);
        *(s16 *)(out + offset * 2) = (sy * SIN(theta)) >> 14;
        *(s16 *)(out + offset * 3) = (sy * COS(theta)) >> 14;
        // The BIOS reads the source with an 8 byte stride.
        in += 8;
        out += offset * 4;
    }
}

int MultiBoot(struct MultiBootParam *mp)
{
    return 1; // No link cable.
}
