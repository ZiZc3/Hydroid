// apk.c — reads the game's APK in place: zip central directory, stored or
// deflated (zlib) entries. Serves Android's AAssetManager API from assets/.

#include "apk.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "android_shim.h"
#include "game_settings.h"

typedef struct {
    char *name;
    int fd; // which archive (base APK first, then splits)
    uint16_t method; // 0 stored, 8 deflate
    uint32_t csize, usize, local;
} entry;

#define MAX_ARCHIVES 24 // a game's APKs: base + asset packs + config splits (Dead Trigger 2: 9)
static char archive_paths[MAX_ARCHIVES][256];
static int archive_count;
static entry *entries;
static int entry_count;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(int fd, void *buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, (char *)buf + done, n - done, off + (off_t)done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

int apk_is_open(void) { return archive_count > 0; }
int apk_archive_count(void) { return archive_count; }
const char *apk_archive_path(int i) { return i >= 0 && i < archive_count ? archive_paths[i] : NULL; }

int apk_open(const char *path) {
    if (archive_count == MAX_ARCHIVES) return -5;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    // End of central directory: the last 22..65557 bytes.
    size_t tail = size < 65557 ? (size_t)size : 65557;
    uint8_t *buf = malloc(tail);
    if (!buf || read_at(fd, buf, tail, size - (off_t)tail) != 0) {
        free(buf);
        close(fd);
        return -2;
    }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (u32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) {
        free(buf);
        close(fd);
        return -3;
    }
    uint16_t count = u16(buf + eocd + 10);
    uint32_t cd_size = u32(buf + eocd + 12), cd_off = u32(buf + eocd + 16);
    free(buf);
    uint8_t *cd = malloc(cd_size);
    entry *grown = realloc(entries, (size_t)(entry_count + count) * sizeof(entry));
    if (!cd || !grown || read_at(fd, cd, cd_size, cd_off) != 0) {
        free(cd);
        close(fd);
        return -4;
    }
    entries = grown;
    int first = entry_count;
    const uint8_t *p = cd;
    for (int i = 0; i < count && p + 46 <= cd + cd_size && u32(p) == 0x02014b50; i++) {
        uint16_t nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32);
        entry *e = &entries[entry_count++];
        e->fd = fd;
        e->method = u16(p + 10);
        e->csize = u32(p + 20);
        e->usize = u32(p + 24);
        e->local = u32(p + 42);
        e->name = strndup((const char *)p + 46, nlen);
        p += 46 + nlen + xlen + clen;
    }
    free(cd);
    snprintf(archive_paths[archive_count++], sizeof(archive_paths[0]), "%s", path);
    droid_log("apk: %s, %d entries", path, entry_count - first);
    return 0;
}

