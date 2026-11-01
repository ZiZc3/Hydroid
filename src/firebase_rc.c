// firebase_rc.c — Firebase Remote Config as a phone has it: the game's live settings from Google's
// servers, for the games on the list (FRC_GAMES). Without them a game takes the defaults it ships,
// and those can be placeholders: Temple Run 1's Global Challenges lines are "on" with no dates
// until Remote Config sends the real ones (all finished), so on Hydroid an empty challenge sat
// over the menu and PLAY took no press (found by comparing with the game on LDPlayer).
// The same two requests the game's Firebase SDK makes, with the game's own ids (from its APK):
//   Firebase Installations: an installation id and its token
//   Remote Config fetch: the entries (key -> text)
// The answer is kept in the game folder (firebase_rc.json): the next launch has it from its first
// call, and offline too; a fresh one is fetched in the background each launch.
// Made with the user's permission to send the game's own Firebase requests (2026-10-27).
#include "firebase_rc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apk_sign.h"
#include "game_settings.h"
#include "https.h"

void droid_log(const char *fmt, ...);
void *apk_load(const char *name, size_t *size); // apk.c
extern const char *java_package;                 // jni_impl.c

// the games whose Remote Config is fetched (exact package names)
static const char *const FRC_GAMES[] = { "com.imangi.templerun", NULL };

static struct {
    char api_key[64], app_id[96], project_id[96], project_number[32];
} ids;

typedef struct { char *k, *v; } entry;
typedef struct { entry *e; int n, cap; } table;
static table active, fresh; // what the game reads / the newest fetch
static int enabled, fetched_ok, fetch_done;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static char app_version[64];

static void table_free(table *t) {
    for (int i = 0; i < t->n; i++) free(t->e[i].k), free(t->e[i].v);
    free(t->e);
    memset(t, 0, sizeof(*t));
}
static void table_add(table *t, char *k, char *v) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->e = realloc(t->e, sizeof(entry) * (size_t)t->cap);
    }
    t->e[t->n].k = k, t->e[t->n].v = v;
    t->n++;
}

// ---- a little JSON: the "entries" object's string members ---------------------------------
static const char *json_string(const char *p, char **out) { // p at the opening quote
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, n = 0;
    char *s = malloc(cap);
    while (*p && *p != '"') {
        if (n + 8 >= cap) s = realloc(s, cap *= 2);
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned u = (unsigned)strtoul((char[5]){ p[0], p[1], p[2], p[3], 0 }, NULL, 16);
                p += 4;
                if (u < 0x80) s[n++] = (char)u;
                else if (u < 0x800) s[n++] = (char)(0xc0 | u >> 6), s[n++] = (char)(0x80 | (u & 63));
                else s[n++] = (char)(0xe0 | u >> 12), s[n++] = (char)(0x80 | ((u >> 6) & 63)), s[n++] = (char)(0x80 | (u & 63));
                continue;
            }
            default: break; // \" \\ \/
            }
        }
        s[n++] = c;
    }
    s[n] = 0;
    *out = s;
    return *p == '"' ? p + 1 : NULL;
}
static int parse_entries(const char *json, table *t) {
    const char *p = json ? strstr(json, "\"entries\"") : NULL;
    if (!p || !(p = strchr(p, '{'))) return 0;
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '"') break;
        char *k = NULL, *v = NULL;
        if (!(p = json_string(p, &k))) return t->n;
        while (*p == ' ' || *p == ':') p++;
        if (!(p = json_string(p, &v))) {
            free(k);
            return t->n;
        }
        table_add(t, k, v);
    }
    return t->n;
}
// a "name": "value" in a JSON text (google-services-desktop.json), first one
static int json_field(const char *json, const char *name, char *out, size_t n) {
    char want[64];
    snprintf(want, sizeof(want), "\"%s\"", name);
    const char *p = strstr(json, want);
    if (!p || !(p = strchr(p + strlen(want), '"'))) return 0;
    const char *e = strchr(p + 1, '"');
    if (!e || (size_t)(e - p - 1) >= n) return 0;
    snprintf(out, n, "%.*s", (int)(e - p - 1), p + 1);
    return 1;
}

