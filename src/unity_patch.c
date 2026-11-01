// unity_patch.c — Vulkan first for every Unity game. A Unity build lists its graphics APIs in
// order in assets/bin/Data/globalgamemanagers (BuildSettings.m_GraphicsAPIs: 11 OpenGL ES 3,
// 8 OpenGL ES 2, 21 Vulkan) and takes the first that starts; Subway Surfers lists
// [11, 8, 21], so it never tried Vulkan. On the PS5 Vulkan is the smooth one (OpenGL's
// swaps wait on flips: 0.5-1.4 s freezes). The game's APK stays as it is: a copy in the
// game's folder gets the list with Vulkan first ([21, 11, 8]); if Vulkan can't start,
// Unity still falls back to OpenGL ES by itself.
// The entry is usually compressed: the copy keeps every byte of the APK and adds the
// changed entry, stored, after the others; its central directory points there.

#include "unity_patch.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "android_shim.h"

#define GGM "assets/bin/Data/globalgamemanagers"
#define API_VULKAN 21

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v) { wr16(p, v & 0xffff); wr16(p + 2, v >> 16); }

static int pread_all(int fd, void *buf, size_t n, off_t at) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, at + (off_t)got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r <= 0) return -1;
        put += (size_t)r;
    }
    return 0;
}

typedef struct {
    unsigned char *cd; // the central directory
    uint32_t cd_size, cd_at, entries;
    uint32_t rec;      // our entry's record in it
    uint32_t method, csize, usize, local;
} zip_info;

static int read_zip(int fd, off_t file_size, zip_info *z) {
    unsigned char tail[65536 + 22];
    off_t from = file_size > (off_t)sizeof(tail) ? file_size - (off_t)sizeof(tail) : 0;
    size_t n = (size_t)(file_size - from);
    if (pread_all(fd, tail, n, from) != 0) return -1;
    long eocd = -1;
    for (long i = (long)n - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) return -1;
    z->entries = rd16(tail + eocd + 10);
    z->cd_size = rd32(tail + eocd + 12);
    z->cd_at = rd32(tail + eocd + 16);
    z->cd = malloc(z->cd_size);
    if (!z->cd || pread_all(fd, z->cd, z->cd_size, z->cd_at) != 0) return -1;
    for (uint32_t i = 0; i + 46 <= z->cd_size;) {
        if (rd32(z->cd + i) != 0x02014b50) break;
        uint32_t nl = rd16(z->cd + i + 28), xl = rd16(z->cd + i + 30), cl = rd16(z->cd + i + 32);
        if (nl == strlen(GGM) && !memcmp(z->cd + i + 46, GGM, nl)) {
            z->rec = i;
            z->method = rd16(z->cd + i + 10);
            z->csize = rd32(z->cd + i + 20);
            z->usize = rd32(z->cd + i + 24);
            z->local = rd32(z->cd + i + 42);
            return 0;
        }
        i += 46 + nl + xl + cl;
    }
    return -1; // not a Unity game
}

// The entry's bytes, uncompressed
static unsigned char *read_entry(int fd, const zip_info *z) {
    unsigned char lh[30];
    if (pread_all(fd, lh, 30, z->local) != 0 || rd32(lh) != 0x04034b50) return NULL;
    off_t data = (off_t)z->local + 30 + rd16(lh + 26) + rd16(lh + 28);
    unsigned char *raw = malloc(z->csize ? z->csize : 1), *out = malloc(z->usize ? z->usize : 1);
    if (!raw || !out || pread_all(fd, raw, z->csize, data) != 0) goto fail;
    if (z->method == 0) {
        free(out);
        return raw;
    }
    if (z->method != 8) goto fail;
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) goto fail;
    s.next_in = raw;
    s.avail_in = z->csize;
    s.next_out = out;
    s.avail_out = z->usize;
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (rc != Z_STREAM_END || s.total_out != z->usize) goto fail;
    free(raw);
    return out;
fail:
    free(raw);
    free(out);
    return NULL;
}

// The API list: a count (2-6) then that many known APIs, Vulkan among them. Its offset, or -1.
static long find_list(const unsigned char *buf, size_t n, int *first) {
    for (size_t p = 0; p + 4 <= n; p += 4) {
        uint32_t c = rd32(buf + p);
        if (c < 2 || c > 6 || p + 4 + c * 4 > n) continue;
        int ok = 1, vulkan = 0;
        for (uint32_t k = 0; k < c && ok; k++) {
            uint32_t v = rd32(buf + p + 4 + k * 4);
            ok = v == 8 || v == 11 || v == 17 || v == 21; // ES2, ES3, Metal, Vulkan
            vulkan |= v == API_VULKAN;
        }
        if (ok && vulkan) {
            *first = (int)rd32(buf + p + 4);
            return (long)p;
        }
    }
    return -1;
}

