// H.264 (AVC) decoding for the movie decoder (hle_movie.c, interface hle_movie_avc.h) on Android's MediaCodec (the phone's hardware decoder), the counterpart of
// hle_movie_avc.c on Apple's VideoToolbox. Pokémon Shield's one H.264 movie is the staff credits (bin/appli/credit/movie/staff.mp4).
// The SDK's demuxer hands over one access unit per packet with Annex B start codes; SPS / PPS come in Configure's csd-0 / csd-1 and may also appear in the stream: they are kept
// and given to MediaCodec as its own csd-0 / csd-1. Pictures come out of MediaCodec in presentation order through its byte-buffer output, in one of the two standard layouts
// (YUV420 semi-planar = NV12, or planar = I420) at the stride / slice height the output format names; each is copied as NV12 at its own size into a queue, and leaves it
// cropped or padded to the size the game asks for (or as RGBA, BT.601 video range, in the native texture mode). End of stream (mavc_take with flush): MediaCodec gets the
// end-of-stream flag and gives up what it still holds.
#include "hle_movie_avc.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QMAX 16
#define LOGR(...) do { if (!a->red++) fprintf(stderr, "[dm] [RED] movie: " __VA_ARGS__); } while (0)
typedef struct { uint8_t *nv12; int w, h; int64_t pts; } pic;
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AMediaCodec *mc; int eos_sent, eos_seen;
    int ow, oh, ostride, oslice, ofmt, cl, ct, cr, cb;   // the output format: size, stride, slice height, colour format, crop
    pic q[QMAX]; int nq;
    uint8_t *au; size_t aucap;
    int red, rgba;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) { fprintf(stderr, "[dm] movie: AVC decoder allocation failed\n"); abort(); } return a; }
