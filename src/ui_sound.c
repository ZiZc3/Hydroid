// ui_sound.c — the menus' sounds (ui_sound.h): a sound port of its own (48 kHz, 16-bit
// stereo, as android_shim.c's AAudio opens them) and a thread mixing up to 8 voices of the
// built-in sound (ui_sounds_data.h).

#include "ui_sound.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_shim.h"
#include "ui_sounds_data.h"

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);
int sceAudioOutClose(int32_t handle);

#define GRAIN 256 // frames a block (~5 ms)
#define VOICES 8
static struct {
    const int16_t *pcm;
    int frames, at;
    float gain;
} voices[VOICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int port = -1;
static volatile int running;

static void *mix_thread(void *arg) {
    (void)arg;
    int16_t out[GRAIN * 2];
    float acc[GRAIN * 2];
    while (running) {
        memset(acc, 0, sizeof(acc));
        pthread_mutex_lock(&lock);
        for (int v = 0; v < VOICES; v++) {
            if (!voices[v].pcm) continue;
            int n = voices[v].frames - voices[v].at;
            if (n > GRAIN) n = GRAIN;
            const int16_t *p = voices[v].pcm + voices[v].at;
            for (int i = 0; i < n; i++) {
                float s = p[i] * voices[v].gain;
                acc[2 * i] += s, acc[2 * i + 1] += s;
            }
            voices[v].at += n;
            if (voices[v].at >= voices[v].frames) voices[v].pcm = NULL;
        }
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < GRAIN * 2; i++) {
            float s = acc[i];
            out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        sceAudioOutOutput(port, out); // blocks for one block: the clock
    }
    return NULL;
}

int ui_sound_open(void) {
    if (port >= 0) return 0;
    sceAudioOutInit(); // (again if the games' audio did already: harmless)
    port = sceAudioOutOpen(0xff, 0, 0, GRAIN, 48000, 1);
    if (port < 0) {
        droid_log("ui: no sound port (%#x)", port);
        return -1;
    }
    running = 1;
    if (pthread_create(&thread, NULL, mix_thread, NULL) != 0) {
        running = 0;
        sceAudioOutClose(port);
        port = -1;
        return -1;
    }
    return 0;
}

void ui_sound_close(void) {
    if (port < 0) return;
    running = 0;
    pthread_join(thread, NULL);
    sceAudioOutClose(port);
    port = -1;
    memset(voices, 0, sizeof(voices));
}

void ui_sound_play(int id, float gain) {
    if (port < 0 || id < 0 || id >= (int)(sizeof(ui_sounds) / sizeof(ui_sounds[0]))) return;
    pthread_mutex_lock(&lock);
    int v = 0, oldest = 0;
    for (; v < VOICES && voices[v].pcm; v++)
        if (voices[v].at > voices[oldest].at) oldest = v;
    if (v == VOICES) v = oldest; // all busy: the one furthest along makes room
    voices[v].pcm = ui_sounds[id].pcm;
    voices[v].frames = ui_sounds[id].frames;
    voices[v].at = 0;
    voices[v].gain = gain;
    pthread_mutex_unlock(&lock);
}
