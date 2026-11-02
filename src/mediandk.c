// mediandk.c — Android's libmediandk (AMediaExtractor / AMediaCodec / AMediaFormat /
// AMediaDataSource) for Unity's VideoPlayer, step 1: a video ends at once. The mp4 is read for
// real (its tracks, codec, size, length), and the decoder answers "end of stream" to the
// first frame it is asked for: the game hears the video finished and goes on (Shadow Fight 4
// waited on a black screen for its intro, "VideoPlayer cannot play clip", until the app was
// paused and resumed).
// Step 2, for the games in VIDEO_GAMES (Shadow Fight 4 first): real playing through FFmpeg
// (libavformat reads the file, libavcodec decodes; the VLC port's build). Unity asks for no
// surface: the frames go out as buffers - video NV12 (color format 21), audio 16-bit PCM - and
// Unity draws / plays them itself. Every other game still has its videos end at once.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "game_settings.h"
#include "native_activity.h"
#include "video_tex.h"

void droid_log(const char *fmt, ...);
void *arm_callback(uint64_t guest);

// Real decoding: VIDEO_GAMES (video_tex.h: decoded into a SurfaceTexture, its GL hooks too), and the
// games that take the decoded pictures and sound from the codec's own buffers (no GL hooks): the
// Infinity Blade ports' startup movies (Startup.m4v, StartupLoop.m4v: AVC + AAC) - they "ended at
// once", the screen stayed black for the load they cover on a phone, the loop restarted endlessly
#define DECODE_GAMES "com.ib3port.,com.ib2port.,com.ib1port."
static int video_real(void) { return video_game() || droid_game_in(DECODE_GAMES); }

#define AMEDIA_OK 0
#define AMEDIA_ERROR_UNKNOWN (-10000)
#define AMEDIACODEC_INFO_TRY_AGAIN_LATER (-1)
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM 4

// ---- AMediaFormat: a few named values -----------------------------------------------------
#define FMT_KEYS 24
typedef struct {
    struct {
        char key[48];
        int kind; // 'i' int32, 'l' int64, 'f' float, 's' string
        int64_t i;
        float f;
        char s[64];
    } v[FMT_KEYS];
    int n;
} media_format;

static media_format *fmt_new(void) { return calloc(1, sizeof(media_format)); }
static void *fmt_slot(media_format *m, const char *key, int kind) {
    for (int k = 0; k < m->n; k++)
        if (!strcmp(m->v[k].key, key)) {
            m->v[k].kind = kind;
            return &m->v[k];
        }
    if (m->n == FMT_KEYS) return NULL;
    snprintf(m->v[m->n].key, sizeof(m->v[m->n].key), "%s", key);
    m->v[m->n].kind = kind;
    return &m->v[m->n++];
}
static void fmt_int(media_format *m, const char *key, int64_t v, int kind) {
    typeof(&m->v[0]) s = fmt_slot(m, key, kind);
    if (s) s->i = v;
}
static void fmt_str(media_format *m, const char *key, const char *v) {
    typeof(&m->v[0]) s = fmt_slot(m, key, 's');
    if (s) snprintf(s->s, sizeof(s->s), "%s", v);
}
static int fmt_find(const media_format *m, const char *key) {
    for (int k = 0; m && k < m->n; k++)
        if (!strcmp(m->v[k].key, key)) return k;
    return -1;
}

static media_format *AMediaFormat_new(void) { return fmt_new(); }
static int AMediaFormat_delete(media_format *m) { free(m); return AMEDIA_OK; }
static int AMediaFormat_getInt32(media_format *m, const char *key, int32_t *out) {
    int k = fmt_find(m, key);
    if (k < 0 || (m->v[k].kind != 'i' && m->v[k].kind != 'l')) return 0;
    *out = (int32_t)m->v[k].i;
    return 1;
}
static int AMediaFormat_getInt64(media_format *m, const char *key, int64_t *out) {
    int k = fmt_find(m, key);
    if (k < 0 || (m->v[k].kind != 'i' && m->v[k].kind != 'l')) return 0;
    *out = m->v[k].i;
    return 1;
}
static int AMediaFormat_getFloat(media_format *m, const char *key, float *out) {
    int k = fmt_find(m, key);
    if (k < 0) return 0;
    *out = m->v[k].kind == 'f' ? m->v[k].f : (float)m->v[k].i;
    return 1;
}
static int AMediaFormat_getString(media_format *m, const char *key, const char **out) {
    int k = fmt_find(m, key);
    if (k < 0 || m->v[k].kind != 's') return 0;
    *out = m->v[k].s;
    return 1;
}
static void AMediaFormat_setInt32(media_format *m, const char *key, int32_t v) { if (m) fmt_int(m, key, v, 'i'); }

// ---- AMediaDataSource: the game's own reader (ARM callbacks) -------------------------------
typedef struct {
    void *user;
    uint64_t read_at, get_size, close; // guest functions
} media_source;
static media_source *AMediaDataSource_new(void) { return calloc(1, sizeof(media_source)); }
static void AMediaDataSource_delete(media_source *d) {
    if (d && d->close) ((void (*)(void *))arm_callback(d->close))(d->user);
    free(d);
}
static void AMediaDataSource_setUserdata(media_source *d, void *u) { d->user = u; }
static void AMediaDataSource_setReadAt(media_source *d, uint64_t f) { d->read_at = f; }
static void AMediaDataSource_setGetSize(media_source *d, uint64_t f) { d->get_size = f; }
static void AMediaDataSource_setClose(media_source *d, uint64_t f) { d->close = f; }

