// video_player.c — Unity's full-screen movie (Handheld.PlayFullScreenMovie: the Java
// UnityPlayer.showVideoPlayer). Unity's own Java plays the file in a view on top and pauses the
// player until it ends (com.unity3d.player.t: pause(), then resume() when done or skipped).
// Here: showVideoPlayer starts our player (FFmpeg: a decode thread, a sound thread on an audio
// port of its own) and answers true; unity_host.c stops calling nativeRender while it plays
// (the pause); the picture is drawn on Unity's render thread, the one holding its GL context,
// inside the swap it was in: that swap goes on showing the video until it's over, then gives
// every GL setting back as it was (Unity keeps its own copy of them) and returns.
// Games in VIDEO_PLAYER_GAMES only (Vector 2 first).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "android_shim.h"
#include "apk.h"
#include "game_settings.h"

#define VIDEO_PLAYER_GAMES "com.nekki.vector2"

void *egl_resolve(const char *name);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);
int sceAudioOutClose(int32_t handle);

static double vp_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#define AUDIO_GRAIN 1024             // frames per output (16-bit stereo, 48 kHz)
#define AUDIO_RING (48000 * 2 * 2)   // 2 s of samples (int16, both channels)

static struct {
    pthread_mutex_t lock;
    volatile int active, skip, over, threads_done;
    int control, scaling;
    uint32_t bg; // ARGB
    // the file: in memory (an APK asset) or a file
    uint8_t *mem;
    size_t mem_size;
    int fd;
    int64_t base, length, pos;
    // the newest picture
    uint8_t *rgba;
    size_t rgba_cap;       // (rgba: the Y, U, V planes now)
    int w, h, fresh, full_range;
    double upload_s;
    unsigned long shown;
    // the sound: decoded samples waiting for the port
    int16_t ring[AUDIO_RING];
    size_t ring_r, ring_w; // in samples
    volatile int audio_eof;
    pthread_t decode_thread, audio_thread;
} vp = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

int video_player_game(void) { return droid_game_in(VIDEO_PLAYER_GAMES); }
int video_player_active(void) { return vp.active; }

// ---- reading the file --------------------------------------------------------------------
static int vp_read(void *o, uint8_t *buf, int n) {
    (void)o;
    int64_t left = vp.length - vp.pos;
    if (left <= 0) return AVERROR_EOF;
    if (n > left) n = (int)left;
    if (vp.mem) memcpy(buf, vp.mem + vp.pos, (size_t)n);
    else if ((n = (int)pread(vp.fd, buf, (size_t)n, vp.base + vp.pos)) <= 0) return AVERROR_EOF;
    vp.pos += n;
    return n;
}
static int64_t vp_seek(void *o, int64_t off, int whence) {
    (void)o;
    if (whence & AVSEEK_SIZE) return vp.length;
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET) vp.pos = off;
    else if (whence == SEEK_CUR) vp.pos += off;
    else if (whence == SEEK_END) vp.pos = vp.length + off;
    else return AVERROR(EINVAL);
    return vp.pos;
}

// ---- the sound: its own port, fed from the ring ------------------------------------------
static size_t ring_count(void) { return (vp.ring_w - vp.ring_r + AUDIO_RING) % AUDIO_RING; }
static void ring_put(const int16_t *s, size_t n) { // waits for room (the ring is 2 s)
    for (size_t i = 0; i < n;) {
        pthread_mutex_lock(&vp.lock);
        const size_t room = AUDIO_RING - 1 - ring_count();
        size_t k = n - i < room ? n - i : room;
        for (size_t j = 0; j < k; j++) {
            vp.ring[vp.ring_w] = s[i + j];
            vp.ring_w = (vp.ring_w + 1) % AUDIO_RING;
        }
        pthread_mutex_unlock(&vp.lock);
        i += k;
        if (i < n) {
            if (vp.skip) return;
            usleep(5000);
        }
    }
}
static void *audio_main(void *arg) {
    (void)arg;
    const int port = sceAudioOutOpen(0xff, 0, 0, AUDIO_GRAIN, 48000, 1);
    static int16_t out[AUDIO_GRAIN * 2];
    while (!vp.skip && !(vp.audio_eof && ring_count() == 0)) {
        pthread_mutex_lock(&vp.lock);
        size_t have = ring_count(), k = have < AUDIO_GRAIN * 2 ? have : AUDIO_GRAIN * 2;
        for (size_t j = 0; j < k; j++) {
            out[j] = vp.ring[vp.ring_r];
            vp.ring_r = (vp.ring_r + 1) % AUDIO_RING;
        }
        pthread_mutex_unlock(&vp.lock);
        memset(out + k, 0, (AUDIO_GRAIN * 2 - k) * sizeof(int16_t)); // (late: silence)
        if (port >= 0) sceAudioOutOutput(port, out); // blocks for one grain: the clock
        else usleep(AUDIO_GRAIN * 1000000 / 48000);
    }
    if (port >= 0) sceAudioOutClose(port);
    return NULL;
}

