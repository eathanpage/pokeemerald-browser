// C port of the assembly half of the m4a ("Sappy") sound engine
// (src/m4a_1.s): the sequencer (MPlayMain and the ply_* commands), note
// allocation (ply_note) and the Direct Sound software mixer (SoundMain).
//
// The mixer writes 8-bit samples into gSoundInfo.pcmBuffer exactly like the
// GBA version; audio.c then feeds that buffer and the emulated PSG channels
// to the browser.

#include "port_internal.h"
#include "gba/m4a_internal.h"

#define TONEDATA_TYPE_REV 0x10
#define TONEDATA_TYPE_CMP 0x20
#define SOUND_CHANNEL_SF_SPECIAL 0x20
#define WAVE_DATA_FLAG_LOOP 0xC000 // in WaveData.status

extern void *const gMPlayJumpTableTemplate[];
extern const s8 gDeltaEncodingTable[];
extern const u8 gClockTable[];
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

typedef void (*TrackFunc)(struct MusicPlayerInfo *, struct MusicPlayerTrack *);

// The GBA copies SoundMainRAM into IWRAM at startup; this is just a stand-in
// for that copy's source.
char SoundMainRAM[0x800];

// Where SoundMain mixed this frame's samples (read by audio.c).
s8 *gPortPcmChunk;
u32 gPortPcmChunkSize;

static s8 sDecodingBuffer[0x40];

u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return ((u64)multiplier * multiplicand) >> 32;
}

void SoundMainBTM(void *x)
{
    memset(x, 0, 64);
}