// ---- AMediaExtractor: the mp4's tracks ----------------------------------------------------
#define MAX_TRACKS 4
typedef struct {
    int fd;          // ours (a dup or a path opened), or -1
    int64_t base, length;
    media_source *src;
    int tracks;
    struct {
        char mime[40];
        int width, height, rate, channels, fps;
        int64_t duration_us;
    } t[MAX_TRACKS];
    // step 2: FFmpeg's reader; the sample in hand (cur) is the next one of a chosen track
    int real;
    AVFormatContext *fc;
    AVIOContext *io;
    int64_t pos;
    int stream_of[MAX_TRACKS], chosen[MAX_TRACKS];
    AVPacket *cur;
    int cur_ok, cur_track;
} media_extractor;

static int64_t ex_read(media_extractor *e, int64_t off, void *buf, size_t n) {
    if (e->src && e->src->read_at)
        return ((int64_t (*)(void *, int64_t, void *, size_t))arm_callback(e->src->read_at))(e->src->user, off, buf, n);
    if (e->fd < 0) return -1;
    return pread(e->fd, buf, n, e->base + off);
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// the boxes in [from, to): containers walked, the ones with a track's facts read
static void mp4_walk(media_extractor *e, int64_t from, int64_t to, int depth, int *cur, uint32_t *timescale) {
    uint8_t h[16];
    for (int64_t at = from; at + 8 <= to && depth < 8;) {
        if (ex_read(e, at, h, 16) < 8) return;
        uint64_t size = be32(h);
        uint32_t type = be32(h + 4);
        int64_t head = 8;
        if (size == 1) {
            size = be64(h + 8);
            head = 16;
        } else if (size == 0) {
            size = (uint64_t)(to - at);
        }
        if (size < (uint64_t)head || at + (int64_t)size > to + 8) return;
        int64_t body = at + head, end = at + (int64_t)size;
#define T(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))
        if (type == T('m', 'o', 'o', 'v') || type == T('m', 'd', 'i', 'a') || type == T('m', 'i', 'n', 'f') ||
            type == T('s', 't', 'b', 'l')) {
            mp4_walk(e, body, end, depth + 1, cur, timescale);
        } else if (type == T('t', 'r', 'a', 'k')) {
            if (e->tracks < MAX_TRACKS) {
                *cur = e->tracks++;
                memset(&e->t[*cur], 0, sizeof(e->t[*cur]));
                mp4_walk(e, body, end, depth + 1, cur, timescale);
                if (!e->t[*cur].mime[0]) e->tracks--; // not audio or video
            }
        } else if (*cur >= 0 && type == T('t', 'k', 'h', 'd')) {
            uint8_t b[96];
            int64_t n = ex_read(e, body, b, sizeof(b));
            int off = b[0] == 1 ? 88 : 76; // width, height (16.16) at its end
            if (n >= off + 8) {
                e->t[*cur].width = (int)(be32(b + off) >> 16);
                e->t[*cur].height = (int)(be32(b + off + 4) >> 16);
            }
        } else if (*cur >= 0 && type == T('m', 'd', 'h', 'd')) {
            uint8_t b[32];
            if (ex_read(e, body, b, sizeof(b)) >= 32) {
                uint64_t dur;
                if (b[0] == 1) {
                    *timescale = be32(b + 20);
                    dur = be64(b + 24);
                } else {
                    *timescale = be32(b + 12);
                    dur = be32(b + 16);
                }
                if (*timescale) e->t[*cur].duration_us = (int64_t)(dur * 1000000ULL / *timescale);
            }
        } else if (*cur >= 0 && type == T('s', 't', 's', 'd')) {
            uint8_t b[48];
            if (ex_read(e, body, b, sizeof(b)) >= 44) {
                uint32_t codec = be32(b + 12); // the first entry's format
                if (codec == T('a', 'v', 'c', '1') || codec == T('a', 'v', 'c', '3')) snprintf(e->t[*cur].mime, 40, "video/avc");
                else if (codec == T('h', 'v', 'c', '1') || codec == T('h', 'e', 'v', '1')) snprintf(e->t[*cur].mime, 40, "video/hevc");
                else if (codec == T('v', 'p', '0', '9')) snprintf(e->t[*cur].mime, 40, "video/x-vnd.on2.vp9");
                else if (codec == T('m', 'p', '4', 'a')) {
                    snprintf(e->t[*cur].mime, 40, "audio/mp4a-latm");
                    e->t[*cur].channels = (int)(b[16 + 16] << 8 | b[16 + 17]);
                    e->t[*cur].rate = (int)(be32(b + 16 + 24) >> 16);
                }
            }
        }
#undef T
        at = end;
    }
}

static int64_t ex_size(media_extractor *e) {
    int64_t size = e->length;
    if (size <= 0 && e->src && e->src->get_size) size = ((int64_t (*)(void *))arm_callback(e->src->get_size))(e->src->user);
    if (size <= 0 && e->fd >= 0) size = lseek(e->fd, 0, SEEK_END) - e->base;
    return size;
}

// ---- step 2: FFmpeg reads the file through ex_read (the game's file, or its own reader) ----
static int io_read(void *o, uint8_t *buf, int n) {
    media_extractor *e = o;
    const int64_t r = ex_read(e, e->pos, buf, (size_t)n);
    if (r <= 0) return AVERROR_EOF;
    e->pos += r;
    return (int)r;
}
static int64_t io_seek(void *o, int64_t off, int whence) {
    media_extractor *e = o;
    const int64_t size = ex_size(e);
    if (whence & AVSEEK_SIZE) return size > 0 ? size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET) e->pos = off;
    else if (whence == SEEK_CUR) e->pos += off;
    else if (whence == SEEK_END && size > 0) e->pos = size + off;
    else return AVERROR(EINVAL);
    return e->pos;
}
static const char *mime_of(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_AV1: return "video/av01";
    case AV_CODEC_ID_MPEG4: return "video/mp4v-es";
    case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
    case AV_CODEC_ID_MP3: return "audio/mpeg";
    case AV_CODEC_ID_OPUS: return "audio/opus";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    default: return NULL;
    }
}
// the last tracks read, for a decoder whose format came without our key (the game made its own)
static AVCodecParameters *last_par[2]; // [0] audio, [1] video
static int ex_open_av(media_extractor *e) {
    enum { IO_BUF = 1 << 16 };
    e->pos = 0;
    e->io = avio_alloc_context(av_malloc(IO_BUF), IO_BUF, 0, e, io_read, NULL, io_seek);
    e->fc = avformat_alloc_context();
    if (!e->io || !e->fc) return -1;
    e->fc->pb = e->io;
    if (avformat_open_input(&e->fc, NULL, NULL, NULL) < 0) {
        e->fc = NULL; // freed by FFmpeg
        return -1;
    }
    avformat_find_stream_info(e->fc, NULL);
    e->tracks = 0;
    for (unsigned s = 0; s < e->fc->nb_streams && e->tracks < MAX_TRACKS; s++) {
        AVStream *st = e->fc->streams[s];
        const AVCodecParameters *p = st->codecpar;
        const char *mime = mime_of(p->codec_id);
        if (!mime || (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO)) {
            st->discard = AVDISCARD_ALL;
            continue;
        }
        const int t = e->tracks++;
        memset(&e->t[t], 0, sizeof(e->t[t]));
        snprintf(e->t[t].mime, sizeof(e->t[t].mime), "%s", mime);
        e->t[t].width = p->width;
        e->t[t].height = p->height;
        e->t[t].rate = p->sample_rate;
        e->t[t].channels = p->ch_layout.nb_channels;
        e->t[t].fps = st->avg_frame_rate.den ? (int)(av_q2d(st->avg_frame_rate) + 0.5) : 30;
        e->t[t].duration_us = st->duration != AV_NOPTS_VALUE ? av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q)
                                                              : (e->fc->duration > 0 ? e->fc->duration : 0);
        e->stream_of[t] = (int)s;
        st->discard = AVDISCARD_ALL; // until the game chooses it
    }
    e->cur = av_packet_alloc();
    e->cur_track = -1;
    for (int i = 0; i < e->tracks; i++)
        droid_log("video: track %d %s %dx%d %d fps, %d Hz %d ch, %.1f s (FFmpeg)", i, e->t[i].mime, e->t[i].width,
                  e->t[i].height, e->t[i].fps, e->t[i].rate, e->t[i].channels, e->t[i].duration_us / 1e6);
    return e->tracks ? 0 : -1;
}
static void ex_close_av(media_extractor *e) {
    if (e->cur) av_packet_free(&e->cur);
    if (e->fc) avformat_close_input(&e->fc);
    if (e->io) {
        av_freep(&e->io->buffer);
        avio_context_free(&e->io);
    }
}
static int track_of_stream(media_extractor *e, int s) {
    for (int t = 0; t < e->tracks; t++)
        if (e->stream_of[t] == s) return e->chosen[t] ? t : -1;
    return -1;
}
// the sample in hand: the next packet of a chosen track (-1: the file is over)
static void ex_have(media_extractor *e) {
    if (e->cur_ok) return;
    e->cur_ok = 1;
    e->cur_track = -1;
    while (av_read_frame(e->fc, e->cur) >= 0) {
        const int t = track_of_stream(e, e->cur->stream_index);
        if (t >= 0) {
            e->cur_track = t;
            return;
        }
        av_packet_unref(e->cur);
    }
}

