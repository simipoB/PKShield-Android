// The output AudioUnit (AudioToolbox.h) on AAudio: hle_audio.c asks for a 48 kHz 16-bit output with a render callback, and gets an AAudio stream in low-latency game mode
// whose data callback calls it. A stream the system disconnected (headphones plugged or unplugged, a Bluetooth device switch) stops calling back; hle_audio.c's watchdog
// then stops and restarts the unit, and the start reopens the stream on the current device. Surround output (the Switch's 5.1: FL FR FC LFE BL BR) is mixed down to
// stereo here: phone speakers and headphones are stereo, and Android's low-latency path takes only mono / stereo streams.
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dm_audio_component { int dummy; };
static struct dm_audio_component g_comp;
typedef struct { AudioStreamBasicDescription f; AURenderCallbackStruct cb; AAudioStream *st; pthread_mutex_t m; int frames_logged; int16_t *mix; int32_t mixcap; } unit;

AudioComponent AudioComponentFindNext(AudioComponent c, const AudioComponentDescription *d) { (void)c; return d && d->componentType == kAudioUnitType_Output ? &g_comp : NULL; }
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *o) {
    if (c != &g_comp) { *o = NULL; return dm_at_unsupported; }
    unit *u = calloc(1, sizeof *u); pthread_mutex_init(&u->m, NULL); *o = (AudioComponentInstance)(void *)u; return noErr;
}
static void closest(unit *u) { if (u->st) { AAudioStream_requestStop(u->st); AAudioStream_close(u->st); u->st = NULL; } }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance i) { unit *u = (unit *)(void *)i; if (!u) return noErr; pthread_mutex_lock(&u->m); closest(u); pthread_mutex_unlock(&u->m); free(u->mix); free(u); return noErr; }
OSStatus AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *d, UInt32 n) {
    (void)s; (void)e; unit *u = (unit *)(void *)au;
    if (p == kAudioUnitProperty_StreamFormat && n >= sizeof u->f) { memcpy(&u->f, d, sizeof u->f); return noErr; }
    if (p == kAudioUnitProperty_SetRenderCallback && n >= sizeof u->cb) { memcpy(&u->cb, d, sizeof u->cb); return noErr; }
    return dm_at_unsupported;
}
static int down(const unit *u) { return u->f.mChannelsPerFrame > 2 && !(u->f.mFormatFlags & kLinearPCMFormatFlagIsFloat); }
static int16_t sat(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static aaudio_data_callback_result_t on_data(AAudioStream *st, void *ud, void *data, int32_t frames) {
    unit *u = ud; (void)st; AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {0};
    if (!down(u)) {
        AudioBufferList bl = { 1, { { u->f.mChannelsPerFrame, (UInt32)frames * u->f.mBytesPerFrame, data } } };
        u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    int ch = (int)u->f.mChannelsPerFrame;
    if (frames * ch > u->mixcap) { free(u->mix); u->mixcap = frames * ch; u->mix = malloc(sizeof(int16_t) * (size_t)u->mixcap); }   // grows once to the largest callback
    AudioBufferList bl = { 1, { { (UInt32)ch, (UInt32)frames * u->f.mBytesPerFrame, u->mix } } };
    u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl);
    int16_t *o = data; const int16_t *m = u->mix;
    for (int32_t i = 0; i < frames; i++, m += ch) {   // ITU downmix: centre and surrounds at -3 dB, LFE left out
        int32_t c = ch > 2 ? m[2] * 23170 >> 15 : 0, bl_ = ch > 4 ? m[4] * 23170 >> 15 : 0, br = ch > 5 ? m[5] * 23170 >> 15 : 0;
        o[2 * i] = sat(m[0] + c + bl_); o[2 * i + 1] = sat(m[1] + c + br);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *st, void *ud, aaudio_result_t err) { (void)st; (void)ud; fprintf(stderr, "[dm] audio: AAudio stream error %d (%s): the watchdog reopens it\n", err, AAudio_convertResultToText(err)); }
static int open_stream(unit *u) {
    AAudioStreamBuilder *b; if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)u->f.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, down(u) ? 2 : (int32_t)u->f.mChannelsPerFrame);
    AAudioStreamBuilder_setFormat(b, u->f.mFormatFlags & kLinearPCMFormatFlagIsFloat ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME); AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, on_data, u); AAudioStreamBuilder_setErrorCallback(b, on_error, u);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &u->st); AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[dm] [RED] audio: AAudio open failed %d (%s)\n", r, AAudio_convertResultToText(r)); u->st = NULL; return -1; }
    int32_t burst = AAudioStream_getFramesPerBurst(u->st);   // two bursts on the low-latency path; four on the normal one (bigger callbacks, more jitter: crackle)
    if (burst > 0) AAudioStream_setBufferSizeInFrames(u->st, burst * (AAudioStream_getPerformanceMode(u->st) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 2 : 4));
    if (!u->frames_logged++) fprintf(stderr, "[dm] audio: AAudio %d Hz, %d channels%s, burst %d frames, buffer %d frames, %s\n", AAudioStream_getSampleRate(u->st), AAudioStream_getChannelCount(u->st), down(u) ? " (mixed down from 5.1)" : "", burst,
                                     AAudioStream_getBufferSizeInFrames(u->st), AAudioStream_getPerformanceMode(u->st) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low latency" : "normal path");
    return 0;
}
OSStatus AudioUnitInitialize(AudioUnit au) {
    unit *u = (unit *)(void *)au; if (!u || !u->cb.inputProc || !u->f.mSampleRate) return dm_at_unsupported;
    pthread_mutex_lock(&u->m); int r = u->st ? 0 : open_stream(u); pthread_mutex_unlock(&u->m);
    return r ? dm_at_unsupported : noErr;
}
OSStatus AudioOutputUnitStart(AudioUnit au) {
    unit *u = (unit *)(void *)au; if (!u) return dm_at_unsupported;
    pthread_mutex_lock(&u->m);
    if (u->st && AAudioStream_getState(u->st) == AAUDIO_STREAM_STATE_DISCONNECTED) closest(u);   // the device went away: a fresh stream on the current one
    int r = !u->st && open_stream(u) ? -1 : AAudioStream_requestStart(u->st) == AAUDIO_OK ? 0 : -1;
    if (r && u->st) closest(u);
    pthread_mutex_unlock(&u->m);
    return r ? dm_at_unsupported : noErr;
}
OSStatus AudioOutputUnitStop(AudioUnit au) {
    unit *u = (unit *)(void *)au; if (!u) return noErr;
    pthread_mutex_lock(&u->m); if (u->st) AAudioStream_requestPause(u->st); pthread_mutex_unlock(&u->m);
    return noErr;
}
