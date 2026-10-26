// AudioToolbox's AudioConverter on Android, for what hle_movie.c asks of it: AAC packets (with the MPEG-4 ES descriptor as the magic cookie) into interleaved 16-bit PCM,
// decoded by MediaCodec ("audio/mp4a-latm", the phone's AAC decoder). FillComplexBuffer pulls packets through the caller's input procedure, queues them and returns the PCM
// MediaCodec has ready; what does not fit the caller's buffer waits for the next call. Other conversions are refused as before (the caller outputs silence).
#include "AudioToolbox/AudioToolbox.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dm_audio_converter {
    AMediaCodec *mc; int rate, ch, started, red; uint8_t asc[64]; int an;
    uint8_t *left; size_t nleft, capleft; uint64_t pts;
};

OSStatus AudioConverterNew(const AudioStreamBasicDescription *i, const AudioStreamBasicDescription *o, AudioConverterRef *c) {
    *c = NULL;
    if (i->mFormatID != kAudioFormatMPEG4AAC || o->mFormatID != kAudioFormatLinearPCM || o->mBitsPerChannel != 16) return dm_at_unsupported;
    struct dm_audio_converter *a = calloc(1, sizeof *a); if (!a) return dm_at_unsupported;
    a->rate = (int)i->mSampleRate; a->ch = (int)i->mChannelsPerFrame;
    *c = a; return noErr;
}
// the MPEG-4 ES descriptor (tag 3) → its decoder configuration (4) → the decoder specific info (5): the AudioSpecificConfig
static size_t descr_len(const uint8_t *p, size_t n, size_t *i) { size_t v = 0; for (int k = 0; k < 4 && *i < n; k++) { uint8_t b = p[(*i)++]; v = v << 7 | (b & 0x7f); if (!(b & 0x80)) break; } return v; }
static int find_asc(const uint8_t *p, size_t n, uint8_t *out, int cap) {
    size_t i = 0;
    while (i + 2 <= n) {
        int tag = p[i++]; size_t len = descr_len(p, n, &i);
        if (tag == 3) { i += 3; continue; }        // ES ID and flags, then the nested descriptors
        if (tag == 4) { i += 13; continue; }       // object type, stream type, buffer size, bit rates, then the nested descriptor
        if (tag == 5) { if (len > (size_t)cap || i + len > n) return 0; memcpy(out, p + i, len); return (int)len; }
        i += len;
    }
    return 0;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID p, UInt32 n, const void *d) {
    if (!c || p != kAudioConverterDecompressionMagicCookie) return dm_at_unsupported;
    c->an = find_asc(d, n, c->asc, sizeof c->asc);
    return c->an ? noErr : dm_at_unsupported;
}
static int start(AudioConverterRef a) {
    if (a->started) return a->mc != NULL;
    a->started = 1;
    a->mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!a->mc) { fprintf(stderr, "[dm] [RED] movie: the phone has no AAC decoder, the movie is silent\n"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, a->rate); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, a->ch);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    if (a->an) AMediaFormat_setBuffer(f, "csd-0", a->asc, (size_t)a->an);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0); AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { fprintf(stderr, "[dm] [RED] movie: MediaCodec would not start the AAC decoder (%d), the movie is silent\n", (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    fprintf(stderr, "[dm] [movie] AAC: MediaCodec decoding, %d channels %d Hz\n", a->ch, a->rate);
    return 1;
}
static void keep(AudioConverterRef a, const uint8_t *p, size_t n) {
    if (a->nleft + n > a->capleft) { a->capleft = (a->nleft + n) * 2; a->left = realloc(a->left, a->capleft); if (!a->left) { a->nleft = a->capleft = 0; return; } }
    memcpy(a->left + a->nleft, p, n); a->nleft += n;
}
static void drain(AudioConverterRef a, int64_t wait_us) {
    for (;;) {
        AMediaCodecBufferInfo info; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &info, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        size_t sz = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &sz);
        if (b && info.size > 0) keep(a, b + info.offset, (size_t)info.size);
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        wait_us = 0;
    }
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef a, AudioConverterComplexInputDataProc f, void *u, UInt32 *n, AudioBufferList *o, AudioStreamPacketDescription *d) {
    (void)d;
    size_t want = (size_t)*n * 2 * (size_t)a->ch; *n = 0;
    if (!start(a)) return dm_at_unsupported;
    OSStatus st = noErr;
    while (a->nleft < want) {   // packets from the caller until it has none left
        UInt32 np = 1; AudioBufferList bl = {1, {{(UInt32)a->ch, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        st = f(a, &np, &bl, &pd, u);
        if (st || !np || !bl.mBuffers[0].mData) break;
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
        if (i < 0) { if (!a->red++) fprintf(stderr, "[dm] [RED] movie: the AAC decoder takes no input (first case)\n"); break; }
        size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap); size_t len = bl.mBuffers[0].mDataByteSize < cap ? bl.mBuffers[0].mDataByteSize : cap;
        if (b) memcpy(b, bl.mBuffers[0].mData, len);
        AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, len, a->pts, 0); a->pts += 1000000ull * 1024 / (uint64_t)(a->rate > 0 ? a->rate : 48000);
        drain(a, 3000);
    }
    drain(a, 0);
    size_t give = a->nleft < want ? a->nleft : want; give -= give % (2 * (size_t)a->ch);
    if (give) { memcpy(o->mBuffers[0].mData, a->left, give); memmove(a->left, a->left + give, a->nleft - give); a->nleft -= give; }
    o->mBuffers[0].mDataByteSize = (UInt32)give; *n = (UInt32)(give / (2 * (size_t)a->ch));
    return st;
}
OSStatus AudioConverterReset(AudioConverterRef a) { if (a && a->mc) AMediaCodec_flush(a->mc); if (a) a->nleft = 0; return noErr; }
OSStatus AudioConverterDispose(AudioConverterRef a) {
    if (!a) return noErr;
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); }
    free(a->left); free(a); return noErr;
}