static void ex_parse(media_extractor *e) {
    if (e->real && ex_open_av(e) == 0) return;
    if (e->real) {
        droid_log("video: FFmpeg could not read the file: it ends at once");
        ex_close_av(e);
        e->real = 0;
    }
    const int64_t size = ex_size(e);
    int cur = -1;
    uint32_t ts = 0;
    e->tracks = 0;
    mp4_walk(e, 0, size > 0 ? size : (int64_t)1 << 40, 0, &cur, &ts);
    if (!e->tracks) { // not an mp4 we read: one video track all the same, so it "plays" (and ends)
        e->tracks = 1;
        snprintf(e->t[0].mime, sizeof(e->t[0].mime), "video/avc");
        e->t[0].width = 1280;
        e->t[0].height = 720;
    }
    for (int i = 0; i < e->tracks; i++)
        droid_log("video: track %d %s %dx%d, %.1f s (ends at once: no decoding yet)", i, e->t[i].mime, e->t[i].width,
                  e->t[i].height, e->t[i].duration_us / 1e6);
}

// Games that get no video reader at all, as before Hydroid had one (24 Oct): Unity gives the video
// up at once and goes on. Shadow Fight 2's start waits for a 102 s video it never shows: with
// "videos end at once" and with the real player it waited for ever (a black screen at 60 fps,
// 2026-10-29). By exact package: a prefix would take Shadow Fight 3 too.
static int no_video_game(void) {
    extern const char *java_package; // jni_impl.c
    return java_package && !strcmp(java_package, "com.nekki.shadowfight");
}