// ---- the picture and the sound out of the file -------------------------------------------
static void *decode_main(void *arg) {
    (void)arg;
    AVFormatContext *fc = avformat_alloc_context();
    AVIOContext *io = avio_alloc_context(av_malloc(1 << 16), 1 << 16, 0, NULL, vp_read, NULL, vp_seek);
    AVCodecContext *vc = NULL, *ac = NULL;
    struct SwsContext *sws = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int vs = -1, as = -1;
    fc->pb = io;
    if (avformat_open_input(&fc, NULL, NULL, NULL) < 0) {
        droid_log("video player: FFmpeg could not read the file");
        fc = NULL;
        goto done;
    }
    avformat_find_stream_info(fc, NULL);
    vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    as = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    for (int k = 0; k < 2; k++) {
        const int s = k ? as : vs;
        if (s < 0) continue;
        const AVCodec *d = avcodec_find_decoder(fc->streams[s]->codecpar->codec_id);
        AVCodecContext *c = d ? avcodec_alloc_context3(d) : NULL;
        if (!c || avcodec_parameters_to_context(c, fc->streams[s]->codecpar) < 0) continue;
        c->thread_count = k ? 1 : 4;
        if (avcodec_open2(c, d, NULL) < 0) {
            avcodec_free_context(&c);
            continue;
        }
        if (k) ac = c;
        else vc = c;
    }
    droid_log("video player: %s %dx%d, sound %s %d Hz, %.1f s", vc ? vc->codec->name : "no picture", vc ? vc->width : 0,
              vc ? vc->height : 0, ac ? ac->codec->name : "none", ac ? ac->sample_rate : 0,
              fc->duration > 0 ? fc->duration / 1e6 : 0.0);
    if (!vc) goto done;
    if (ac) pthread_create(&vp.audio_thread, NULL, audio_main, NULL);
    double start = 0, told = vp_now(), convert_s = 0;
    unsigned long n_frames = 0, n_dropped = 0;
    uint8_t *back = NULL;
    size_t back_cap = 0;
    const AVRational vtb = fc->streams[vs]->time_base;
    int eof = 0;
    while (!vp.skip) {
        int got_pkt = 0;
        if (!eof) {
            if (av_read_frame(fc, pkt) < 0) eof = 1;
            else got_pkt = 1;
        }
        AVCodecContext *c = NULL;
        if (got_pkt) c = pkt->stream_index == vs ? vc : pkt->stream_index == as ? ac : NULL;
        if (got_pkt && !c) {
            av_packet_unref(pkt);
            continue;
        }
        if (eof && !got_pkt) { // the end: both decoders drained
            if (vc) avcodec_send_packet(vc, NULL);
            if (ac) avcodec_send_packet(ac, NULL);
        } else {
            avcodec_send_packet(c, pkt);
            av_packet_unref(pkt);
        }
        for (int k = 0; k < 2; k++) {
            AVCodecContext *d = k ? ac : vc;
            if (!d || (!eof && d != c)) continue;
            while (!vp.skip && avcodec_receive_frame(d, f) == 0) {
                if (d == vc) {
                    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
                    const double t = ts != AV_NOPTS_VALUE ? ts * av_q2d(vtb) : 0;
                    if (!start) start = vp_now() - t;
                    const double wait = start + t - vp_now(); // the picture at its time
                    n_frames++;
                    if (wait < -0.05) { // late: dropped, as a player does (the time stays the film's)
                        n_dropped++;
                        av_frame_unref(f);
                        continue;
                    }
                    if (wait > 0 && wait < 1) usleep((useconds_t)(wait * 1e6));
                    const double c0 = vp_now();
                    // the picture as its Y, U and V planes (4:2:0, packed): the GPU makes the colours
                    // (as RGBA on the CPU: ~30 ms a 720p picture here, most of them dropped)
                    const int w = f->width, h = f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
                    const size_t need = (size_t)w * h + 2 * (size_t)cw * ch;
                    if (back_cap < need) {
                        free(back);
                        back = malloc(need);
                        back_cap = back ? need : 0;
                    }
                    const int planar = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
                    if (!planar)
                        sws = sws_getCachedContext(sws, w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_YUV420P,
                                                   SWS_POINT, NULL, NULL, NULL);
                    if (back && (planar || sws)) {
                        uint8_t *pl[3] = { back, back + (size_t)w * h, back + (size_t)w * h + (size_t)cw * ch };
                        int ls[3] = { w, cw, cw };
                        if (planar) {
                            for (int p = 0; p < 3; p++)
                                for (int y = 0; y < (p ? ch : h); y++)
                                    memcpy(pl[p] + (size_t)y * ls[p], f->data[p] + (size_t)y * f->linesize[p], (size_t)ls[p]);
                        } else {
                            uint8_t *dst[4] = { pl[0], pl[1], pl[2], NULL };
                            int stride[4] = { ls[0], ls[1], ls[2], 0 };
                            sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, stride);
                        }
                        vp.full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
                        pthread_mutex_lock(&vp.lock);
                        uint8_t *old = vp.rgba;
                        const size_t old_cap = vp.rgba_cap;
                        vp.rgba = back;
                        vp.rgba_cap = back_cap;
                        vp.w = f->width;
                        vp.h = f->height;
                        vp.fresh = 1;
                        pthread_mutex_unlock(&vp.lock);
                        back = old;
                        back_cap = old ? old_cap : 0;
                    }
                    convert_s += vp_now() - c0;
                    if (vp_now() - told >= 5) {
                        told = vp_now();
                        droid_log("video player: at %.1f s of the film (%.1f s played), %lu pictures, %lu dropped, "
                                  "%.1f ms to convert one, %lu uploaded",
                                  t, vp_now() - start, n_frames, n_dropped, n_frames > n_dropped ? convert_s * 1000 / (n_frames - n_dropped) : 0.0,
                                  vp.shown);
                    }
                } else {
                    if (!swr) {
                        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                        swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &f->ch_layout,
                                            (enum AVSampleFormat)f->format, f->sample_rate, 0, NULL);
                        if (swr) swr_init(swr);
                    }
                    if (swr) {
                        static int16_t pcm[8192 * 2];
                        uint8_t *outp = (uint8_t *)pcm;
                        const int n = swr_convert(swr, &outp, 8192, (const uint8_t **)f->extended_data, f->nb_samples);
                        if (n > 0) ring_put(pcm, (size_t)n * 2);
                    }
                }
                av_frame_unref(f);
            }
        }
        if (eof && !got_pkt) break;
    }
    free(back);
