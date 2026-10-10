// Audio output: mixes the m4a Direct Sound buffer with an emulation of the
// four Game Boy (PSG) channels and resamples to the browser's sample rate.
//
// The PSG registers are plain memory in the port, so instead of reacting to
// register writes the emulation picks up channel (re)starts by looking at the
// trigger bits once per frame, after the m4a engine has run.

#include <emscripten.h>
#include "port_internal.h"
#include "gba/m4a_internal.h"

#define FRAMES_PER_SECOND (16777216.0 / CYCLES_PER_FRAME) // ~59.7275
#define MAX_OUT_SAMPLES 16384

extern s8 *gPortPcmChunk;
extern u32 gPortPcmChunkSize;

struct Envelope
{
    u8 volume;
    bool8 increase;
    u8 period;
    u8 timer;
};

struct PsgChannel
{
    bool8 on;
    bool8 lengthEnabled;
    u16 length;
    u16 freq;
    double phase;
    struct Envelope env;
};

static struct PsgChannel sSquare[2];
static struct PsgChannel sWave;
static struct PsgChannel sNoise;
static u16 sSweepShadow;
static u8 sSweepTimer;
static u16 sLfsr = 0x7FFF;
static u8 sWaveSamples[32];

static double sOutputRate = 48000.0;
static double sSamplesOwed;
static double sSequencerPhase; // 512 Hz frame sequencer
static u32 sSequencerStep;
static float sOutBuffer[MAX_OUT_SAMPLES * 2];
static u32 sOutCount;
static float sDcLeft, sDcRight, sPrevLeft, sPrevRight;
static float sVolume = 1.0f;

static const u8 sDutyPatterns[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};

#define NR(offset) (*(vu8 *)(REG_BASE + (offset)))

void Audio_Init(void)
{
    memset(sSquare, 0, sizeof(sSquare));
    memset(&sWave, 0, sizeof(sWave));
    memset(&sNoise, 0, sizeof(sNoise));
    sSamplesOwed = 0;
}

static void StartEnvelope(struct Envelope *env, u8 nrx2)
{
    env->volume = nrx2 >> 4;
    env->increase = (nrx2 & 8) != 0;
    env->period = nrx2 & 7;
    env->timer = env->period;
}

// Looks at the registers the m4a engine wrote this frame.
static void SyncRegisters(void)
{
    int i;
    static const u8 sSquareRegs[2][4] = {{0x62, 0x63, 0x64, 0x65}, {0x68, 0x69, 0x6C, 0x6D}};

    for (i = 0; i < 2; i++)
    {
        struct PsgChannel *ch = &sSquare[i];
        u8 nrx1 = NR(sSquareRegs[i][0]);
        u8 nrx2 = NR(sSquareRegs[i][1]);
        u8 nrx4 = NR(sSquareRegs[i][3]);

        ch->freq = NR(sSquareRegs[i][2]) | ((nrx4 & 7) << 8);
        ch->lengthEnabled = (nrx4 & 0x40) != 0;
        if (nrx4 & 0x80)
        {
            NR(sSquareRegs[i][3]) = nrx4 & 0x7F;
            ch->on = (nrx2 & 0xF8) != 0; // DAC enabled
            ch->length = 64 - (nrx1 & 0x3F);
            StartEnvelope(&ch->env, nrx2);
            if (i == 0)
            {
                u8 nr10 = NR(0x60);
                sSweepShadow = ch->freq;
                sSweepTimer = (nr10 >> 4) & 7;
            }
        }
    }

    {
        u8 nr30 = NR(0x70);
        u8 nr34 = NR(0x75);
        sWave.freq = NR(0x74) | ((nr34 & 7) << 8);
        sWave.lengthEnabled = (nr34 & 0x40) != 0;
        if (nr34 & 0x80)
        {
            const u8 *ram = (const u8 *)(REG_BASE + REG_OFFSET_WAVE_RAM0);
            NR(0x75) = nr34 & 0x7F;
            for (i = 0; i < 16; i++)
            {
                sWaveSamples[i * 2] = ram[i] >> 4;
                sWaveSamples[i * 2 + 1] = ram[i] & 0xF;
            }
            sWave.on = TRUE;
            sWave.length = 256 - NR(0x72);
            sWave.phase = 0;
        }
        if (!(nr30 & 0x80))
            sWave.on = FALSE;
    }

    {
        u8 nr42 = NR(0x79);
        u8 nr44 = NR(0x7D);
        sNoise.lengthEnabled = (nr44 & 0x40) != 0;
        if (nr44 & 0x80)
        {
            NR(0x7D) = nr44 & 0x7F;
            sNoise.on = (nr42 & 0xF8) != 0;
            sNoise.length = 64 - (NR(0x78) & 0x3F);
            StartEnvelope(&sNoise.env, nr42);
            sLfsr = 0x7FFF;
        }
    }
}