static media_extractor *AMediaExtractor_new(void) {
    if (no_video_game()) {
        static int told;
        if (!told++) droid_log("video: no video reader for this game (as before 24 Oct: it goes on without its video)");
        return NULL;
    }
    media_extractor *e = calloc(1, sizeof(*e));
    if (e) {
        e->fd = -1;
        e->real = video_real();
    }
    return e;
}
static int AMediaExtractor_delete(media_extractor *e) {
    if (e) ex_close_av(e);
    if (e && e->fd >= 0) close(e->fd);
    free(e);
    return AMEDIA_OK;
}
static int AMediaExtractor_setDataSourceFd(media_extractor *e, int fd, int64_t offset, int64_t length) {
    e->fd = dup(fd);
    if (e->fd < 0) e->fd = fd; // the console may refuse dup: the game's own (kept open by it)
    e->base = offset;
    e->length = length;
    ex_parse(e);
    return AMEDIA_OK;
}
static int AMediaExtractor_setDataSource(media_extractor *e, const char *location) {
    e->fd = open(location, O_RDONLY);
    droid_log("video: %s%s", location, e->fd < 0 ? " (not opened)" : "");
    ex_parse(e);
    return AMEDIA_OK;
}
static int AMediaExtractor_setDataSourceCustom(media_extractor *e, media_source *src) {
    e->src = src;
    ex_parse(e);
    return AMEDIA_OK;
}
static size_t AMediaExtractor_getTrackCount(media_extractor *e) { return (size_t)e->tracks; }
static media_format *AMediaExtractor_getTrackFormat(media_extractor *e, size_t i) {
    media_format *m = fmt_new();
    if (!m || i >= (size_t)e->tracks) return m;
    fmt_str(m, "mime", e->t[i].mime);
    fmt_int(m, "durationUs", e->t[i].duration_us, 'l');
    if (!strncmp(e->t[i].mime, "video/", 6)) {
        fmt_int(m, "width", e->t[i].width, 'i');
        fmt_int(m, "height", e->t[i].height, 'i');
        fmt_int(m, "frame-rate", e->t[i].fps ? e->t[i].fps : 30, 'i');
    } else {
        fmt_int(m, "sample-rate", e->t[i].rate ? e->t[i].rate : 44100, 'i');
        fmt_int(m, "channel-count", e->t[i].channels ? e->t[i].channels : 2, 'i');
    }
    if (e->fc) { // our key: the decoder takes FFmpeg's own facts of this track (avcC & co.)
        AVCodecParameters *p = e->fc->streams[e->stream_of[i]]->codecpar;
        fmt_int(m, "hydroid-par", (int64_t)(intptr_t)p, 'l');
        last_par[p->codec_type == AVMEDIA_TYPE_VIDEO] = p;
    }
    return m;
}
static int AMediaExtractor_selectTrack(media_extractor *e, size_t i) {
    if (!e->fc || i >= (size_t)e->tracks) return AMEDIA_OK;
    e->chosen[i] = 1;
    e->fc->streams[e->stream_of[i]]->discard = AVDISCARD_DEFAULT;
    return AMEDIA_OK;
}
// the last seek's time: a decoder flushed after it hands out no picture before it (the
// seek lands on the key frame before; Unity threw the earlier ones away anyway, one by one,
// and was late again: it sought on, a picture in a thousand shown)
static int64_t seek_target_us = -1;
static int AMediaExtractor_seekTo(media_extractor *e, int64_t us, int mode) {
    if (!e->fc) return AMEDIA_OK;
    seek_target_us = us;
    static int seeks;
    if (seeks++ < 40) droid_log("video: seek to %.3f s (mode %d)", us / 1e6, mode);
    if (e->cur_ok) av_packet_unref(e->cur);
    e->cur_ok = 0;
    av_seek_frame(e->fc, -1, us, AVSEEK_FLAG_BACKWARD);
    return AMEDIA_OK;
}
// step 1 (no FFmpeg): no samples, the decoder says "end of stream"
static ssize_t AMediaExtractor_readSampleData(media_extractor *e, uint8_t *buf, size_t n) {
    if (!e->fc) return -1;
    ex_have(e);
    if (e->cur_track < 0) return -1;
    const size_t k = (size_t)e->cur->size < n ? (size_t)e->cur->size : n;
    memcpy(buf, e->cur->data, k);
    return (ssize_t)k;
}
static int AMediaExtractor_getSampleTrackIndex(media_extractor *e) {
    if (!e->fc) return -1;
    ex_have(e);
    return e->cur_track;
}
static int64_t AMediaExtractor_getSampleTime(media_extractor *e) {
    if (!e->fc) return -1;
    ex_have(e);
    if (e->cur_track < 0) return -1;
    const AVStream *st = e->fc->streams[e->cur->stream_index];
    int64_t ts = e->cur->pts != AV_NOPTS_VALUE ? e->cur->pts : e->cur->dts;
    if (ts == AV_NOPTS_VALUE) return 0;
    if (st->start_time != AV_NOPTS_VALUE) ts -= st->start_time;
    const int64_t us = av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q);
    return us < 0 ? 0 : us; // (AAC's first packet: a little before 0)
}
static int AMediaExtractor_advance(media_extractor *e) {
    if (!e->fc) return 0;
    ex_have(e);
    if (e->cur_track >= 0) av_packet_unref(e->cur);
    e->cur_ok = 0;
    ex_have(e);
    return e->cur_track >= 0;
}