// ---- the game's Firebase ids: assets/google-services-desktop.json (Unity's Firebase plugin) --
static int find_ids(void) {
    size_t n = 0;
    char *j = apk_load("assets/google-services-desktop.json", &n);
    if (j) {
        char *t = malloc(n + 1);
        memcpy(t, j, n);
        t[n] = 0;
        free(j);
        json_field(t, "current_key", ids.api_key, sizeof(ids.api_key));
        json_field(t, "mobilesdk_app_id", ids.app_id, sizeof(ids.app_id));
        json_field(t, "project_id", ids.project_id, sizeof(ids.project_id));
        json_field(t, "project_number", ids.project_number, sizeof(ids.project_number));
        free(t);
    }
    if (!ids.project_number[0] && ids.app_id[0]) sscanf(ids.app_id, "1:%31[0-9]:", ids.project_number);
    return ids.api_key[0] && ids.app_id[0] && ids.project_id[0] && ids.project_number[0];
}

// ---- the requests ---------------------------------------------------------------------------
static void headers(char *h, size_t n, const char *cert) {
    snprintf(h, n, "Content-Type: application/json\r\nX-Goog-Api-Key: %s\r\nX-Android-Package: %s\r\nX-Android-Cert: %s\r\n",
             ids.api_key, java_package, cert);
}

// the installation (Firebase Installations): its id and token, kept in firebase_install.txt
// ("fid token expiry") while the token lasts
static int installation(const char *cert, char *fid, size_t fn, char *token, size_t tn) {
    char path[600];
    game_path(path, sizeof(path), "firebase_install.txt");
    FILE *f = fopen(path, "r");
    if (f) {
        long long until = 0;
        char a[64] = "", b[2048] = "";
        if (fscanf(f, "%63s %2047s %lld", a, b, &until) == 3 && until > (long long)time(NULL) + 3600) {
            fclose(f);
            snprintf(fid, fn, "%s", a), snprintf(token, tn, "%s", b);
            return 1;
        }
        fclose(f);
    }
    unsigned char r[17];
    void arc4random_buf(void *buf, size_t n);
    arc4random_buf(r, sizeof(r));
    r[0] = (unsigned char)(0x70 | (r[0] & 0x0f)); // the FID's form (its first 4 bits 0111)
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char id[24];
    for (int i = 0; i < 22; i++) { // 22 base64url characters of the 17 bytes
        const int bit = i * 6, by = bit / 8, sh = bit % 8;
        const unsigned v = ((unsigned)r[by] << 8 | (by + 1 < 17 ? r[by + 1] : 0)) >> (10 - sh);
        id[i] = b64[v & 63];
    }
    id[22] = 0;
    char h[512], body[512], url[160];
    headers(h, sizeof(h), cert);
    snprintf(body, sizeof(body), "{\"fid\":\"%s\",\"appId\":\"%s\",\"authVersion\":\"FIS_v2\",\"sdkVersion\":\"a:17.2.0\"}",
             id, ids.app_id);
    snprintf(url, sizeof(url), "/v1/projects/%s/installations", ids.project_id);
    int st = 0;
    char *resp = https_post("firebaseinstallations.googleapis.com", url, h, body, &st);
    char got_fid[64] = "", tok[2048] = "", exp[32] = "";
    if (resp && st == 200) {
        json_field(resp, "fid", got_fid, sizeof(got_fid));
        const char *at = strstr(resp, "\"authToken\"");
        if (at) json_field(at, "token", tok, sizeof(tok)), json_field(at, "expiresIn", exp, sizeof(exp));
    }
    if (!got_fid[0] || !tok[0]) {
        droid_log("firebase: installation failed (HTTP %d) %.200s", st, resp ? resp : "");
        free(resp);
        return 0;
    }
    free(resp);
    const long long until = (long long)time(NULL) + atoll(exp);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%s %s %lld\n", got_fid, tok, until);
        fclose(f);
    }
    snprintf(fid, fn, "%s", got_fid), snprintf(token, tn, "%s", tok);
    return 1;
}