static void ClockEnvelope(struct PsgChannel *ch)
{
    struct Envelope *env = &ch->env;
    if (env->period == 0)
        return;
    if (--env->timer == 0)
    {
        env->timer = env->period;
        if (env->increase && env->volume < 15)
            env->volume++;
        else if (!env->increase && env->volume > 0)
            env->volume--;
    }
}

static void ClockLength(struct PsgChannel *ch)
{
    if (ch->lengthEnabled && ch->length > 0)
    {
        if (--ch->length == 0)
            ch->on = FALSE;
    }
}

static void ClockSweep(void)
{
    u8 nr10 = NR(0x60);
    u8 time = (nr10 >> 4) & 7;
    u8 shift = nr10 & 7;
    u32 delta, newFreq;

    if (time == 0 || !sSquare[0].on)
        return;
    if (--sSweepTimer != 0)
        return;
    sSweepTimer = time;

    delta = sSweepShadow >> shift;
    if (nr10 & 8)
        newFreq = sSweepShadow - delta;
    else
        newFreq = sSweepShadow + delta;
    if (newFreq > 2047)
    {
        sSquare[0].on = FALSE;
    }
    else if (shift != 0)
    {
        sSweepShadow = newFreq;
        sSquare[0].freq = newFreq;
        NR(0x64) = newFreq & 0xFF;
        NR(0x65) = (NR(0x65) & ~7) | (newFreq >> 8);
    }
}

// The 512 Hz frame sequencer: length at 256 Hz, sweep at 128 Hz, envelope
// at 64 Hz.
static void ClockSequencer(void)
{
    switch (sSequencerStep & 7)
    {
    case 2:
    case 6:
        ClockSweep();
        // fallthrough
    case 0:
    case 4:
        ClockLength(&sSquare[0]);
        ClockLength(&sSquare[1]);
        ClockLength(&sWave);
        ClockLength(&sNoise);
        break;
    case 7:
        ClockEnvelope(&sSquare[0]);
        ClockEnvelope(&sSquare[1]);
        ClockEnvelope(&sNoise);
        break;
    }
    sSequencerStep++;
}

// Returns the channel's output in the range -7.5..7.5.
static float SampleSquare(struct PsgChannel *ch, u8 duty)
{
    double hz = 131072.0 / (2048 - ch->freq);
    u32 step;

    ch->phase += hz / sOutputRate;
    ch->phase -= (u32)ch->phase;
    if (!ch->on)
        return 0;
    step = (u32)(ch->phase * 8) & 7;
    return sDutyPatterns[duty][step] ? ch->env.volume * 0.5f : -ch->env.volume * 0.5f;
}

static float SampleWave(void)
{
    u8 nr30 = NR(0x70);
    u8 nr32 = NR(0x73);
    double hz = 2097152.0 / (2048 - sWave.freq); // samples per second
    u32 numSamples = (nr30 & 0x20) ? 64 : 32;
    u32 index;
    float value;

    sWave.phase += hz / sOutputRate;
    while (sWave.phase >= numSamples)
        sWave.phase -= numSamples;
    if (!sWave.on)
        return 0;

    index = (u32)sWave.phase;
    // In 64 sample mode both banks play; only one bank is kept here.
    value = sWaveSamples[index & 31] - 7.5f;
    if (nr32 & 0x80)
        return value * 0.75f;
    switch ((nr32 >> 5) & 3)
    {
    case 0:
        return 0;
    case 1:
        return value;
    case 2:
        return value * 0.5f;
    default:
        return value * 0.25f;
    }
}

static float SampleNoise(void)
{
    u8 nr43 = NR(0x7C);
    double divisor = (nr43 & 7) ? (nr43 & 7) * 2.0 : 1.0;
    double hz = 524288.0 / divisor / (1 << ((nr43 >> 4) + 1)) * 2.0;
    u32 steps;
    float acc = 0;
    u32 i;

    sNoise.phase += hz / sOutputRate;
    steps = (u32)sNoise.phase;
    sNoise.phase -= steps;
    if (steps > 64)
        steps = 64;

    // Average the LFSR output over the steps taken during this sample.
    for (i = 0; i < steps; i++)
    {
        u16 bit = (sLfsr ^ (sLfsr >> 1)) & 1;
        sLfsr = (sLfsr >> 1) | (bit << 14);
        if (nr43 & 8)
            sLfsr = (sLfsr & ~0x40) | (bit << 6);
        acc += (sLfsr & 1) ? -1.0f : 1.0f;
    }
    if (!sNoise.on)
        return 0;
    if (steps == 0)
        acc = (sLfsr & 1) ? -1.0f : 1.0f;
    else
        acc /= steps;
    return acc * sNoise.env.volume * 0.5f;
}