// ---- AMediaCodec: step 1 done at once; step 2 FFmpeg's decoder -----------------------------
// Step 2: one input buffer (sent to the decoder at once; kept while the decoder is full), and
// OUT_SLOTS output buffers of decoded frames. The game takes them in order and gives them back
// (releaseOutputBuffer); while they are all taken it gets no input buffer, so it can't run ahead.
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define OUT_SLOTS 6
typedef struct {
    int32_t offset, size;
    int64_t presentation_us;
    uint32_t flags;
} codec_buffer_info;
typedef struct {
    char mime[40];
    media_format *out_format;
    uint8_t in[64 << 10], out[64 << 10]; // step 1's
    // step 2
    int real;
    pthread_mutex_t lock;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pending; // given by the game, not taken by the decoder yet
    int has_pending, eos_in, eos_sent, drained, eos_given, format_told;
    int64_t skip_before; // after a seek: pictures before this time not handed out (-1: none)
    struct SwsContext *sws;
    SwrContext *swr;
    uint8_t *inbuf;
    size_t in_cap;
    struct {
        uint8_t *data;
        size_t cap, size;
        int64_t pts;
        int state; // 0 free, 1 ready, 2 with the game
        uint64_t seq;
    } out_slot[OUT_SLOTS];
    uint64_t seq;
    video_surface *vs; // configured with a Surface of a SurfaceTexture: rendered buffers go there
    unsigned long n_in_ask, n_in, n_out_ask, n_out, n_rel, n_render; // what the game did (the log)
    time_t told_at;
} media_codec;

static int slot_free(media_codec *c) {
    for (int i = 0; i < OUT_SLOTS; i++)
        if (!c->out_slot[i].state) return i;
    return -1;
}
static uint8_t *slot_room(media_codec *c, int i, size_t n) {
    if (c->out_slot[i].cap < n) {
        free(c->out_slot[i].data);
        c->out_slot[i].data = malloc(n);
        c->out_slot[i].cap = c->out_slot[i].data ? n : 0;
    }
    return c->out_slot[i].data;
}
// a decoded frame into a free slot: video NV12 (the stride its width), audio 16-bit interleaved
static void slot_fill(media_codec *c, int i, const AVFrame *f) {
    size_t n = 0;
    if (c->ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        const int w = f->width, h = f->height;
        n = (size_t)w * h + (size_t)w * ((h + 1) / 2);
        uint8_t *d = slot_room(c, i, n);
        if (d && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P)) { // by hand: fast
            for (int y = 0; y < h; y++) memcpy(d + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
            uint8_t *uv = d + (size_t)w * h;
            for (int y = 0; y < (h + 1) / 2; y++) {
                const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1], *v = f->data[2] + (size_t)y * f->linesize[2];
                uint8_t *o = uv + (size_t)y * w;
                for (int x = 0; x < w / 2; x++) {
                    o[2 * x] = u[x];
                    o[2 * x + 1] = v[x];
                }
            }
        } else if (d && (c->sws = sws_getCachedContext(c->sws, w, h, (enum AVPixelFormat)f->format, w, h,
                                                       AV_PIX_FMT_NV12, SWS_POINT, NULL, NULL, NULL))) {
            uint8_t *planes[4] = { d, d + (size_t)w * h, NULL, NULL };
            int strides[4] = { w, w, 0, 0 };
            sws_scale(c->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, planes, strides);
        }
    } else {
        const int ch = f->ch_layout.nb_channels;
        if (!c->swr) {
            swr_alloc_set_opts2(&c->swr, &f->ch_layout, AV_SAMPLE_FMT_S16, f->sample_rate, &f->ch_layout,
                                (enum AVSampleFormat)f->format, f->sample_rate, 0, NULL);
            if (c->swr) swr_init(c->swr);
        }
        n = (size_t)f->nb_samples * ch * 2;
        uint8_t *d = slot_room(c, i, n);
        if (d && c->swr) {
            const int got = swr_convert(c->swr, &d, f->nb_samples, (const uint8_t **)f->extended_data, f->nb_samples);
            n = got > 0 ? (size_t)got * ch * 2 : 0;
        }
    }
    c->out_slot[i].size = n;
    c->out_slot[i].pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    c->out_slot[i].state = 1;
    c->out_slot[i].seq = ++c->seq;
}
// the decoder's frames into free slots, the pending packet into the decoder when it has room
static void pump(media_codec *c) {
    for (int guard = 0; guard < 64; guard++) {
        if (c->has_pending) {
            const int r = avcodec_send_packet(c->ctx, c->pending);
            if (r != AVERROR(EAGAIN)) {
                av_packet_unref(c->pending);
                c->has_pending = 0;
            }
        }
        if (!c->has_pending && c->eos_in && !c->eos_sent) {
            if (avcodec_send_packet(c->ctx, NULL) != AVERROR(EAGAIN)) c->eos_sent = 1;
        }
        const int i = slot_free(c);
        if (i < 0) return;
        const int r = avcodec_receive_frame(c->ctx, c->frame);
        if (r == 0) {
            const int64_t pts = c->frame->best_effort_timestamp != AV_NOPTS_VALUE ? c->frame->best_effort_timestamp : c->frame->pts;
            if (c->skip_before >= 0 && pts != AV_NOPTS_VALUE && pts < c->skip_before - 20000) {
                av_frame_unref(c->frame); // before the seek's time: not handed out
                continue;
            }
            c->skip_before = -1;
            slot_fill(c, i, c->frame);
            av_frame_unref(c->frame);
            continue;
        }
        if (r == AVERROR_EOF) c->drained = 1;
        if (r != AVERROR(EAGAIN) || !c->has_pending) return;
    }
}

static void codec_tell(media_codec *c) { // every 2 s while a video plays: what the game did
    const time_t now = time(NULL);
    if (now - c->told_at < 2) return;
    c->told_at = now;
    droid_log("video: %s%s: input asked %lu given %lu, output asked %lu given %lu, back %lu (%lu drawn)", c->mime,
              c->vs ? " (to a surface)" : "", c->n_in_ask, c->n_in, c->n_out_ask, c->n_out, c->n_rel, c->n_render);
}