done:
    vp.audio_eof = 1;
    if (vp.audio_thread) pthread_join(vp.audio_thread, NULL);
    vp.audio_thread = 0;
    if (vc) avcodec_free_context(&vc);
    if (ac) avcodec_free_context(&ac);
    if (sws) sws_freeContext(sws);
    if (swr) swr_free(&swr);
    av_packet_free(&pkt);
    av_frame_free(&f);
    if (fc) avformat_close_input(&fc);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    droid_log("video player: %s", vp.skip ? "skipped" : "played to its end");
    vp.over = 1;
    return NULL;
}

// UnityPlayer.showVideoPlayer(file, background ARGB, control mode, scaling mode, is a URL,
// offset, length): the file is an asset in the APK (StreamingAssets) or a path. 1: it plays.
int video_player_start(const char *file, int bg, int control, int scaling, int is_url, int offset, int length) {
    if (!video_player_game() || !file || vp.active) return 0;
    if (is_url) {
        droid_log("video player: %s is a URL: not played", file);
        return 0;
    }
    vp.fd = -1;
    vp.mem = NULL;
    vp.pos = 0;
    char name[512];
    snprintf(name, sizeof(name), "assets/%s", file[0] == '/' ? file + 1 : file);
    size_t size = 0;
    if (file[0] != '/' && (vp.mem = apk_load(name, &size))) {
        vp.mem_size = size;
        vp.base = 0;
        vp.length = (int64_t)size;
    } else if ((vp.fd = open(file, O_RDONLY)) >= 0) {
        vp.base = offset > 0 ? offset : 0;
        vp.length = length > 0 ? length : lseek(vp.fd, 0, SEEK_END) - vp.base;
    } else {
        droid_log("video player: %s not found (asset or file)", file);
        return 0;
    }
    vp.bg = (uint32_t)bg;
    vp.control = control;
    vp.scaling = scaling;
    vp.skip = vp.over = 0;
    vp.fresh = 0;
    vp.shown = 0;
    vp.ring_r = vp.ring_w = 0;
    vp.audio_eof = 0;
    droid_log("video player: %s (%lld bytes), control %d, scaling %d, background %#x", file, (long long)vp.length,
              control, scaling, (unsigned)bg);
    vp.active = 1;
    pthread_create(&vp.decode_thread, NULL, decode_main, NULL);
    return 1;
}