static const entry *find(const char *name) {
    for (int i = 0; i < entry_count; i++)
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

// Where the entry's data starts (after its local header).
static off_t data_offset(const entry *e) {
    uint8_t h[30];
    if (read_at(e->fd, h, sizeof(h), e->local) != 0 || u32(h) != 0x04034b50) return -1;
    return (off_t)e->local + 30 + u16(h + 26) + u16(h + 28);
}

// The whole entry, uncompressed (malloc'd).
static void *load(const entry *e) {
    off_t off = data_offset(e);
    if (off < 0) return NULL;
    uint8_t *out = malloc(e->usize ? e->usize : 1);
    if (!out) return NULL;
    if (e->method == 0) {
        if (read_at(e->fd, out, e->usize, off) != 0) { free(out); return NULL; }
        return out;
    }
    uint8_t *in = malloc(e->csize ? e->csize : 1);
    if (!in || read_at(e->fd, in, e->csize, off) != 0) {
        free(in);
        free(out);
        return NULL;
    }
    z_stream z;
    memset(&z, 0, sizeof(z));
    int ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
    if (ok) {
        z.next_in = in;
        z.avail_in = e->csize;
        z.next_out = out;
        z.avail_out = e->usize;
        ok = inflate(&z, Z_FINISH) == Z_STREAM_END;
        inflateEnd(&z);
    }
    free(in);
    if (!ok) {
        droid_log("apk: inflate failed for %s", e->name);
        free(out);
        return NULL;
    }
    return out;
}

int apk_extract(const char *name, const char *dest) {
    const entry *e = find(name);
    if (!e) return -1;
    struct stat st;
    if (stat(dest, &st) == 0 && (uint64_t)st.st_size == e->usize) return 0;
    void *data = load(e);
    if (!data) return -2;
    // written beside it, then renamed: a cut-off write (the app closed) is never taken for the file
    char part[300];
    snprintf(part, sizeof(part), "%s.part", dest);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc = fd >= 0 && write(fd, data, e->usize) == (ssize_t)e->usize ? 0 : -3;
    if (fd >= 0) close(fd);
    if (rc == 0 && rename(part, dest) != 0) rc = -4;
    if (rc != 0) unlink(part);
    free(data);
    droid_log("apk: extracted %s (%u KB) rc=%d", name, e->usize >> 10, rc);
    return rc;
}

int apk_extract_stream(const char *name, const char *dest) {
    const entry *e = find(name);
    if (!e) return -1;
    struct stat st;
    if (stat(dest, &st) == 0 && (uint64_t)st.st_size == e->usize) return 0;
    off_t off = data_offset(e);
    if (off < 0) return -2;
    char part[300];
    snprintf(part, sizeof(part), "%s.part", dest);
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return -3;
    int rc = 0;
    if (e->method == 0) {
        const size_t CH = 4u << 20;
        unsigned char *buf = malloc(CH);
        if (!buf) rc = -4;
        for (off_t pos = 0; rc == 0 && pos < (off_t)e->usize;) {
            size_t n = (size_t)((off_t)e->usize - pos < (off_t)CH ? (off_t)e->usize - pos : (off_t)CH);
            if (read_at(e->fd, buf, n, off + pos) != 0 || write(out, buf, n) != (ssize_t)n) rc = -5;
            pos += (off_t)n;
        }
        free(buf);
    } else {
        void *data = load(e); // deflated: not the OBB's case, but works
        if (!data) rc = -6;
        else {
            if (write(out, data, e->usize) != (ssize_t)e->usize) rc = -7;
            free(data);
        }
    }
    close(out);
    if (rc == 0 && rename(part, dest) != 0) rc = -8;
    if (rc != 0) unlink(part);
    droid_log("apk: streamed %s (%u MB) rc=%d", name, e->usize >> 20, rc);
    return rc;
}

void *apk_load(const char *name, size_t *size) {
    const entry *e = find(name);
    void *data = e ? load(e) : NULL;
    if (data && size) *size = e->usize;
    return data;
}

void apk_each(const char *prefix, void (*fn)(const char *name, void *ctx), void *ctx) {
    size_t n = strlen(prefix);
    for (int i = 0; i < entry_count; i++)
        if (strncmp(entries[i].name, prefix, n) == 0) fn(entries[i].name, ctx);
}

// ---- AAssetManager over assets/ ---------------------------------------------------

typedef struct {
    const entry *e;
    uint8_t *data; // deflated entries / local fallbacks: the bytes; stored: NULL (read in place)
    off_t base;    // stored entries: where the data starts in the APK
    int64_t pos;
    int64_t usize;
    int is_local;
    char local[700];
} asset;

static int manager; // AAssetManager*: any non-NULL pointer

static void *AAssetManager_fromJava(void *env, void *obj) {
    (void)env; (void)obj;
    return &manager;
}

static asset *AAssetManager_open(void *mgr, const char *filename, int mode) {
    (void)mgr; (void)mode;
    char name[512];
    while (filename[0] == '/') filename++;
    snprintf(name, sizeof(name), "assets/%s", filename);
    const entry *e = find(name);
    static int told;
    if (told < 120) {
        told++;
        droid_log("asset: open %s -> %s", name, e ? "found" : "NOT in the APK");
    }
    // not in the APK: an Unreal game's file in its own folder (its downloader's, or ours); every
    // other game: none, as before
    if (!e && !(droid_quirks & QUIRK_UNREAL)) return NULL;
    asset *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    if (!e) {
        // Not in the APK: a file in the game's own folder (a packer/downloader's file, or ours).
        char path[700];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", game_dir, filename);
        if (stat(path, &st) != 0) {
            snprintf(path, sizeof(path), "%s/assets/%s", game_dir, filename);
            if (stat(path, &st) != 0) { free(a); return NULL; }
        }
        FILE *f = fopen(path, "rb");
        if (!f) { free(a); return NULL; }
        a->usize = (int64_t)st.st_size;
        a->data = malloc((size_t)(a->usize ? a->usize : 1));
        if (!a->data || fread(a->data, 1, (size_t)a->usize, f) != (size_t)a->usize) {
            fclose(f); free(a->data); free(a);
            return NULL;
        }
        fclose(f);
        a->is_local = 1;
        snprintf(a->local, sizeof(a->local), "%s", path);
        droid_log("asset: local %s", path);
        return a;
    }
    a->e = e;
    a->usize = e->usize;
    if (e->method == 0) {
        a->base = data_offset(e);
    } else if (!(a->data = load(e))) {
        free(a);
        return NULL;
    }
    return a;
}

static int AAsset_openFileDescriptor(asset *a, off_t *start, off_t *len) {
    if (!a || a->is_local == 2) return -1;
    if (a->is_local) {
        int fd = open(a->local, O_RDONLY);
        if (fd < 0) return -1;
        if (start) *start = 0;
        if (len) *len = (off_t)a->usize;
        return fd;
    }
    if (!a->e || a->e->method != 0) return -1;
    off_t base = data_offset(a->e);
    if (base < 0) return -1;
    int fd = dup(a->e->fd);
    if (fd < 0) return -1;
    if (start) *start = base;
    if (len) *len = (off_t)a->e->usize;
    return fd;
}
static int AAsset_openFileDescriptor64(asset *a, int64_t *start, int64_t *len) {
    off_t s = 0, l = 0;
    int fd = AAsset_openFileDescriptor(a, &s, &l);
    if (fd >= 0) { if (start) *start = (int64_t)s; if (len) *len = (int64_t)l; }
    return fd;
}

static int AAsset_read(asset *a, void *buf, size_t count) {
    if (!a) return -1;
    int64_t left = a->usize - a->pos;
    if (left <= 0) return 0;
    if ((int64_t)count > left) count = (size_t)left;
    if (a->data) memcpy(buf, a->data + a->pos, count);
    else if (read_at(a->e->fd, buf, count, a->base + a->pos) != 0) return -1;
    a->pos += (int64_t)count;
    return (int)count;
}

static int64_t AAsset_seek64(asset *a, int64_t off, int whence) {
    if (!a) return -1;
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? a->pos + off
              : a->usize + off;
    if (p < 0 || p > a->usize) return -1;
    a->pos = p;
    return p;
}

static long AAsset_seek(asset *a, long off, int whence) { return (long)AAsset_seek64(a, off, whence); }
static void AAsset_close(asset *a) {
    if (!a) return;
    free(a->data);
    free(a);
}
static long AAsset_getLength(asset *a) { return a ? (long)a->usize : 0; }
static int64_t AAsset_getLength64(asset *a) { return a ? a->usize : 0; }
static long AAsset_getRemainingLength(asset *a) { return a ? (long)(a->usize - a->pos) : 0; }
static int64_t AAsset_getRemainingLength64(asset *a) { return a ? a->usize - a->pos : 0; }
static int AAsset_isAllocated(asset *a) { return a && a->data != NULL; }

static const void *AAsset_getBuffer(asset *a) {
    if (!a) return NULL;
    if (!a->data && a->e) a->data = load(a->e);
    return a->data;
}

// AAssetDir: the files (not folders) directly inside assets/<dir>.
typedef struct {
    char prefix[512];
    int next;
} asset_dir;

static asset_dir *AAssetManager_openDir(void *mgr, const char *dir) {
    (void)mgr;
    asset_dir *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    snprintf(d->prefix, sizeof(d->prefix), "assets/%s%s", dir, dir[0] ? "/" : "");
    return d;
}

static const char *AAssetDir_getNextFileName(asset_dir *d) {
    size_t n = strlen(d->prefix);
    while (d->next < entry_count) {
        const char *name = entries[d->next++].name;
        if (strncmp(name, d->prefix, n) == 0 && name[n] && !strchr(name + n, '/')) return name + n;
    }
    return NULL;
}

static void AAssetDir_rewind(asset_dir *d) { d->next = 0; }
static void AAssetDir_close(asset_dir *d) { free(d); }

#define E(name) { #name, (void *)name }
static const struct { const char *name; void *fn; } table[] = {
    E(AAssetManager_fromJava), E(AAssetManager_open), E(AAsset_openFileDescriptor), E(AAsset_openFileDescriptor64), E(AAsset_read), E(AAsset_seek),
    E(AAsset_seek64), E(AAsset_close), E(AAsset_getLength), E(AAsset_getLength64),
    E(AAsset_getRemainingLength), E(AAsset_getRemainingLength64), E(AAsset_isAllocated),
    E(AAsset_getBuffer), E(AAssetManager_openDir), E(AAssetDir_getNextFileName),
    E(AAssetDir_rewind), E(AAssetDir_close),
};

void *apk_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}