static void *fetch_thread(void *arg) {
    (void)arg;
    char cert[41], fid[64], token[2048];
    char *resp = NULL;
    int st = 0;
    if (apk_cert_sha1_hex(cert) != 0) {
        droid_log("firebase: no signing certificate: Remote Config not fetched");
    } else if (installation(cert, fid, sizeof(fid), token, sizeof(token))) {
        char h[512], url[160], body[4096];
        headers(h, sizeof(h), cert);
        snprintf(url, sizeof(url), "/v1/projects/%s/namespaces/firebase:fetch", ids.project_number);
        snprintf(body, sizeof(body),
                 "{\"appInstanceId\":\"%s\",\"appInstanceIdToken\":\"%s\",\"appId\":\"%s\",\"countryCode\":\"US\","
                 "\"languageCode\":\"en-US\",\"platformVersion\":\"30\",\"timeZone\":\"UTC\",\"appVersion\":\"%s\","
                 "\"packageName\":\"%s\",\"sdkVersion\":\"22.0.0\",\"analyticsUserProperties\":{}}",
                 fid, token, ids.app_id, app_version, java_package);
        resp = https_post("firebaseremoteconfig.googleapis.com", url, h, body, &st);
    }
    table t = { 0 };
    const int n = resp && st == 200 ? parse_entries(resp, &t) : 0;
    if (n > 0) {
        char path[600];
        FILE *f = fopen(game_path(path, sizeof(path), "firebase_rc.json"), "w");
        if (f) {
            fputs(resp, f);
            fclose(f);
        }
        droid_log("firebase: Remote Config fetched: %d values (kept for the next launch)", n);
    } else if (resp || st) {
        droid_log("firebase: Remote Config fetch failed (HTTP %d) %.200s", st, resp ? resp : "");
    }
    free(resp);
    pthread_mutex_lock(&lock);
    if (n > 0) {
        table_free(&fresh);
        fresh = t;
        fetched_ok = 1;
        if (!active.n) { // nothing kept from before: the game reads these
            active = fresh;
            memset(&fresh, 0, sizeof(fresh));
        }
    }
    fetch_done = 1;
    pthread_cond_broadcast(&done_cv);
    pthread_mutex_unlock(&lock);
    return NULL;
}

void frc_start(const char *version) {
    enabled = 0;
    for (int i = 0; FRC_GAMES[i]; i++) enabled |= java_package && !strcmp(java_package, FRC_GAMES[i]);
    if (!enabled) return;
    if (!find_ids()) {
        droid_log("firebase: the game's Firebase ids not found: Remote Config as before (its defaults)");
        enabled = 0;
        return;
    }
    snprintf(app_version, sizeof(app_version), "%s", version && *version ? version : "1.0");
    // last launch's answer: the game reads it from its first call
    char path[600];
    FILE *f = fopen(game_path(path, sizeof(path), "firebase_rc.json"), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *j = n > 0 ? malloc((size_t)n + 1) : NULL;
        if (j && fread(j, 1, (size_t)n, f) == (size_t)n) {
            j[n] = 0;
            parse_entries(j, &active);
        }
        free(j);
        fclose(f);
    }
    droid_log("firebase: Remote Config for %s (project %s): %d values kept from last time, a fresh fetch started",
              java_package, ids.project_id, active.n);
    pthread_t th;
    if (pthread_create(&th, NULL, fetch_thread, NULL) == 0) pthread_detach(th);
    else fetch_done = 1;
}

int frc_enabled(void) { return enabled; }

// the game's fetch: done when the background one is (waits up to ms for it)
int frc_wait(int ms) {
    if (!enabled) return 0;
    pthread_mutex_lock(&lock);
    if (!fetch_done) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += ms / 1000;
        t.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
        while (!fetch_done && pthread_cond_timedwait(&done_cv, &lock, &t) == 0) {
        }
    }
    const int ok = fetched_ok || active.n > 0;
    pthread_mutex_unlock(&lock);
    return ok;
}

// the game's activate: the newest fetch becomes what it reads; 1 when that changed anything
int frc_activate(void) {
    if (!enabled) return 0;
    pthread_mutex_lock(&lock);
    int changed = 0;
    if (fresh.n) {
        table_free(&active);
        active = fresh;
        memset(&fresh, 0, sizeof(fresh));
        changed = 1;
    }
    pthread_mutex_unlock(&lock);
    return changed;
}

// the value the game reads for a key (Google's), or NULL (the game's default then)
const char *frc_value(const char *key) {
    if (!enabled || !key) return NULL;
    const char *v = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < active.n && !v; i++)
        if (!strcmp(active.e[i].k, key)) v = active.e[i].v;
    pthread_mutex_unlock(&lock);
    return v;
}

int frc_count(void) { return enabled ? active.n : 0; }
const char *frc_key(int i) { return enabled && i >= 0 && i < active.n ? active.e[i].k : NULL; }