static void close_codec(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->eos_sent = a->eos_seen = 0; }
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].nv12);
    a->nq = 0;
    if (a->mc) { AMediaCodec_flush(a->mc); a->eos_sent = a->eos_seen = 0; }
}
void mavc_free(mavc *a) { if (!a) return; mavc_drop(a); close_codec(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq + (a->mc && a->eos_sent && !a->eos_seen); }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { LOGR("AVC parameter set of %zu bytes, too long\n", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
// the NAL units of p (Annex B start codes, or else 4-byte lengths), each without its prefix (as hle_movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord (the MP4 avcC box)
    if (n < 7 || p[0] != 1) { LOGR("unrecognized avcC record (%zu bytes)\n", n); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { LOGR("AVC has no SPS / PPS, the movie shows black\n"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { LOGR("the phone has no H.264 decoder, the movie shows black\n"); return 0; }
    AMediaFormat *f = AMediaFormat_new(); uint8_t csd[2][516];
    for (int k = 0; k < 2; k++) { const uint8_t *s = k ? a->pps : a->sps; int n = k ? a->npps : a->nsps; csd[k][0] = csd[k][1] = csd[k][2] = 0; csd[k][3] = 1; memcpy(csd[k] + 4, s, (size_t)n);
        AMediaFormat_setBuffer(f, k ? "csd-1" : "csd-0", csd[k], (size_t)n + 4); }
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // an upper bound; the SPS decides
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);   // COLOR_FormatYUV420Flexible: one of the standard YUV layouts in the byte buffers
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0); AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { LOGR("MediaCodec would not start the H.264 decoder (%d), the movie shows black\n", (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    fprintf(stderr, "[dm] [movie] AVC: MediaCodec decoding, profile %d\n", a->nsps > 1 ? a->sps[1] : 0);
    a->dirty = 0; a->ofmt = 0; return 1;
}
static void read_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v;
    a->ow = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) ? v : 0; a->oh = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v) ? v : 0;
    a->ostride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) ? v : a->ow; a->oslice = AMediaFormat_getInt32(f, "slice-height", &v) ? v : a->oh;
    a->ofmt = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 0;
    if (!AMediaFormat_getRect(f, "crop", &a->cl, &a->ct, &a->cr, &a->cb)) { a->cl = a->ct = 0; a->cr = a->ow - 1; a->cb = a->oh - 1; }
    if (a->ostride < a->ow) a->ostride = a->ow; if (a->oslice < a->oh) a->oslice = a->oh;
    AMediaFormat_delete(f);
    fprintf(stderr, "[dm] [movie] AVC output %dx%d (crop %d,%d-%d,%d), stride %d, slice height %d, colour format %#x\n", a->ow, a->oh, a->cl, a->ct, a->cr, a->cb, a->ostride, a->oslice, a->ofmt);
}
// one decoded buffer → an NV12 picture at the crop's size, into the queue
static void keep_pic(mavc *a, const uint8_t *buf, size_t size, int64_t pts) {
    if (a->ofmt != 21 && a->ofmt != 19 && a->ofmt != 0x7F420888) { LOGR("AVC output colour format %#x not handled, the movie shows black\n", a->ofmt); return; }
    int w = a->cr - a->cl + 1, h = a->cb - a->ct + 1; if (w <= 0 || h <= 0) return;
    size_t ysz = (size_t)a->ostride * a->oslice;
    if (size < ysz + ysz / 2 - (size_t)a->ostride / 2) { LOGR("AVC output buffer too small (%zu for %dx%d stride %d)\n", size, a->ow, a->oh, a->ostride); return; }
    if (a->nq == QMAX) { LOGR("AVC picture queue full, dropping the oldest\n"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    uint8_t *o = malloc((size_t)w * h * 3 / 2 + w); if (!o) return;
    for (int r = 0; r < h; r++) memcpy(o + (size_t)r * w, buf + (size_t)(a->ct + r) * a->ostride + a->cl, (size_t)w);
    uint8_t *uv = o + (size_t)w * h;
    if (a->ofmt == 19) {   // planar: U plane, then V plane, each at half the stride
        const uint8_t *u = buf + ysz, *v = u + ysz / 4; int cs = a->ostride / 2;
        for (int r = 0; r < h / 2; r++) for (int x = 0; x < w / 2; x++) { uv[r * w + 2 * x] = u[(a->ct / 2 + r) * cs + a->cl / 2 + x]; uv[r * w + 2 * x + 1] = v[(a->ct / 2 + r) * cs + a->cl / 2 + x]; }
    } else for (int r = 0; r < h / 2; r++) memcpy(uv + (size_t)r * w, buf + ysz + (size_t)(a->ct / 2 + r) * a->ostride + (a->cl & ~1), (size_t)(w & ~1));
    a->q[a->nq++] = (pic){ o, w, h, pts };
}
// everything MediaCodec has ready (wait_us > 0: wait that long for the next one)
static void drain(mavc *a, int64_t wait_us) {
    while (a->mc && !a->eos_seen) {
        AMediaCodecBufferInfo info; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &info, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        if (!a->ofmt) read_format(a);
        size_t sz = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &sz);
        if (b && info.size > 0) keep_pic(a, b + info.offset, (size_t)info.size, info.presentationTimeUs);
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->eos_seen = 1;
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {   // the access unit in Annex B form, without access unit delimiters (parameter sets kept aside too)
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) keep_ps(a, nal, n);
    if (t == 9) return;
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) abort(); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc) && !open_codec(a)) return -1;
    for (int tries = 0; tries < 50; tries++) {   // an input buffer, taking pictures out meanwhile so the decoder never stalls on a full output
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 2000);
        if (i >= 0) {
            size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
            if (!b || cap < c.len) { LOGR("AVC access unit of %zu bytes does not fit MediaCodec's %zu\n", c.len, cap); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts_us, 0); return -1; }
            memcpy(b, a->au, c.len); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)pts_us, 0);
            drain(a, 0); return 0;
        }
        drain(a, 0);
    }
    LOGR("MediaCodec takes no more input (first case)\n");
    return -1;
}
// NV12 (video range) → RGBA, BT.601
static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static void nv12_rgba(const pic *s, uint8_t *dst, int w, int h) {
    for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < h && r < s->h; r++)
        for (int x = 0; x < w && x < s->w; x++) {
            int y = s->nv12[(size_t)r * s->w + x] - 16, u = s->nv12[(size_t)s->w * s->h + (size_t)(r / 2) * s->w + (x & ~1)] - 128, v = s->nv12[(size_t)s->w * s->h + (size_t)(r / 2) * s->w + (x & ~1) + 1] - 128;
            int c = 298 * y; uint8_t *o = dst + ((size_t)r * w + x) * 4;
            o[0] = clamp8((c + 409 * v + 128) >> 8); o[1] = clamp8((c - 100 * u - 208 * v + 128) >> 8); o[2] = clamp8((c + 516 * u + 128) >> 8); o[3] = 255;
        }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc) {
        if (flush && !a->eos_sent) {   // end of stream: MediaCodec gives up what it still holds
            ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
            if (i >= 0) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); a->eos_sent = 1; }
        }
        drain(a, flush && !a->nq ? 20000 : 0);
    }
    if (!a->nq) return 0;
    pic s = a->q[0]; memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    if (pts_us) *pts_us = s.pts;
    if (a->rgba) nv12_rgba(&s, dst, w, h);
    else {
        int cw = s.w < w ? s.w : w, chh = s.h < h ? s.h : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, s.nv12 + (size_t)r * s.w, (size_t)cw);
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, s.nv12 + (size_t)s.w * s.h + (size_t)r * s.w, (size_t)(cw & ~1));
    }
    free(s.nv12);
    return 1;
}