// unity_host.c, each frame while it plays: the pad and the touch screen skip it (Unity's
// control modes Full 0, Minimal 1 and CancelOnInput 2; Hidden 3 can't be skipped)
void video_player_input(const droid_input_event *e) {
    if (!vp.active || vp.control == 3 || !e) return;
    const int key_down = e->type == 1 && e->action == 0;
    const int touch_down = (e->type == 2 && e->action == 0 && (e->source & 0x1002) == 0x1002) || e->type == DROID_EVENT_TOUCH;
    if (key_down || touch_down) {
        if (!vp.skip) droid_log("video player: skipped by the player");
        vp.skip = 1;
    }
}

// ---- the picture on Unity's render thread, in its swap -----------------------------------
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
#define G(ret, name, args) static ret(*name) args;
G(void, gGetIntegerv, (GLenum, GLint *)) G(void, gGetFloatv, (GLenum, GLfloat *)) G(void, gGetBooleanv, (GLenum, GLboolean *))
G(GLboolean, gIsEnabled, (GLenum)) G(void, gEnable, (GLenum)) G(void, gDisable, (GLenum))
G(void, gUseProgram, (GLuint)) G(void, gActiveTexture, (GLenum)) G(void, gBindTexture, (GLenum, GLuint))
G(void, gBindBuffer, (GLenum, GLuint)) G(void, gBindVertexArray, (GLuint)) G(void, gBindFramebuffer, (GLenum, GLuint))
G(void, gBindSampler, (GLuint, GLuint)) G(void, gViewport, (GLint, GLint, GLsizei, GLsizei))
G(void, gColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) G(void, gClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
G(void, gClear, (GLuint)) G(void, gPixelStorei, (GLenum, GLint))
G(void, gTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
G(void, gTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
G(void, gTexParameteri, (GLenum, GLenum, GLint)) G(void, gGenTextures, (GLsizei, GLuint *))
G(void, gGenBuffers, (GLsizei, GLuint *)) G(void, gGenVertexArrays, (GLsizei, GLuint *))
G(void, gBufferData, (GLenum, long, const void *, GLenum)) G(void, gBufferSubData, (GLenum, long, long, const void *))
G(void, gVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
G(void, gEnableVertexAttribArray, (GLuint)) G(void, gDrawArrays, (GLenum, GLint, GLsizei))
G(GLuint, gCreateShader, (GLenum)) G(void, gShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
G(void, gCompileShader, (GLuint)) G(GLuint, gCreateProgram, (void)) G(void, gAttachShader, (GLuint, GLuint))
G(void, gBindAttribLocation, (GLuint, GLuint, const char *)) G(void, gLinkProgram, (GLuint))
G(GLint, gGetUniformLocation, (GLuint, const char *)) G(void, gUniform1i, (GLint, GLint))
G(void, gGetProgramiv, (GLuint, GLenum, GLint *)) G(void, gUniform1f, (GLint, GLfloat))
G(unsigned, eQuerySurface, (void *, void *, GLint, GLint *))
#undef G

static int gl_ready(void) {
    static int state; // 0 not tried, 1 ok, -1 missing
    if (state) return state > 0;
#define R(var, n) if (!(*(void **)&var = egl_resolve(n))) { droid_log("video player: no %s", n); state = -1; return 0; }
    R(gGetIntegerv, "glGetIntegerv") R(gGetFloatv, "glGetFloatv") R(gGetBooleanv, "glGetBooleanv")
    R(gIsEnabled, "glIsEnabled") R(gEnable, "glEnable") R(gDisable, "glDisable") R(gUseProgram, "glUseProgram")
    R(gActiveTexture, "glActiveTexture") R(gBindTexture, "glBindTexture") R(gBindBuffer, "glBindBuffer")
    R(gBindVertexArray, "glBindVertexArray") R(gBindFramebuffer, "glBindFramebuffer") R(gBindSampler, "glBindSampler")
    R(gViewport, "glViewport") R(gColorMask, "glColorMask") R(gClearColor, "glClearColor") R(gClear, "glClear")
    R(gPixelStorei, "glPixelStorei") R(gTexImage2D, "glTexImage2D") R(gTexSubImage2D, "glTexSubImage2D")
    R(gTexParameteri, "glTexParameteri") R(gGenTextures, "glGenTextures") R(gGenBuffers, "glGenBuffers")
    R(gGenVertexArrays, "glGenVertexArrays") R(gBufferData, "glBufferData") R(gBufferSubData, "glBufferSubData")
    R(gVertexAttribPointer, "glVertexAttribPointer") R(gEnableVertexAttribArray, "glEnableVertexAttribArray")
    R(gDrawArrays, "glDrawArrays") R(gCreateShader, "glCreateShader") R(gShaderSource, "glShaderSource")
    R(gCompileShader, "glCompileShader") R(gCreateProgram, "glCreateProgram") R(gAttachShader, "glAttachShader")
    R(gBindAttribLocation, "glBindAttribLocation") R(gLinkProgram, "glLinkProgram")
    R(gGetUniformLocation, "glGetUniformLocation") R(gUniform1i, "glUniform1i") R(gGetProgramiv, "glGetProgramiv") R(gUniform1f, "glUniform1f")
    R(eQuerySurface, "eglQuerySurface")
#undef R
    state = 1;
    return 1;
}

#define V_TEXTURE_2D 0x0DE1
#define V_ARRAY_BUFFER 0x8892
#define V_FRAMEBUFFER 0x8D40
static GLuint prog, tex[3], vbo, vao;
static int tex_w, tex_h;
static GLint u_full;

static int gl_make(void) {
    if (prog) return 1;
    static const char *vs_src =
        "attribute vec2 p;\n"
        "attribute vec2 t;\n"
        "varying vec2 v;\n"
        "void main() { v = t; gl_Position = vec4(p, 0.0, 1.0); }\n";
    // Y, U, V (one byte each, in .r) -> RGB, BT.601; limited range unless the film is full range
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec2 v;\n"
        "uniform sampler2D sy;\n"
        "uniform sampler2D su;\n"
        "uniform sampler2D sv;\n"
        "uniform float full;\n"
        "void main() {\n"
        "  float y = texture2D(sy, v).r, u = texture2D(su, v).r - 0.5, w = texture2D(sv, v).r - 0.5;\n"
        "  if (full < 0.5) { y = (y - 0.0625) * 1.164; u *= 1.138; w *= 1.138; }\n"
        "  gl_FragColor = vec4(y + 1.402 * w, y - 0.344 * u - 0.714 * w, y + 1.772 * u, 1.0);\n"
        "}\n";
    GLuint vs = gCreateShader(0x8B31), fs = gCreateShader(0x8B30);
    gShaderSource(vs, 1, &vs_src, NULL);
    gShaderSource(fs, 1, &fs_src, NULL);
    gCompileShader(vs);
    gCompileShader(fs);
    prog = gCreateProgram();
    gAttachShader(prog, vs);
    gAttachShader(prog, fs);
    gBindAttribLocation(prog, 0, "p");
    gBindAttribLocation(prog, 1, "t");
    gLinkProgram(prog);
    GLint linked = 0;
    gGetProgramiv(prog, 0x8B82, &linked);
    if (!linked) {
        droid_log("video player: its shader did not link");
        prog = 0;
        return 0;
    }
    u_full = gGetUniformLocation(prog, "full");
    gGenTextures(3, tex);
    gGenBuffers(1, &vbo);
    gGenVertexArrays(1, &vao);
    gBindVertexArray(vao);
    gBindBuffer(V_ARRAY_BUFFER, vbo);
    gBufferData(V_ARRAY_BUFFER, 16 * sizeof(float), NULL, 0x88E8); // DYNAMIC_DRAW
    gVertexAttribPointer(0, 2, 0x1406, 0, 4 * sizeof(float), (const void *)0);
    gVertexAttribPointer(1, 2, 0x1406, 0, 4 * sizeof(float), (const void *)(2 * sizeof(float)));
    gEnableVertexAttribArray(0);
    gEnableVertexAttribArray(1);
    return 1;
}

// one picture on framebuffer 0 (the GL state already saved by the caller)
static void gl_draw(int sw, int sh) {
    gBindFramebuffer(V_FRAMEBUFFER, 0);
    gViewport(0, 0, sw, sh);
    gDisable(0x0C11); // scissor
    gDisable(0x0BE2); // blend
    gDisable(0x0B71); // depth
    gDisable(0x0B44); // cull
    gDisable(0x0B90); // stencil
    gColorMask(1, 1, 1, 1);
    gClearColor(((vp.bg >> 16) & 255) / 255.0f, ((vp.bg >> 8) & 255) / 255.0f, (vp.bg & 255) / 255.0f, 1.0f);
    gClear(0x4000);
    if (!gl_make()) return;
    gUseProgram(prog);
    // the newest picture taken out (its buffer and ours swap places): uploaded with no lock held,
    // so the decoder never waits for the upload (38 ms a picture on the PC's Mesa-on-D3D12)
    static uint8_t *front;
    static size_t front_cap;
    pthread_mutex_lock(&vp.lock);
    const int w = vp.w, h = vp.h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int fresh = vp.fresh && vp.rgba;
    if (fresh) {
        uint8_t *mine = front;
        const size_t mine_cap = front_cap;
        front = vp.rgba;
        front_cap = vp.rgba_cap;
        vp.rgba = mine;
        vp.rgba_cap = mine ? mine_cap : 0;
        vp.fresh = 0;
    }
    const int full = vp.full_range;
    pthread_mutex_unlock(&vp.lock);
    const double u0 = vp_now();
    if (fresh) {
        gBindBuffer(0x88EC, 0); // no unpack buffer
        gPixelStorei(0x0CF5, 1);
        gPixelStorei(0x0CF2, 0);
    }
    const uint8_t *plane[3] = { front, front + (size_t)w * h, front + (size_t)w * h + (size_t)cw * ch };
    for (int p = 0; p < 3; p++) {
        gActiveTexture(0x84C0 + p);
        gBindSampler(p, 0);
        gBindTexture(V_TEXTURE_2D, tex[p]);
        if (!fresh) continue;
        const int pw = p ? cw : w, ph = p ? ch : h;
        if (tex_w != w || tex_h != h) { // R8 (one byte, read as .r)
            gTexImage2D(V_TEXTURE_2D, 0, 0x8229, pw, ph, 0, 0x1903, 0x1401, plane[p]);
            gTexParameteri(V_TEXTURE_2D, 0x2801, 0x2601);
            gTexParameteri(V_TEXTURE_2D, 0x2800, 0x2601);
            gTexParameteri(V_TEXTURE_2D, 0x2802, 0x812F);
            gTexParameteri(V_TEXTURE_2D, 0x2803, 0x812F);
        } else {
            gTexSubImage2D(V_TEXTURE_2D, 0, 0, 0, pw, ph, 0x1903, 0x1401, plane[p]);
        }
    }
    if (fresh) {
        tex_w = w;
        tex_h = h;
        vp.shown++;
        vp.upload_s += vp_now() - u0;
        if (vp.shown % 250 == 0)
            droid_log("video player: %lu pictures uploaded, %.1f ms each", vp.shown, vp.upload_s * 1000 / vp.shown);
    }
    gUniform1i(gGetUniformLocation(prog, "sy"), 0);
    gUniform1i(gGetUniformLocation(prog, "su"), 1);
    gUniform1i(gGetUniformLocation(prog, "sv"), 2);
    if (u_full >= 0 && gUniform1f) gUniform1f(u_full, full ? 1.0f : 0.0f);
    if (!tex_w || !tex_h) return;
    // the scaling: None 0 (its own size), AspectFit 1, AspectFill 2, Fill 3
    float sx = 1, sy = 1;
    const float va = (float)tex_w / tex_h, sa = (float)sw / sh;
    if (vp.scaling == 1) (va > sa) ? (sy = sa / va) : (sx = va / sa);
    else if (vp.scaling == 2) (va > sa) ? (sx = va / sa) : (sy = sa / va);
    else if (vp.scaling == 0) sx = (float)tex_w / sw, sy = (float)tex_h / sh;
    const float q[16] = { -sx, -sy, 0, 1, sx, -sy, 1, 1, -sx, sy, 0, 0, sx, sy, 1, 0 }; // row 0 on top
    gBindVertexArray(vao);
    gBindBuffer(V_ARRAY_BUFFER, vbo);
    gBufferSubData(V_ARRAY_BUFFER, 0, sizeof(q), q);
    gDrawArrays(0x0005, 0, 4); // a strip
}

// The game's eglSwapBuffers (pc/egl_pc.c, gles_bridge.c), on its render thread: while a video
// plays, the frames are the video's, swapped by `swap`; 1 when it did the swapping.
typedef unsigned (*vp_swap_fn)(void *dpy, void *surf);
int video_player_swap(void *dpy, void *surf, vp_swap_fn swap) {
    if (!vp.active || !gl_ready()) return 0;
    // what Unity had set: given back after the video
    GLint program, active, tex2d[3], array_buf, vao_b, fbo, view[4], sampler[3], unpack, align, row;
    GLfloat clear[4];
    GLboolean mask[4];
    const GLenum caps[] = { 0x0C11, 0x0BE2, 0x0B71, 0x0B44, 0x0B90 };
    GLboolean on[5];
    gGetIntegerv(0x8B8D, &program);
    gGetIntegerv(0x84E0, &active);
    for (int u = 0; u < 3; u++) { // the units the video uses: 0 Y, 1 U, 2 V
        gActiveTexture(0x84C0 + u);
        gGetIntegerv(0x8069, &tex2d[u]);
        gGetIntegerv(0x8919, &sampler[u]);
    }
    gGetIntegerv(0x8894, &array_buf);
    gGetIntegerv(0x85B5, &vao_b);
    gGetIntegerv(0x8CA6, &fbo);
    gGetIntegerv(0x0BA2, view);
    gGetIntegerv(0x88EF, &unpack);
    gGetIntegerv(0x0CF5, &align);
    gGetIntegerv(0x0CF2, &row);
    gGetFloatv(0x0C22, clear);
    gGetBooleanv(0x0C23, mask);
    for (int i = 0; i < 5; i++) on[i] = gIsEnabled(caps[i]);
    GLint sw = 1920, sh = 1080;
    eQuerySurface(dpy, surf, 0x3057, &sw); // EGL_WIDTH, EGL_HEIGHT
    eQuerySurface(dpy, surf, 0x3056, &sh);
    droid_log("video player: on the render thread, %dx%d", sw, sh);
    double last = vp_now();
    while (!vp.over) {
        gl_draw(sw, sh);
        swap(dpy, surf);
        const double now = vp_now(), left = 1.0 / 60 - (now - last); // (60 at most, vsync or not)
        if (left > 0.001) usleep((useconds_t)(left * 1e6));
        last = vp_now();
    }
    pthread_join(vp.decode_thread, NULL);
    droid_log("video player: %lu pictures shown; the game goes on", vp.shown);
    gBindVertexArray((GLuint)vao_b);
    gBindBuffer(V_ARRAY_BUFFER, (GLuint)array_buf);
    gBindBuffer(0x88EC, (GLuint)unpack);
    gPixelStorei(0x0CF5, align);
    gPixelStorei(0x0CF2, row);
    for (int u = 0; u < 3; u++) {
        gActiveTexture(0x84C0 + u);
        gBindTexture(V_TEXTURE_2D, (GLuint)tex2d[u]);
        gBindSampler(u, (GLuint)sampler[u]);
    }
    gActiveTexture((GLenum)active);
    gUseProgram((GLuint)program);
    gBindFramebuffer(V_FRAMEBUFFER, (GLuint)fbo);
    gViewport(view[0], view[1], view[2], view[3]);
    gClearColor(clear[0], clear[1], clear[2], clear[3]);
    gColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 5; i++) (on[i] ? gEnable : gDisable)(caps[i]);
    free(vp.mem);
    vp.mem = NULL;
    if (vp.fd >= 0) close(vp.fd);
    vp.fd = -1;
    vp.active = 0;
    return 1;
}