void RealClearChain(void *x)
{
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;

    if (track != NULL)
    {
        struct SoundChannel *next = chan->nextChannelPointer;
        struct SoundChannel *prev = chan->prevChannelPointer;

        if (prev != NULL)
            prev->nextChannelPointer = next;
        else
            track->chan = next;
        if (next != NULL)
            next->prevChannelPointer = prev;
        chan->track = NULL;
    }
}

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    int i;
    for (i = 0; i < 36; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

// --------------------------------------------------------------------------
// Direct Sound mixer

static inline void MixSample(s8 *out, s32 sample, u32 envRight, u32 envLeft)
{
    // The GBA mixer adds 8-bit values and lets them wrap around.
    out[0] = (s8)(out[0] + ((sample * (s32)envRight) >> 8));
    out[PCM_DMA_BUF_SIZE] = (s8)(out[PCM_DMA_BUF_SIZE] + ((sample * (s32)envLeft) >> 8));
}

static inline u32 *ChanXpi(struct SoundChannel *chan)
{
    return (u32 *)&chan->xpi;
}

// Decodes (with caching) one sample of DPCM-compressed wave data.
static s32 DecodeCompressedSample(struct SoundChannel *chan, u32 index)
{
    u32 block = index >> 6;

    if (block != *ChanXpi(chan))
    {
        const u8 *src = (const u8 *)chan->wav + 0x10 + block * 33;
        s8 *dest = sDecodingBuffer;
        u8 value = *src++;
        u8 byte;
        int i;

        *ChanXpi(chan) = block;
        *dest++ = value;
        byte = *src++;
        value += gDeltaEncodingTable[byte & 0xF];
        *dest++ = value;
        for (i = 0; i < 31; i++)
        {
            byte = *src++;
            value += gDeltaEncodingTable[byte >> 4];
            *dest++ = value;
            value += gDeltaEncodingTable[byte & 0xF];
            *dest++ = value;
        }
    }
    return sDecodingBuffer[index & 0x3F];
}

// Mixes a channel that plays compressed and/or reversed wave data
// (SoundMainRAM_Unk1 in the assembly). Returns FALSE if the channel ended.
static bool32 MixSpecialChannel(struct SoundChannel *chan, s8 *out, u32 numSamples, u32 divFreq,
                                u32 envRight, u32 envLeft, u32 loopStartIndex, s32 loopLen)
{
    struct WaveData *wav = chan->wav;
    u32 pos = (uintptr_t)chan->currentPointer; // pointer, or index if compressed
    s32 count = chan->count;
    u32 fw = chan->fw;
    u32 step;
    s32 s0, delta;
    u32 i;

    if (!(chan->statusFlags & SOUND_CHANNEL_SF_SPECIAL))
    {
        chan->statusFlags |= SOUND_CHANNEL_SF_SPECIAL;
        if (chan->type & TONEDATA_TYPE_REV)
        {
            pos = wav->size + (uintptr_t)wav * 2 + 0x20 - pos;
            chan->currentPointer = (s8 *)(uintptr_t)pos;
        }
        if (wav->type != 0)
        {
            pos = pos - (uintptr_t)wav - 0x10;
            chan->currentPointer = (s8 *)(uintptr_t)pos;
        }
    }

    if (chan->type & TONEDATA_TYPE_FIX)
        step = 0x800000;
    else
        step = divFreq * chan->frequency;

    if (wav->type != 0)
    {
        *ChanXpi(chan) = 0xFF000000;
        if (!(chan->type & TONEDATA_TYPE_REV))
        {
            s0 = DecodeCompressedSample(chan, pos);
            pos++;
            delta = DecodeCompressedSample(chan, pos) - s0;
            for (i = 0; i < numSamples; i++)
            {
                u32 advance;
                MixSample(out + i, s0 + (((s32)(fw * delta)) >> 23), envRight, envLeft);
                fw += step;
                advance = fw >> 23;
                if (advance == 0)
                    continue;
                fw &= 0x7FFFFF;
                count -= advance;
                if (count <= 0)
                {
                    s32 overshoot;
                    if (loopLen == 0)
                        goto stop;
                    pos = loopStartIndex;
                    overshoot = -count;
                    count += loopLen;
                    while (count <= 0)
                    {
                        overshoot -= loopLen;
                        count += loopLen;
                    }
                    pos += overshoot;
                    s0 = DecodeCompressedSample(chan, pos);
                }
                else if (advance == 1)
                {
                    s0 += delta;
                }
                else
                {
                    pos += advance - 1;
                    s0 = DecodeCompressedSample(chan, pos);
                }
                pos++;
                delta = DecodeCompressedSample(chan, pos) - s0;
            }
            pos--;
        }
        else
        {
            pos--;
            s0 = DecodeCompressedSample(chan, pos);
            pos--;
            delta = DecodeCompressedSample(chan, pos) - s0;
            for (i = 0; i < numSamples; i++)
            {
                u32 advance;
                MixSample(out + i, s0 + (((s32)(fw * delta)) >> 23), envRight, envLeft);
                fw += step;
                advance = fw >> 23;
                if (advance == 0)
                    continue;
                fw &= 0x7FFFFF;
                count -= advance;
                if (count <= 0)
                    goto stop;
                if (advance == 1)
                {
                    s0 += delta;
                }
                else
                {
                    pos -= advance - 1;
                    s0 = DecodeCompressedSample(chan, pos);
                }
                pos--;
                delta = DecodeCompressedSample(chan, pos) - s0;
            }
            pos += 2;
        }
    }
    else if (chan->type & TONEDATA_TYPE_REV)
    {
        const s8 *p = (const s8 *)(uintptr_t)pos;
        p--;
        s0 = *p;
        delta = p[-1] - s0;
        for (i = 0; i < numSamples; i++)
        {
            u32 advance;
            MixSample(out + i, s0 + (((s32)(fw * delta)) >> 23), envRight, envLeft);
            fw += step;
            advance = fw >> 23;
            if (advance == 0)
                continue;
            fw &= 0x7FFFFF;
            count -= advance;
            if (count <= 0)
                goto stop;
            p -= advance;
            s0 = *p;
            delta = p[-1] - s0;
        }
        pos = (uintptr_t)(p + 1);
    }

    chan->fw = fw;
    chan->count = count;
    chan->currentPointer = (s8 *)(uintptr_t)pos;
    return TRUE;

stop:
    chan->statusFlags = 0;
    return FALSE;
}

static void MixChannel(struct SoundInfo *soundInfo, struct SoundChannel *chan, s8 *out, u32 numSamples)
{
    struct WaveData *wav = chan->wav;
    u8 flags = chan->statusFlags;
    u32 env, envRight, envLeft;
    s32 loopLen = 0;
    const s8 *loopStart = NULL;
    u32 loopStartIndex = 0;

    if (!(flags & SOUND_CHANNEL_SF_ON))
        return;

    if (flags & SOUND_CHANNEL_SF_START)
    {
        if (flags & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return;
        }
        flags = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->statusFlags = flags;
        chan->currentPointer = wav->data + chan->count;
        chan->count = wav->size - chan->count;
        env = 0;
        chan->envelopeVolume = 0;
        chan->fw = 0;
        if (wav->status & WAVE_DATA_FLAG_LOOP)
        {
            flags |= SOUND_CHANNEL_SF_LOOP;
            chan->statusFlags = flags;
        }
        goto attack;
    }

    env = chan->envelopeVolume;
    if (flags & SOUND_CHANNEL_SF_IEC)
    {
        u8 length = chan->pseudoEchoLength;
        chan->pseudoEchoLength = length - 1;
        if (length >= 2)
            goto envelope_done;
        chan->statusFlags = 0;
        return;
    }
    if (flags & SOUND_CHANNEL_SF_STOP)
    {
        env = (env * chan->release) >> 8;
        if (env > chan->pseudoEchoVolume)
            goto envelope_done;
    pseudo_echo:
        env = chan->pseudoEchoVolume;
        if (env == 0)
        {
            chan->statusFlags = 0;
            return;
        }
        flags |= SOUND_CHANNEL_SF_IEC;
        chan->statusFlags = flags;
        goto envelope_done;
    }
    if ((flags & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY)
    {
        env = (env * chan->decay) >> 8;
        if (env > chan->sustain)
            goto envelope_done;
        env = chan->sustain;
        if (env == 0)
            goto pseudo_echo;
        flags--;
        chan->statusFlags = flags;
        goto envelope_done;
    }
    if ((flags & SOUND_CHANNEL_SF_ENV) != SOUND_CHANNEL_SF_ENV_ATTACK)
        goto envelope_done;
attack:
    env += chan->attack;
    if (env >= 0xFF)
    {
        env = 0xFF;
        flags--;
        chan->statusFlags = flags;
    }
envelope_done:
    chan->envelopeVolume = env;
    env = (env * (soundInfo->masterVolume + 1)) >> 4;
    chan->envelopeVolumeRight = (chan->rightVolume * env) >> 8;
    chan->envelopeVolumeLeft = (chan->leftVolume * env) >> 8;
    envRight = chan->envelopeVolumeRight;
    envLeft = chan->envelopeVolumeLeft;

    if (flags & SOUND_CHANNEL_SF_LOOP)
    {
        loopStart = wav->data + wav->loopStart;
        loopStartIndex = wav->loopStart;
        loopLen = wav->size - wav->loopStart;
    }

    if (chan->type & (TONEDATA_TYPE_CMP | TONEDATA_TYPE_REV))
    {
        MixSpecialChannel(chan, out, numSamples, soundInfo->divFreq, envRight, envLeft, loopStartIndex, loopLen);
        return;
    }

    if (chan->type & TONEDATA_TYPE_FIX)
    {
        // Fixed frequency: one source sample per output sample.
        const s8 *p = chan->currentPointer;
        s32 count = chan->count;
        u32 i;

        for (i = 0; i < numSamples; i++)
        {
            MixSample(out + i, *p++, envRight, envLeft);
            if (--count == 0)
            {
                if (loopLen == 0)
                {
                    chan->statusFlags = 0;
                    return;
                }
                p = loopStart;
                count = loopLen;
            }
        }
        chan->count = count;
        chan->currentPointer = (s8 *)p;
    }
    else
    {
        // Resampling with linear interpolation (23-bit fraction).
        u32 step = soundInfo->divFreq * chan->frequency;
        const s8 *p = chan->currentPointer;
        s32 count = chan->count;
        u32 fw = chan->fw;
        s32 s0 = p[0];
        s32 delta;
        u32 i;

        p++;
        delta = *p - s0;
        for (i = 0; i < numSamples; i++)
        {
            u32 advance;
            MixSample(out + i, s0 + (((s32)(fw * delta)) >> 23), envRight, envLeft);
            fw += step;
            advance = fw >> 23;
            if (advance == 0)
                continue;
            fw &= 0x7FFFFF;
            count -= advance;
            if (count <= 0)
            {
                s32 overshoot;
                if (loopLen == 0)
                {
                    chan->statusFlags = 0;
                    return;
                }
                overshoot = -count;
                count += loopLen;
                while (count <= 0)
                {
                    overshoot -= loopLen;
                    count += loopLen;
                }
                p = loopStart + overshoot;
                s0 = *p;
            }
            else if (advance == 1)
            {
                s0 += delta;
            }
            else
            {
                p += advance - 1;
                s0 = *p;
            }
            p++;
            delta = *p - s0;
        }
        chan->fw = fw;
        chan->count = count;
        chan->currentPointer = (s8 *)(p - 1);
    }
}

void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    u32 samples;
    s8 *chunk;
    u32 counter;
    s32 i;

    if (soundInfo->ident != ID_NUMBER)
        return;
    soundInfo->ident++;

    if (soundInfo->MPlayMainHead != NULL)
        soundInfo->MPlayMainHead(soundInfo->musicPlayerHead);
    soundInfo->CgbSound();

    samples = soundInfo->pcmSamplesPerVBlank;
    chunk = soundInfo->pcmBuffer;
    counter = soundInfo->pcmDmaCounter;
    if (counter >= 2)
        chunk += samples * (soundInfo->pcmDmaPeriod - (counter - 1));

    if (soundInfo->reverb != 0)
    {
        const s8 *prev = (counter == 2) ? soundInfo->pcmBuffer : chunk + samples;
        u32 j;
        for (j = 0; j < samples; j++)
        {
            s32 sum = chunk[j + PCM_DMA_BUF_SIZE] + chunk[j] + prev[j + PCM_DMA_BUF_SIZE] + prev[j];
            s32 value = (sum * soundInfo->reverb) >> 9;
            if (value & 0x80)
                value++;
            chunk[j + PCM_DMA_BUF_SIZE] = value;
            chunk[j] = value;
        }
    }
    else
    {
        memset(chunk, 0, samples);
        memset(chunk + PCM_DMA_BUF_SIZE, 0, samples);
    }

    for (i = 0; i < soundInfo->maxChans; i++)
        MixChannel(soundInfo, &soundInfo->chans[i], chunk, samples);

    gPortPcmChunk = chunk;
    gPortPcmChunkSize = samples;
    soundInfo->ident = ID_NUMBER;
}

void m4aSoundVSync(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    if (soundInfo->ident - ID_NUMBER > 1)
        return;

    {
        s32 counter = soundInfo->pcmDmaCounter - 1;
        soundInfo->pcmDmaCounter = counter;
        if (counter > 0)
            return;
    }
    soundInfo->pcmDmaCounter = soundInfo->pcmDmaPeriod;
}

// --------------------------------------------------------------------------
// Sequencer

static u8 ReadCmdByte(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

static void ChnVolSet(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    s32 rhythmPan = (s8)chan->rhythmPan;
    u32 right = (((0x80 + rhythmPan) * chan->velocity) * track->volMR) >> 14;
    u32 left = (((0x7F - rhythmPan) * chan->velocity) * track->volML) >> 14;

    chan->rightVolume = right > 0xFF ? 0xFF : right;
    chan->leftVolume = left > 0xFF ? 0xFF : left;
}

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    const u8 *p = track->cmdPtr;
    track->cmdPtr = (u8 *)(uintptr_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel < 3)
    {
        track->patternStack[track->patternLevel] = track->cmdPtr + 4;
        track->patternLevel++;
        ply_goto(mplayInfo, track);
    }
    else
    {
        ply_fine(mplayInfo, track);
    }
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel != 0)
    {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 *start = track->cmdPtr;

    if (*start == 0)
    {
        track->cmdPtr = start + 1;
        ply_goto(mplayInfo, track);
        return;
    }

    track->repN++;
    track->cmdPtr = start + 1;
    if (track->repN < *start)
    {
        ply_goto(mplayInfo, track);
    }
    else
    {
        track->repN = 0;
        track->cmdPtr = start + 5;
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->priority = ReadCmdByte(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u16 tempo = ReadCmdByte(track) * 2;
    mplayInfo->tempoD = tempo;
    mplayInfo->tempoI = (tempo * mplayInfo->tempoU) >> 8;
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->keyShift = ReadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 index = ReadCmdByte(track);
    track->tone = mplayInfo->tone[index];
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->vol = ReadCmdByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->pan = ReadCmdByte(track) - C_V;
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bend = ReadCmdByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bendRange = ReadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoDelay = ReadCmdByte(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 modT = ReadCmdByte(track);
    if (track->modT != modT)
    {
        track->modT = modT;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->tune = ReadCmdByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 reg = ReadCmdByte(track);
    u8 value = ReadCmdByte(track);
    *(vu8 *)(REG_ADDR_SOUND1CNT_L + reg) = value;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoSpeed = ReadCmdByte(track);
    if (track->lfoSpeed == 0)
        ClearModM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->mod = ReadCmdByte(track);
    if (track->mod == 0)
        ClearModM(track);
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;
    u8 key;

    if (*track->cmdPtr < 0x80)
    {
        key = *track->cmdPtr++;
        track->key = key;
    }
    else
    {
        key = track->key;
    }

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        u8 flags = chan->statusFlags;
        if ((flags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV))
         && !(flags & SOUND_CHANNEL_SF_STOP)
         && chan->midiKey == key)
        {
            chan->statusFlags = flags | SOUND_CHANNEL_SF_STOP;
            return;
        }
    }
}

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;

    if (!(track->flags & MPT_FLG_EXIST))
        return;

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags != 0)
        {
            u8 cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (cgbType)
                SOUND_INFO_PTR->CgbOscOff(cgbType);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

void ply_note(u32 noteCmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct ToneData *tone;
    struct SoundChannel *chan;
    u8 toneType;
    u32 key, priority, cgbType;
    s32 rhythmPan = 0;
    s32 shiftedKey;
    u32 freq;
    const u8 *p;

    track->gateTime = gClockTable[noteCmd];
    p = track->cmdPtr;
    if (*p < 0x80)
    {
        track->key = *p++;
        if (*p < 0x80)
        {
            track->velocity = *p++;
            if (*p < 0x80)
                track->gateTime += *p++;
        }
        track->cmdPtr = (u8 *)p;
    }

    toneType = track->tone.type;
    if (toneType & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        u32 index = track->key;
        if (toneType & TONEDATA_TYPE_SPL)
        {
            const u8 *keySplitTable = (const u8 *)(uintptr_t)*(u32 *)&track->tone.attack;
            index = keySplitTable[track->key];
        }
        tone = &((struct ToneData *)track->tone.wav)[index];
        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
            return;
        if (toneType & TONEDATA_TYPE_RHY)
        {
            if (tone->pan_sweep & 0x80)
                rhythmPan = (s32)(tone->pan_sweep - TONEDATA_P_S_PAN) << 1;
            key = tone->key;
        }
        else
        {
            key = track->key;
        }
    }
    else
    {
        tone = &track->tone;
        key = track->key;
    }

    priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    cgbType = tone->type & TONEDATA_TYPE_CGB;
    if (cgbType)
    {
        if (soundInfo->cgbChans == NULL)
            return;
        chan = (struct SoundChannel *)&soundInfo->cgbChans[cgbType - 1];
        if ((chan->statusFlags & SOUND_CHANNEL_SF_ON) && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP))
        {
            if (chan->priority > priority)
                return;
            if (chan->priority == priority && (uintptr_t)chan->track < (uintptr_t)track)
                return;
        }
    }
    else
    {
        u32 bestPriority = priority;
        struct MusicPlayerTrack *bestTrack = track;
        bool32 foundStopping = FALSE;
        struct SoundChannel *best = NULL;
        s32 i;

        chan = soundInfo->chans;
        for (i = 0; i < soundInfo->maxChans; i++, chan++)
        {
            u8 flags = chan->statusFlags;
            if (!(flags & SOUND_CHANNEL_SF_ON))
            {
                best = chan;
                break;
            }
            if (flags & SOUND_CHANNEL_SF_STOP)
            {
                if (!foundStopping)
                {
                    foundStopping = TRUE;
                    bestPriority = chan->priority;
                    bestTrack = chan->track;
                    best = chan;
                    continue;
                }
            }
            else if (foundStopping)
            {
                continue;
            }

            if (chan->priority < bestPriority)
            {
                bestPriority = chan->priority;
                bestTrack = chan->track;
                best = chan;
            }
            else if (chan->priority == bestPriority)
            {
                if ((uintptr_t)chan->track > (uintptr_t)bestTrack)
                {
                    bestTrack = chan->track;
                    best = chan;
                }
                else if (chan->track == bestTrack)
                {
                    best = chan;
                }
            }
        }
        if (best == NULL)
            return;
        chan = best;
    }

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModM(track);
    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = key;
    chan->rhythmPan = rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    ChnVolSet(chan, track);

    shiftedKey = (s32)chan->key + (s8)track->keyM;
    if (shiftedKey < 0)
        shiftedKey = 0;

    if (cgbType)
    {
        struct CgbChannel *cgb = (struct CgbChannel *)chan;
        u8 sweep = tone->pan_sweep;
        cgb->length = tone->length;
        if ((sweep & 0x80) || !(sweep & 0x70))
            sweep = 8;
        cgb->sweep = sweep;
        freq = soundInfo->MidiKeyToCgbFreq(cgbType, shiftedKey, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        freq = MidiKeyToFreq(chan->wav, shiftedKey, track->pitM);
    }
    chan->frequency = freq;
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    struct SoundInfo *soundInfo;
    u32 tempoC;
    s32 i;
    struct MusicPlayerTrack *track;

    if (mplayInfo->ident != ID_NUMBER)
        return;
    mplayInfo->ident++;

    if (mplayInfo->MPlayMainNext != NULL)
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    if (mplayInfo->status & MUSICPLAYER_STATUS_PAUSE)
        goto end;

    soundInfo = SOUND_INFO_PTR;
    FadeOutBody(mplayInfo);
    if (mplayInfo->status & MUSICPLAYER_STATUS_PAUSE)
        goto end;

    tempoC = mplayInfo->tempoC + mplayInfo->tempoI;
    while (TRUE)
    {
        u32 bit, activeTracks;

        mplayInfo->tempoC = tempoC;
        if (tempoC < 150)
            break;

        activeTracks = 0;
        track = mplayInfo->tracks;
        for (i = 0, bit = 1; i < mplayInfo->trackCount; i++, track++, bit <<= 1)
        {
            struct SoundChannel *chan;

            if (!(track->flags & MPT_FLG_EXIST))
                continue;
            activeTracks |= bit;

            for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
            {
                if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
                {
                    if (chan->gateTime != 0 && --chan->gateTime == 0)
                        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                }
                else
                {
                    ClearChain(chan);
                }
            }

            if (track->flags & MPT_FLG_START)
            {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 64;
                track->lfoSpeed = 22;
                track->tone.type = 1;
            }

            while (track->wait == 0)
            {
                u8 cmd = *track->cmdPtr;
                if (cmd < 0x80)
                {
                    cmd = track->runningStatus;
                }
                else
                {
                    track->cmdPtr++;
                    if (cmd >= 0xBD)
                        track->runningStatus = cmd;
                }

                if (cmd >= 0xCF)
                {
                    soundInfo->plynote(cmd - 0xCF, mplayInfo, track);
                }
                else if (cmd > 0xB0)
                {
                    mplayInfo->cmd = cmd - 0xB1;
                    ((TrackFunc)soundInfo->MPlayJumpTable[cmd - 0xB1])(mplayInfo, track);
                    if (track->flags == 0)
                        goto next_track;
                }
                else
                {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }

            track->wait--;
            if (track->lfoSpeed != 0 && track->mod != 0)
            {
                if (track->lfoDelayC != 0)
                {
                    track->lfoDelayC--;
                }
                else
                {
                    u8 speedC = track->lfoSpeedC + track->lfoSpeed;
                    s32 value;

                    track->lfoSpeedC = speedC;
                    if ((u8)(speedC - 0x40) < 0x80)
                        value = 0x80 - speedC;
                    else
                        value = (s8)speedC;
                    value = (track->mod * value) >> 6;
                    if ((s8)(track->modM ^ value) != 0)
                    {
                        track->modM = value;
                        track->flags |= (track->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                    }
                }
            }
        next_track:;
        }

        mplayInfo->clock++;
        if (activeTracks == 0)
        {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto end;
        }
        mplayInfo->status = activeTracks;
        tempoC -= 150;
    }

    track = mplayInfo->tracks;
    for (i = 0; i < mplayInfo->trackCount; i++, track++)
    {
        struct SoundChannel *chan;

        if (!(track->flags & MPT_FLG_EXIST) || !(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            continue;

        TrkVolPitSet(mplayInfo, track);
        for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
        {
            u8 cgbType;

            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                ClearChain(chan);
                continue;
            }

            cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (track->flags & MPT_FLG_VOLCHG)
            {
                ChnVolSet(chan, track);
                if (cgbType)
                    ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
            }
            if (track->flags & MPT_FLG_PITCHG)
            {
                s32 key = (s32)chan->key + (s8)track->keyM;
                if (key < 0)
                    key = 0;
                if (cgbType)
                {
                    struct CgbChannel *cgb = (struct CgbChannel *)chan;
                    cgb->frequency = soundInfo->MidiKeyToCgbFreq(cgbType, key, track->pitM);
                    cgb->modify |= CGB_CHANNEL_MO_PIT;
                }
                else
                {
                    chan->frequency = MidiKeyToFreq(chan->wav, key, track->pitM);
                }
            }
        }
        track->flags &= 0xF0;
    }

end:
    mplayInfo->ident = ID_NUMBER;
}