void Audio_BeginFrame(void)
{
    sOutCount = 0;
}

void Audio_EndFrame(void)
{
    u16 soundcntH = REG_SOUNDCNT_H;
    u8 nr50 = NR(0x80);
    u8 nr51 = NR(0x81);
    bool32 masterOn = NR(0x84) & 0x80;
    float psgScale, dmaAScale, dmaBScale;
    s8 *chunk = gPortPcmChunk;
    u32 chunkSize = gPortPcmChunkSize;
    u32 n, i;

    SyncRegisters();

    sSamplesOwed += sOutputRate / FRAMES_PER_SECOND;
    n = (u32)sSamplesOwed;
    sSamplesOwed -= n;
    // Several video frames can run during one Port_Frame (see Port_BusyWait).
    if (sOutCount + n > MAX_OUT_SAMPLES)
        n = MAX_OUT_SAMPLES - sOutCount;

    switch (soundcntH & 3)
    {
    case 0:
        psgScale = 0.25f;
        break;
    case 1:
        psgScale = 0.5f;
        break;
    default:
        psgScale = 1.0f;
        break;
    }
    dmaAScale = (soundcntH & 4) ? 4.0f : 2.0f;
    dmaBScale = (soundcntH & 8) ? 4.0f : 2.0f;

    for (i = 0; i < n; i++)
    {
        float left = 0, right = 0;
        float ch[4];
        int c;

        sSequencerPhase += 512.0 / sOutputRate;
        while (sSequencerPhase >= 1.0)
        {
            sSequencerPhase -= 1.0;
            ClockSequencer();
        }

        ch[0] = SampleSquare(&sSquare[0], NR(0x62) >> 6);
        ch[1] = SampleSquare(&sSquare[1], NR(0x68) >> 6);
        ch[2] = SampleWave();
        ch[3] = SampleNoise();

        for (c = 0; c < 4; c++)
        {
            if (nr51 & (1 << c))
                right += ch[c];
            if (nr51 & (0x10 << c))
                left += ch[c];
        }
        // Channels are 4-bit (here centered: -7.5..7.5). The GBA scales the
        // PSG sum by 8 * (NR50 volume + 1) and then by the SOUNDCNT_H ratio,
        // giving up to 240 per channel on the 10-bit DAC scale where Direct
        // Sound reaches +-512.
        right *= ((nr50 & 7) + 1) * psgScale * 2.0f;
        left *= (((nr50 >> 4) & 7) + 1) * psgScale * 2.0f;

        if (chunk != NULL && chunkSize != 0)
        {
            // Linear interpolation of the Direct Sound samples.
            float pos = (float)i * chunkSize / n;
            u32 i0 = (u32)pos;
            u32 i1 = i0 + 1 < chunkSize ? i0 + 1 : i0;
            float frac = pos - i0;
            float a = chunk[i0] + (chunk[i1] - chunk[i0]) * frac;
            float b = chunk[i0 + PCM_DMA_BUF_SIZE] + (chunk[i1 + PCM_DMA_BUF_SIZE] - chunk[i0 + PCM_DMA_BUF_SIZE]) * frac;

            if (soundcntH & 0x100)
                right += a * dmaAScale;
            if (soundcntH & 0x200)
                left += a * dmaAScale;
            if (soundcntH & 0x1000)
                right += b * dmaBScale;
            if (soundcntH & 0x2000)
                left += b * dmaBScale;
        }

        if (!masterOn)
            left = right = 0;

        left /= 512.0f;
        right /= 512.0f;

        // DC blocking filter (the GBA has a coupling capacitor too).
        sDcLeft = left - sPrevLeft + 0.995f * sDcLeft;
        sPrevLeft = left;
        sDcRight = right - sPrevRight + 0.995f * sDcRight;
        sPrevRight = right;

        left = sDcLeft * sVolume;
        right = sDcRight * sVolume;
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        sOutBuffer[(sOutCount + i) * 2] = left;
        sOutBuffer[(sOutCount + i) * 2 + 1] = right;
    }
    sOutCount += n;

    gPortPcmChunk = NULL;
    gPortPcmChunkSize = 0;
}

EMSCRIPTEN_KEEPALIVE void Port_SetAudioRate(u32 rate)
{
    sOutputRate = rate;
}

EMSCRIPTEN_KEEPALIVE void Port_SetVolume(float volume)
{
    sVolume = volume;
}

// Stereo interleaved float samples produced by the last frame.
EMSCRIPTEN_KEEPALIVE float *Port_GetAudioBuffer(void)
{
    return sOutBuffer;
}

EMSCRIPTEN_KEEPALIVE u32 Port_GetAudioSampleCount(void)
{
    return sOutCount;
}
