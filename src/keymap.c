// keymap.c — a game's keymap (keymap.h): touchmap.txt, and the play for hosts that have no
// touch map of their own: a mapped button holds a finger on its spot, the left stick drags a
// finger around the joystick's centre (down at the centre first, then out over three moves:
// a floating joystick places itself where the touch lands).

#include "keymap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "android_shim.h"
#include "game_settings.h"

volatile int droid_keymap_version = 1;

int keymap_read(keymap_t *k) {
    memset(k, 0, sizeof(*k));
    FILE *touchmap_open_read(void); // game_settings.c: the game's, else the app's shipped one
    FILE *f = touchmap_open_read();
    if (!f) return 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float x, y, r;
        int code;
        if (sscanf(line, "stick2 %f %f %f", &x, &y, &r) == 3) {
            k->stick2_x = x, k->stick2_y = y, k->stick2_r = r;
        } else if (sscanf(line, "stick %f %f %f", &x, &y, &r) == 3) {
            k->stick_x = x, k->stick_y = y, k->stick_r = r;
        } else if (sscanf(line, "button %d %f %f", &code, &x, &y) == 3 && k->count < KEYMAP_MAX) {
            k->button[k->count].keycode = code;
            k->button[k->count].x = x, k->button[k->count].y = y;
            k->count++;
        }
    }
    fclose(f);
    return 1;
}

int keymap_write(const keymap_t *k) {
    char path[300];
    FILE *f = fopen(game_path(path, sizeof(path), "touchmap.txt"), "w");
    if (!f) {
        droid_log("keymap: touchmap.txt not written");
        return -1;
    }
    fprintf(f, "# made in Keymap Editor (hold Options in the game > Keymap Editor)\n");
    if (k->stick_r > 0) fprintf(f, "stick %.0f %.0f %.0f\n", k->stick_x, k->stick_y, k->stick_r);
    if (k->stick2_r > 0) fprintf(f, "stick2 %.0f %.0f %.0f\n", k->stick2_x, k->stick2_y, k->stick2_r);
    for (int i = 0; i < k->count; i++) fprintf(f, "button %d %.0f %.0f\n", k->button[i].keycode, k->button[i].x, k->button[i].y);
    fclose(f);
    droid_keymap_version++;
    droid_log("keymap: saved (joysticks: left %s, right %s; %d buttons)", k->stick_r > 0 ? "yes" : "no",
              k->stick2_r > 0 ? "yes" : "no", k->count);
    return 0;
}

// ---- the play
static keymap_t km;
static int km_version;
typedef struct { int down, moves; float x, y; } stick_play;
static stick_play left_play, right_play;

void keymap_sync(void) {
    if (km_version == droid_keymap_version) return;
    km_version = droid_keymap_version;
    if (keymap_read(&km))
        droid_log("keymap: joystick %s, %d buttons (touchmap.txt)", km.stick_r > 0 ? "yes" : "no", km.count);
}

int keymap_has_stick(void) { return km.stick_r > 0; }
int keymap_has_stick2(void) { return km.stick2_r > 0; }
int keymap_has_key(int keycode) {
    for (int i = 0; i < km.count; i++)
        if (km.button[i].keycode == keycode) return 1;
    return 0;
}
void keymap_triggers(float l2, float r2, keymap_finger_fn f) {
    static int was_l2, was_r2;
    const int dl = l2 > 0.5f, dr = r2 > 0.5f;
    if (dl != was_l2) keymap_key(KEY_L2, dl, f), was_l2 = dl;
    if (dr != was_r2) keymap_key(KEY_R2, dr, f), was_r2 = dr;
}

int keymap_key(int keycode, int down, keymap_finger_fn f) {
    for (int i = 0; i < km.count; i++)
        if (km.button[i].keycode == keycode) {
            f(1 + i, down, km.button[i].x, km.button[i].y);
            return 1;
        }
    return 0;
}

void keymap_stick(float lx, float ly) { left_play.x = lx, left_play.y = ly; }
void keymap_stick2(float rx, float ry) { right_play.x = rx, right_play.y = ry; }

static void stick_tick(stick_play *p, int id, float cx, float cy, float r, keymap_finger_fn f) {
    if (r <= 0) return;
    const float m = sqrtf(p->x * p->x + p->y * p->y);
    if (m < 0.2f) {
        if (p->down) f(id, 0, cx, cy);
        p->down = 0;
        return;
    }
    if (!p->down) { // the thumb lands on the centre first
        p->down = 1, p->moves = 0;
        f(id, 1, cx, cy);
        return;
    }
    if (p->moves < 3) p->moves++;
    float reach = (m > 1 ? 1 : m) * p->moves / 3.0f;
    f(id, 1, cx + p->x / m * r * reach, cy + p->y / m * r * reach);
}

void keymap_tick(keymap_finger_fn f) {
    stick_tick(&left_play, 0, km.stick_x, km.stick_y, km.stick_r, f);
    stick_tick(&right_play, KEYMAP_STICK2_ID, km.stick2_x, km.stick2_y, km.stick2_r, f);
}