// The copy: a plain zip, every entry written again in order (its data as it is; the stored
// ones 4-byte aligned, as zipalign does), ours stored with the new list, then a new central
// directory. (Adding our entry after the APK's signing block made a zip Unity didn't mount.)
static int write_copy(int in, const zip_info *z, const unsigned char *data, const char *copy) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.part", copy);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return -1;
    unsigned char *cd = malloc(z->cd_size);
    size_t cap = 8 << 20;
    unsigned char *buf = malloc(cap);
    int rc = cd && buf ? 0 : -1;
    if (rc == 0) memcpy(cd, z->cd, z->cd_size);
    uint32_t at = 0; // where we are in the copy
    const uint32_t crc = (uint32_t)crc32(0, data, z->usize);
    for (uint32_t i = 0; rc == 0 && i + 46 <= z->cd_size;) {
        unsigned char *r = cd + i;
        if (rd32(r) != 0x02014b50) break;
        uint32_t nl = rd16(r + 28), xl = rd16(r + 30), cl = rd16(r + 32);
        int ours = i == z->rec;
        uint32_t method = ours ? 0 : rd16(r + 10);
        uint32_t csize = ours ? z->usize : rd32(r + 20);
        uint32_t usize = ours ? z->usize : rd32(r + 24);
        uint32_t ecrc = ours ? crc : rd32(r + 16);
        unsigned char lh[30];
        uint32_t old_local = rd32(r + 42);
        if (pread_all(in, lh, 30, old_local) != 0 || rd32(lh) != 0x04034b50) {
            rc = -1;
            break;
        }
        off_t old_data = (off_t)old_local + 30 + rd16(lh + 26) + rd16(lh + 28);
        // the new local header: sizes in it (no data descriptor), padding so stored data aligns
        uint32_t pad = 0;
        if (method == 0) pad = (4 - (at + 30 + nl) % 4) % 4;
        wr16(lh + 6, rd16(lh + 6) & ~8u); // no data descriptor after the data
        wr16(lh + 8, method);
        wr32(lh + 14, ecrc);
        wr32(lh + 18, csize);
        wr32(lh + 22, usize);
        wr16(lh + 26, nl);
        wr16(lh + 28, pad);
        static const unsigned char zeros[4];
        if (write_all(out, lh, 30) != 0 || write_all(out, r + 46, nl) != 0 || write_all(out, zeros, pad) != 0) {
            rc = -1;
            break;
        }
        if (ours) {
            if (write_all(out, data, usize) != 0) rc = -1;
        } else {
            for (uint32_t done = 0; rc == 0 && done < csize;) {
                size_t n = csize - done < cap ? csize - done : cap;
                if (pread_all(in, buf, n, old_data + done) != 0 || write_all(out, buf, n) != 0) rc = -1;
                done += (uint32_t)n;
            }
        }
        // its record: where it is now (and ours: stored, its size, its crc)
        wr16(r + 8, rd16(r + 8) & ~8u);
        wr16(r + 10, method);
        wr32(r + 16, ecrc);
        wr32(r + 20, csize);
        wr32(r + 24, usize);
        wr32(r + 42, at);
        at += 30 + nl + pad + csize;
        i += 46 + nl + xl + cl;
    }
    uint32_t cd_at = at;
    if (rc == 0 && write_all(out, cd, z->cd_size) != 0) rc = -1;
    unsigned char end[22];
    memset(end, 0, sizeof(end));
    wr32(end, 0x06054b50);
    wr16(end + 8, z->entries);
    wr16(end + 10, z->entries);
    wr32(end + 12, z->cd_size);
    wr32(end + 16, cd_at);
    if (rc == 0 && write_all(out, end, 22) != 0) rc = -1;
    free(cd);
    free(buf);
    if (close(out) != 0) rc = -1;
    if (rc == 0 && rename(tmp, copy) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

// ---- without a copy: the list changed as Unity unpacks it ------------------------------
// globalgamemanagers is usually deflated in the APK, and Unity unpacks it with zlib's
// inflate, ours (linux_abi.c). The list's place in the unpacked file is known: when an
// inflate's output covers it and the bytes there are the game's list, they become the
// Vulkan-first one. Nothing written, no copy.
static struct {
    int active, done;
    uint32_t at, len; // where the list is in the unpacked file, its size (count + APIs)
    unsigned char old[28], want[28];
} plan;

int unity_vulkan_plan(const char *apk) {
    plan.active = plan.done = 0;
    int fd = open(apk, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    zip_info z;
    memset(&z, 0, sizeof(z));
    unsigned char *data = NULL;
    int rc = 1;
    if (fstat(fd, &st) == 0 && read_zip(fd, st.st_size, &z) == 0 && z.usize < (64u << 20) &&
        (data = read_entry(fd, &z))) {
        int first = 0;
        long at = find_list(data, z.usize, &first);
        if (at >= 0 && first != API_VULKAN) {
            uint32_t c = rd32(data + at), n = 0, apis[6];
            apis[n++] = API_VULKAN;
            for (uint32_t k = 0; k < c; k++) {
                uint32_t v = rd32(data + at + 4 + k * 4);
                if (v != API_VULKAN) apis[n++] = v;
            }
            plan.at = (uint32_t)at;
            plan.len = 4 + c * 4;
            memcpy(plan.old, data + at, plan.len);
            memcpy(plan.want, data + at, 4);
            for (uint32_t k = 0; k < c; k++) wr32(plan.want + 4 + k * 4, apis[k]);
            if (z.method == 8) {
                plan.active = 1;
                rc = 0;
                droid_log("unity: graphics APIs had %d first: Vulkan first as Unity unpacks them (no copy)", first);
            } else {
                rc = 2; // stored: no inflate to catch it in, a copy then
            }
        } else if (at >= 0) {
            droid_log("unity: graphics APIs: Vulkan is first already");
        } else {
            droid_log("unity: graphics APIs: no Vulkan in the game's list (OpenGL ES only build)");
        }
    } else {
        droid_log("unity: graphics APIs: globalgamemanagers not read (%llu bytes)", (unsigned long long)z.usize);
    }
    free(data);
    free(z.cd);
    close(fd);
    return rc;
}

int unity_inflate(void *strm, int flush) {
    z_streamp s = strm;
    if (!plan.active || plan.done) return inflate(s, flush);
    unsigned char *out0 = s->next_out;
    uLong t0 = s->total_out;
    int rc = inflate(s, flush);
    uLong t1 = s->total_out;
    // the part of the list this call produced (all of it, usually)
    uLong from = t0 > plan.at ? t0 : plan.at, to = t1 < plan.at + plan.len ? t1 : plan.at + plan.len;
    if (from < to) {
        unsigned char *b = out0 + (from - t0);
        const unsigned char *o = plan.old + (from - plan.at), *w = plan.want + (from - plan.at);
        if (!memcmp(b, o, to - from)) { // the game's own list there: this is the file
            memcpy(b, w, to - from);
            if (to == plan.at + plan.len) {
                plan.done = 1;
                droid_log("unity: graphics APIs: Vulkan first (changed as unpacked)");
            }
        }
    }
    return rc;
}

int unity_vulkan_first(const char *apk, const char *copy) {
    // made before (a copy newer than the APK): kept
    struct stat as, cs;
    if (stat(apk, &as) != 0) return -1;
    if (stat(copy, &cs) == 0 && cs.st_mtime >= as.st_mtime && cs.st_size > 0) return 0;
    int fd = open(apk, O_RDONLY);
    if (fd < 0) return -1;
    zip_info z;
    memset(&z, 0, sizeof(z));
    int rc = 1;
    unsigned char *data = NULL;
    if (read_zip(fd, as.st_size, &z) == 0 && z.usize < (64u << 20) && (data = read_entry(fd, &z))) {
        int first = 0;
        long at = find_list(data, z.usize, &first);
        if (at < 0 || first == API_VULKAN) {
            droid_log("unity: graphics APIs: %s", at < 0 ? "no Vulkan in the game's list" : "Vulkan is first already");
        } else {
            uint32_t c = rd32(data + at), n = 0, apis[6];
            apis[n++] = API_VULKAN;
            for (uint32_t k = 0; k < c; k++) {
                uint32_t v = rd32(data + at + 4 + k * 4);
                if (v != API_VULKAN) apis[n++] = v;
            }
            for (uint32_t k = 0; k < c; k++) wr32(data + at + 4 + k * 4, apis[k]);
            droid_log("unity: graphics APIs had %d first: a copy of the APK with Vulkan first (once)", first);
            rc = write_copy(fd, &z, data, copy);
            droid_log("unity: %s", rc == 0 ? "Vulkan first now" : "the copy failed: the game's own list stays");
        }
    }
    free(data);
    free(z.cd);
    close(fd);
    return rc;
}

// ---- the phone view's Auto: is the game portrait only? ------------------------------------
// PlayerSettings (class 129) in globalgamemanagers, found by the file's object table (no type
// trees in a player build): defaultScreenOrientation at +20 (0/1 portrait, 2/3 landscape,
// 4 auto rotation; 2018.3, 2021.3, 2022.3 checked), and for auto rotation the allowed ones,
// found after m_StackTraceTypes (6 log types, 0-2) + two ints (the activity indicators):
// portrait, upside down, landscape right, landscape left, a few bools on by version.
static uint32_t rd32be(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64be(const unsigned char *p) { return (uint64_t)rd32be(p) << 32 | rd32be(p + 4); }

static int player_settings(const unsigned char *f, size_t n, const unsigned char **obj, uint32_t *size, int *major) {
    if (n < 48) return -1;
    const uint32_t ver = rd32be(f + 8);
    uint64_t data_at = rd32be(f + 12);
    size_t p = 20;
    if (ver < 9 || ver > 30 || f[16] != 0) return -1; // little-endian metadata only
    if (ver >= 22) {
        data_at = rd64be(f + 32);
        p = 48;
    }
    const size_t meta0 = p;
    (void)meta0;
    const char *uver = (const char *)f + p;
    const void *z = memchr(f + p, 0, n - p);
    if (!z) return -1;
    *major = atoi(uver);
    p = (size_t)((const unsigned char *)z - f) + 1;
    p += 4; // target platform
    const int tree = ver >= 13 ? f[p++] : 0;
    if (tree) return -1; // an editor-style file: not read here
    if (p + 4 > n) return -1;
    const uint32_t types = rd32(f + p);
    p += 4;
    if (types > 4096) return -1;
    int32_t ids[4096];
    for (uint32_t i = 0; i < types; i++) {
        if (p + 4 > n) return -1;
        const int32_t cls = (int32_t)rd32(f + p);
        p += 4;
        if (ver >= 16) p += 1;
        if (ver >= 17) p += 2;
        if (ver >= 13) {
            if ((ver < 16 && cls < 0) || (ver >= 16 && cls == 114)) p += 16;
            p += 16;
        }
        ids[i] = cls;
    }
    if (p + 4 > n) return -1;
    const uint32_t count = rd32(f + p);
    p += 4;
    for (uint32_t i = 0; i < count; i++) {
        if (ver >= 14) p = (p + 3) & ~(size_t)3;
        if (p + 28 > n) return -1;
        p += ver >= 14 ? 8 : 4; // path id
        uint64_t start;
        if (ver >= 22) {
            start = rd32(f + p) | (uint64_t)rd32(f + p + 4) << 32;
            p += 8;
        } else {
            start = rd32(f + p);
            p += 4;
        }
        const uint32_t sz = rd32(f + p);
        const uint32_t t = rd32(f + p + 4);
        p += 8;
        if (ver < 16) p += 2; // class id (old files)
        if (ver == 15 || ver == 16) p += 1; // stripped
        const int32_t cls = ver >= 16 ? (t < types ? ids[t] : -1) : (int32_t)t;
        if (cls == 129 && data_at + start + sz <= n) {
            *obj = f + data_at + start;
            *size = sz;
            return 0;
        }
    }
    return -1;
}

int unity_portrait_only(const char *apk) {
    int fd = open(apk, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    zip_info z;
    memset(&z, 0, sizeof(z));
    unsigned char *data = NULL;
    int rc = -1;
    const unsigned char *ps = NULL;
    uint32_t psz = 0;
    int major = 0;
    if (fstat(fd, &st) == 0 && read_zip(fd, st.st_size, &z) == 0 && z.usize < (64u << 20) &&
        (data = read_entry(fd, &z)) && player_settings(data, z.usize, &ps, &psz, &major) == 0 && psz > 64) {
        const int dso = (int)rd32(ps + 20);
        if (dso == 0 || dso == 1) rc = 1;
        else if (dso == 2 || dso == 3) rc = 0;
        else if (dso == 4) {
            // the allowed four: after the 6 log types and the two indicator ints, by version
            const int skip = major >= 2022 ? 9 : major >= 2019 ? 10 : 17;
            for (uint32_t i = 24; i + 28 + 32 <= psz; i += 4) {
                if (rd32(ps + i) != 6) continue;
                int ok = 1;
                for (int k = 1; k <= 6 && ok; k++) ok = rd32(ps + i + 4 * k) <= 2;
                if (!ok) continue;
                const unsigned char *a = ps + i + 28 + skip;
                if (a[0] > 1 || a[1] > 1 || a[2] > 1 || a[3] > 1 || !(a[0] | a[1] | a[2] | a[3])) break;
                rc = !(a[2] | a[3]); // no landscape allowed
                break;
            }
        }
        droid_log("unity: screen orientation %d (Unity %d): %s", dso, major,
                  rc == 1 ? "portrait only" : rc == 0 ? "landscape allowed" : "not known");
    }
    free(data);
    free(z.cd);
    close(fd);
    return rc;
}