static media_codec *AMediaCodec_createDecoderByType(const char *mime) {
    media_codec *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    snprintf(c->mime, sizeof(c->mime), "%s", mime ? mime : "");
    c->out_format = fmt_new();
    if (c->out_format) fmt_str(c->out_format, "mime", c->mime);
    c->real = video_real();
    pthread_mutex_init(&c->lock, NULL);
    droid_log("video: decoder for %s%s", c->mime, c->real ? "" : " (ends at once)");
    return c;
}
static void codec_free_av(media_codec *c) {
    if (c->ctx) avcodec_free_context(&c->ctx);
    if (c->frame) av_frame_free(&c->frame);
    if (c->pending) av_packet_free(&c->pending);
    if (c->sws) sws_freeContext(c->sws);
    if (c->swr) swr_free(&c->swr);
    c->sws = NULL;
    for (int i = 0; i < OUT_SLOTS; i++) free(c->out_slot[i].data);
    memset(c->out_slot, 0, sizeof(c->out_slot));
    free(c->inbuf);
    c->inbuf = NULL;
}
static int AMediaCodec_delete(media_codec *c) {
    if (c) {
        codec_free_av(c);
        free(c->out_format);
        pthread_mutex_destroy(&c->lock);
    }
    free(c);
    return AMEDIA_OK;
}
static int AMediaCodec_configure(media_codec *c, media_format *f, void *surface, void *crypto, uint32_t flags) {
    (void)crypto;
    droid_log("video: %s decoder configured, surface %p, flags %#x", c->mime, surface, flags);
    int32_t w = 0, h = 0;
    if (f && c->out_format && AMediaFormat_getInt32(f, "width", &w) && AMediaFormat_getInt32(f, "height", &h)) {
        fmt_int(c->out_format, "width", w, 'i');
        fmt_int(c->out_format, "height", h, 'i');
        fmt_int(c->out_format, "color-format", 21, 'i'); // YUV420 semi-planar
    }
    if (!c->real) return AMEDIA_OK;
    video_window *vw = video_window_of(surface);
    c->vs = vw ? vw->vs : NULL;
    int64_t pp = 0;
    const AVCodecParameters *par = f && AMediaFormat_getInt64(f, "hydroid-par", &pp) ? (const AVCodecParameters *)(intptr_t)pp
                                                                                    : last_par[!strncmp(c->mime, "video/", 6)];
    const AVCodec *dec = par ? avcodec_find_decoder(par->codec_id) : NULL;
    if (!dec || !(c->ctx = avcodec_alloc_context3(dec)) || avcodec_parameters_to_context(c->ctx, par) < 0) {
        droid_log("video: no FFmpeg decoder for %s: it ends at once", c->mime);
        codec_free_av(c);
        c->real = 0;
        return AMEDIA_OK;
    }
    c->ctx->pkt_timebase = AV_TIME_BASE_Q; // the game's times: microseconds
    c->ctx->thread_count = c->ctx->codec_type == AVMEDIA_TYPE_VIDEO ? 4 : 1;
    if (avcodec_open2(c->ctx, dec, NULL) < 0) {
        droid_log("video: FFmpeg's %s decoder did not open: it ends at once", dec->name);
        codec_free_av(c);
        c->real = 0;
        return AMEDIA_OK;
    }
    c->frame = av_frame_alloc();
    c->pending = av_packet_alloc();
    c->in_cap = c->ctx->codec_type == AVMEDIA_TYPE_VIDEO ? (size_t)8 << 20 : (size_t)256 << 10;
    c->inbuf = malloc(c->in_cap);
    if (c->ctx->codec_type == AVMEDIA_TYPE_AUDIO && c->out_format) {
        fmt_int(c->out_format, "sample-rate", c->ctx->sample_rate, 'i');
        fmt_int(c->out_format, "channel-count", c->ctx->ch_layout.nb_channels, 'i');
        fmt_int(c->out_format, "pcm-encoding", 2, 'i'); // 16-bit
    }
    droid_log("video: FFmpeg %s decoder (%s)", dec->name, c->mime);
    return AMEDIA_OK;
}
static int AMediaCodec_start(media_codec *c) { (void)c; return AMEDIA_OK; }
static int AMediaCodec_stop(media_codec *c) { (void)c; return AMEDIA_OK; }
static int AMediaCodec_flush(media_codec *c) {
    if (!c->real) return AMEDIA_OK;
    static int flushes;
    if (flushes++ < 40) droid_log("video: %s decoder flushed", c->mime);
    pthread_mutex_lock(&c->lock);
    avcodec_flush_buffers(c->ctx);
    if (c->has_pending) av_packet_unref(c->pending);
    c->has_pending = c->eos_in = c->eos_sent = c->drained = c->eos_given = 0;
    c->skip_before = seek_target_us;
    for (int i = 0; i < OUT_SLOTS; i++) c->out_slot[i].state = 0;
    pthread_mutex_unlock(&c->lock);
    return AMEDIA_OK;
}
static ssize_t AMediaCodec_dequeueInputBuffer(media_codec *c, int64_t timeout) {
    if (!c->real) return 0;
    pthread_mutex_lock(&c->lock);
    c->n_in_ask++;
    codec_tell(c);
    pump(c);
    const int full = c->has_pending || c->eos_in || slot_free(c) < 0;
    pthread_mutex_unlock(&c->lock);
    if (full && timeout > 0) usleep(timeout < 2000 ? (useconds_t)timeout : 2000);
    return full ? AMEDIACODEC_INFO_TRY_AGAIN_LATER : 0;
}
static uint8_t *AMediaCodec_getInputBuffer(media_codec *c, size_t i, size_t *size) {
    (void)i;
    if (c->real) {
        if (size) *size = c->in_cap;
        return c->inbuf;
    }
    if (size) *size = sizeof(c->in);
    return c->in;
}
static int AMediaCodec_queueInputBuffer(media_codec *c, size_t i, int64_t off, size_t size, uint64_t us, uint32_t flags) {
    (void)i;
    if (!c->real) return AMEDIA_OK;
    pthread_mutex_lock(&c->lock);
    c->n_in++;
    if (size && off >= 0 && (size_t)off + size <= c->in_cap && av_new_packet(c->pending, (int)size) == 0) {
        memcpy(c->pending->data, c->inbuf + off, size);
        c->pending->pts = c->pending->dts = (int64_t)us;
        c->has_pending = 1;
    }
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) c->eos_in = 1;
    pump(c);
    pthread_mutex_unlock(&c->lock);
    return AMEDIA_OK;
}
static ssize_t AMediaCodec_dequeueOutputBuffer(media_codec *c, codec_buffer_info *info, int64_t timeout) {
    if (!c->real) {
        if (info) {
            memset(info, 0, sizeof(*info));
            info->flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM; // the video is over
        }
        return 0;
    }
    pthread_mutex_lock(&c->lock);
    c->n_out_ask++;
    codec_tell(c);
    pump(c);
    int best = -1;
    for (int i = 0; i < OUT_SLOTS; i++)
        if (c->out_slot[i].state == 1 && (best < 0 || c->out_slot[i].seq < c->out_slot[best].seq)) best = i;
    ssize_t r = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    if (best >= 0 && !c->format_told) {
        c->format_told = 1; // the real output first, as Android tells it (Unity waits for it: audio too)
        if (c->ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            fmt_int(c->out_format, "width", c->ctx->width, 'i');
            fmt_int(c->out_format, "height", c->ctx->height, 'i');
            fmt_int(c->out_format, "stride", c->ctx->width, 'i');
            fmt_int(c->out_format, "slice-height", c->ctx->height, 'i');
            fmt_int(c->out_format, "color-format", 21, 'i');
        } else {
            fmt_int(c->out_format, "sample-rate", c->ctx->sample_rate, 'i');
            fmt_int(c->out_format, "channel-count", c->ctx->ch_layout.nb_channels, 'i');
            fmt_int(c->out_format, "pcm-encoding", 2, 'i');
        }
        r = AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    } else if (best >= 0) {
        c->out_slot[best].state = 2;
        c->n_out++;
        if (info) {
            info->offset = 0;
            info->size = (int32_t)c->out_slot[best].size;
            info->presentation_us = c->out_slot[best].pts;
            info->flags = 0;
        }
        r = best;
    } else if (c->drained && !c->eos_given) {
        const int i = slot_free(c);
        if (i >= 0) {
            c->eos_given = 1;
            c->out_slot[i].state = 2;
            c->out_slot[i].size = 0;
            if (info) {
                memset(info, 0, sizeof(*info));
                info->flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
            }
            r = i;
            droid_log("video: %s played to its end", c->mime);
        }
    }
    pthread_mutex_unlock(&c->lock);
    if (r == AMEDIACODEC_INFO_TRY_AGAIN_LATER && timeout > 0) usleep(timeout < 2000 ? (useconds_t)timeout : 2000);
    return r;
}
static uint8_t *AMediaCodec_getOutputBuffer(media_codec *c, size_t i, size_t *size) {
    if (c->real && i < OUT_SLOTS) {
        if (size) *size = c->out_slot[i].cap;
        return c->out_slot[i].data ? c->out_slot[i].data : c->out;
    }
    if (size) *size = sizeof(c->out);
    return c->out;
}
static media_format *AMediaCodec_getOutputFormat(media_codec *c) {
    media_format *m = fmt_new(); // the caller's to delete: a copy
    if (m && c->out_format) memcpy(m, c->out_format, sizeof(*m));
    return m;
}
static int AMediaCodec_releaseOutputBuffer(media_codec *c, size_t i, int render) {
    if (c->real && i < OUT_SLOTS) {
        c->n_rel++;
        c->n_render += render != 0;
        if (render && c->vs && c->out_slot[i].size && c->ctx && c->ctx->codec_type == AVMEDIA_TYPE_VIDEO)
            vsurf_frame_nv12(c->vs, c->out_slot[i].data, c->ctx->width, c->ctx->height, c->out_slot[i].pts);
        pthread_mutex_lock(&c->lock);
        c->out_slot[i].state = 0;
        pthread_mutex_unlock(&c->lock);
    }
    return AMEDIA_OK;
}
static int AMediaCodec_setOutputSurface(media_codec *c, void *surface) { (void)c; (void)surface; return AMEDIA_OK; }

// ---- ANativeWindow from a Java Surface (the video's picture): one of ours -----------------
static droid_window video_window_plain = { 1280, 720 };
void *java_surface_video(void *surface); // java_layer.c: a SurfaceTexture's picture, or NULL
// (linux_abi.c's ANativeWindow_fromSurface, the one games find, asks here first)
void *mediandk_video_window(void *surface) {
    video_surface *vs = video_game() && surface ? java_surface_video(surface) : NULL;
    if (!vs) return NULL;
    video_window *w = calloc(1, sizeof(*w)); // (one per Surface the game asks for: a few per video)
    if (!w) return NULL;
    w->base.width = 1920;
    w->base.height = 1080;
    w->magic = VIDEO_WINDOW_MAGIC;
    w->vs = vs;
    return w;
}
static void *ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env;
    void *w = mediandk_video_window(surface);
    return w ? w : &video_window_plain;
}
video_window *video_window_of(void *window) {
    video_window *w = window;
    return w && w != (void *)&video_window_plain && w->magic == VIDEO_WINDOW_MAGIC ? w : NULL;
}
static void *ANativeWindow_toSurface(void *env, void *window) { (void)env; (void)window; return NULL; }

#define E(name) { #name, (void *)name }
static const struct { const char *name; void *fn; } table[] = {
    E(AMediaFormat_new), E(AMediaFormat_delete), E(AMediaFormat_getInt32), E(AMediaFormat_getInt64),
    E(AMediaFormat_getFloat), E(AMediaFormat_getString), E(AMediaFormat_setInt32),
    E(AMediaDataSource_new), E(AMediaDataSource_delete), E(AMediaDataSource_setUserdata),
    E(AMediaDataSource_setReadAt), E(AMediaDataSource_setGetSize), E(AMediaDataSource_setClose),
    E(AMediaExtractor_new), E(AMediaExtractor_delete), E(AMediaExtractor_setDataSourceFd),
    E(AMediaExtractor_setDataSource), E(AMediaExtractor_setDataSourceCustom), E(AMediaExtractor_getTrackCount),
    E(AMediaExtractor_getTrackFormat), E(AMediaExtractor_selectTrack), E(AMediaExtractor_seekTo),
    E(AMediaExtractor_readSampleData), E(AMediaExtractor_getSampleTrackIndex), E(AMediaExtractor_getSampleTime),
    E(AMediaExtractor_advance),
    E(AMediaCodec_createDecoderByType), E(AMediaCodec_delete), E(AMediaCodec_configure), E(AMediaCodec_start),
    E(AMediaCodec_stop), E(AMediaCodec_flush), E(AMediaCodec_dequeueInputBuffer), E(AMediaCodec_getInputBuffer),
    E(AMediaCodec_queueInputBuffer), E(AMediaCodec_dequeueOutputBuffer), E(AMediaCodec_getOutputBuffer),
    E(AMediaCodec_getOutputFormat), E(AMediaCodec_releaseOutputBuffer), E(AMediaCodec_setOutputSurface),
    E(ANativeWindow_fromSurface), E(ANativeWindow_toSurface),
};

// the key names, as data (AVPro Video's plugin imports them: AMEDIAFORMAT_KEY_DURATION & co.)
#define KEY(name, text) const char *AMEDIAFORMAT_KEY_##name = text;
KEY(MIME, "mime") KEY(WIDTH, "width") KEY(HEIGHT, "height") KEY(DURATION, "durationUs")
KEY(CHANNEL_COUNT, "channel-count") KEY(SAMPLE_RATE, "sample-rate") KEY(COLOR_FORMAT, "color-format")
KEY(COLOR_STANDARD, "color-standard") KEY(COLOR_RANGE, "color-range") KEY(COLOR_TRANSFER, "color-transfer")
KEY(FRAME_RATE, "frame-rate") KEY(MAX_INPUT_SIZE, "max-input-size") KEY(STRIDE, "stride")
KEY(SLICE_HEIGHT, "slice-height") KEY(DISPLAY_WIDTH, "display-width") KEY(DISPLAY_HEIGHT, "display-height")
KEY(ROTATION, "rotation-degrees") KEY(BIT_RATE, "bitrate") KEY(LANGUAGE, "language") KEY(IS_ADTS, "is-adts")
KEY(PCM_ENCODING, "pcm-encoding") KEY(CSD_0, "csd-0") KEY(CSD_1, "csd-1") KEY(CSD_2, "csd-2")
KEY(PROFILE, "profile") KEY(LEVEL, "level") KEY(AAC_PROFILE, "aac-profile") KEY(TRACK_ID, "track-id")
KEY(I_FRAME_INTERVAL, "i-frame-interval") KEY(MAX_WIDTH, "max-width") KEY(MAX_HEIGHT, "max-height")
KEY(HDR_STATIC_INFO, "hdr-static-info") KEY(DISPLAY_CROP, "crop")
#undef KEY
#define D(name) { "AMEDIAFORMAT_KEY_" #name, (void *)&AMEDIAFORMAT_KEY_##name }
static const struct { const char *name; void *addr; } keys[] = {
    D(MIME), D(WIDTH), D(HEIGHT), D(DURATION), D(CHANNEL_COUNT), D(SAMPLE_RATE), D(COLOR_FORMAT),
    D(COLOR_STANDARD), D(COLOR_RANGE), D(COLOR_TRANSFER), D(FRAME_RATE), D(MAX_INPUT_SIZE), D(STRIDE),
    D(SLICE_HEIGHT), D(DISPLAY_WIDTH), D(DISPLAY_HEIGHT), D(ROTATION), D(BIT_RATE), D(LANGUAGE), D(IS_ADTS),
    D(PCM_ENCODING), D(CSD_0), D(CSD_1), D(CSD_2), D(PROFILE), D(LEVEL), D(AAC_PROFILE), D(TRACK_ID),
    D(I_FRAME_INTERVAL), D(MAX_WIDTH), D(MAX_HEIGHT), D(HDR_STATIC_INFO), D(DISPLAY_CROP),
};

void *mediandk_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcmp(keys[i].name, name) == 0) return keys[i].addr;
    return NULL;
}
