// https.c — Hydroid's own HTTPS requests (mbedTLS 3.6, third_party/build-mbedtls.sh), for the
// Google services a game's Java SDK would talk to on a phone (firebase_rc.c: Remote Config), and
// games' own services (requests, downloads straight to a file).
// The game's own traffic never comes here: it has its own TLS, over net_shim's sockets.
// Certificates: Mozilla's list in the app's assets/certs (as the Java layer's trust store).
#include "https.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "android_shim.h" // APP0
#include "game_settings.h"

void droid_log(const char *fmt, ...);
int net_host_lookup(const char *name, struct in_addr *out); // net_shim.c

// mbedTLS's entropy (MBEDTLS_ENTROPY_HARDWARE_ALT): the platform's arc4random_buf (the console's
// FreeBSD libc has it; so has the PC's glibc)
void arc4random_buf(void *buf, size_t n);
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    arc4random_buf(out, len);
    *olen = len;
    return 0;
}

static mbedtls_x509_crt cas;
static int cas_state; // 0 not read, 1 read, -1 none
static pthread_mutex_t cas_lock = PTHREAD_MUTEX_INITIALIZER;

static int load_cas(void) {
    pthread_mutex_lock(&cas_lock);
    if (!cas_state) {
        mbedtls_x509_crt_init(&cas);
        const int rc = mbedtls_x509_crt_parse_file(&cas, APP0 "/assets/certs/cacert.pem");
        cas_state = rc >= 0 ? 1 : -1;
        if (cas_state < 0) droid_log("https: no root certificates (assets/certs/cacert.pem): %d", rc);
    }
    pthread_mutex_unlock(&cas_lock);
    return cas_state;
}

static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    const ssize_t r = send(*(int *)ctx, b, n, 0);
    return r < 0 ? -1 : (int)r;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    const ssize_t r = recv(*(int *)ctx, b, n, 0);
    return r < 0 ? -1 : (int)r;
}

// One TLS connection to host:443
typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} tls_conn;

static void tls_close(tls_conn *c) {
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int tls_open(tls_conn *c, const char *host) {
    c->fd = -1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    if (load_cas() < 0) return 0;
    struct in_addr ip;
    if (net_host_lookup(host, &ip) != 0) {
        droid_log("https: %s not found", host);
        return 0;
    }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return 0;
    struct timeval tv = { 15, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // a big receive window, asked for before connecting (the window is agreed then): the console's
    // own is small, and over the internet's ~30 ms it held a download to ~2 MB/s (64 KB a trip)
    const int rcv = 4 << 20;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    static int told; // (what the console gave: it caps it, Orbit Store saw 512 KB for 1 MB)
    if (!told) {
        int got = 0;
        socklen_t gl = sizeof(got);
        getsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &got, &gl);
        droid_log("https: receive window %d KB (asked %d KB)", got >> 10, rcv >> 10);
        told = 1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = ip;
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        droid_log("https: %s: no connection", host);
        return 0;
    }
    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, (const unsigned char *)"hydroid", 7);
    if (!rc) rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!rc) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->conf, &cas, NULL);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    }
    if (!rc) rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (!rc) {
        mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
        while ((rc = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        }
    }
    if (rc) {
        char err[120];
        mbedtls_strerror(rc, err, sizeof(err));
        droid_log("https: %s: TLS failed (%s)", host, err);
        return 0;
    }
    return 1;
}

static int tls_write_all(tls_conn *c, const void *p, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        const int w = mbedtls_ssl_write(&c->ssl, (const unsigned char *)p + sent, n - sent);
        if (w <= 0 && w != MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (w > 0) sent += (size_t)w;
    }
    return 1;
}

static int tls_read(tls_conn *c, void *p, size_t n) { // > 0 bytes, 0 the end
    for (;;) {
        const int r = mbedtls_ssl_read(&c->ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return r > 0 ? r : 0; // (close_notify, or the server closed)
    }
}

// a header's value in the response's head (any case), copied; NULL when it isn't there
static char *head_value(const char *head, const char *name) {
    const size_t nl = strlen(name);
    for (const char *p = head; (p = strstr(p, "\r\n"));) {
        p += 2;
        if (strncasecmp(p, name, nl) || p[nl] != ':') continue;
        const char *v = p + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        const char *e = strstr(v, "\r\n");
        const size_t len = e ? (size_t)(e - v) : strlen(v);
        char *out = malloc(len + 1);
        memcpy(out, v, len);
        out[len] = 0;
        return out;
    }
    return NULL;
}

// the body as it comes: to the sink (a 2xx with one), else kept
typedef struct {
    const https_req *r;
    https_resp *out;
    int to_sink, stopped;
    size_t cap;
} body_to;

static void body_put(body_to *b, const char *p, size_t n) {
    if (!n || b->stopped) return;
    if (b->to_sink) {
        if (!b->r->sink(b->r->sink_ctx, p, n)) b->stopped = 1;
        b->out->len += n;
        return;
    }
    if (b->out->len + n + 1 > b->cap) {
        b->cap = (b->out->len + n + 1) * 2;
        b->out->body = realloc(b->out->body, b->cap);
    }
    memcpy(b->out->body + b->out->len, p, n);
    b->out->len += n;
    b->out->body[b->out->len] = 0;
}

// chunked: the sizes' lines, the data, its CRLF; the bytes come in any pieces
typedef struct {
    int state; // 0 a size line, 1 data, 2 the data's CRLF, 3 the end
    char line[32];
    int line_n;
    long long left;
} chunks;

static void chunks_feed(chunks *k, body_to *b, const char *p, size_t n) {
    while (n && k->state != 3 && !b->stopped) {
        if (k->state == 0) {
            const char ch = *p++;
            n--;
            if (ch == '\n') {
                k->line[k->line_n] = 0;
                k->left = strtoll(k->line, NULL, 16);
                k->line_n = 0;
                k->state = k->left > 0 ? 1 : 3;
            } else if (ch != '\r' && k->line_n < (int)sizeof(k->line) - 1) {
                k->line[k->line_n++] = ch;
            }
        } else if (k->state == 1) {
            const size_t take = (size_t)k->left < n ? (size_t)k->left : n;
            body_put(b, p, take);
            p += take, n -= take, k->left -= (long long)take;
            if (!k->left) k->state = 2;
        } else { // the CRLF after the data
            if (*p++ == '\n') k->state = 0;
            n--;
        }
    }
}

static int split_url(const char *url, char *host, size_t hn, const char **path) {
    if (strncmp(url, "https://", 8)) return 0;
    const char *h = url + 8, *e = h + strcspn(h, "/?");
    if ((size_t)(e - h) >= hn) return 0;
    memcpy(host, h, (size_t)(e - h));
    host[e - h] = 0;
    *path = *e ? e : "/";
    return 1;
}

static int is_redirect(int st) { return st == 301 || st == 302 || st == 303 || st == 307 || st == 308; }

// a thread's kept connection (https_req.keep): one, to the server it last asked
typedef struct {
    tls_conn *c;
    char host[256];
} kept_conn;
static pthread_key_t kept_key;
static pthread_once_t kept_once = PTHREAD_ONCE_INIT;
static void kept_make(void) { pthread_key_create(&kept_key, NULL); }
static kept_conn *kept_get(void) {
    pthread_once(&kept_once, kept_make);
    kept_conn *k = pthread_getspecific(kept_key);
    if (!k && (k = calloc(1, sizeof(*k)))) pthread_setspecific(kept_key, k);
    return k;
}
static void conn_drop(tls_conn *c) {
    if (!c) return;
    tls_close(c);
    free(c);
}

int https_request(const https_req *r, https_resp *out) {
    memset(out, 0, sizeof(*out));
    out->total = -1;
    char url[4096];
    snprintf(url, sizeof(url), "%s", r->url);
    const char *method = r->method;
    const void *body = r->body;
    size_t body_len = r->body_len;
    int fresh = 0; // a kept connection failed: a new one
    for (int hop = 0; hop < 6; hop++) {
        char host[256];
        const char *path;
        if (!split_url(url, host, sizeof(host), &path)) {
            droid_log("https: not an https address: %.80s", url);
            return 0;
        }
        tls_conn *c = NULL;
        int reused = 0;
        kept_conn *kc = r->keep ? kept_get() : NULL;
        if (kc && kc->c) { // this thread's connection: the same server goes on with it
            if (!fresh && !strcmp(kc->host, host)) c = kc->c, reused = 1;
            else conn_drop(kc->c);
            kc->c = NULL;
        }
        if (!c) {
            c = malloc(sizeof(*c));
            if (!c) return 0;
            if (!tls_open(c, host)) {
                conn_drop(c);
                return 0;
            }
        }
        // the request
        char range[64] = "";
        if (r->to > 0) snprintf(range, sizeof(range), "Range: bytes=%lld-%lld\r\n", r->from, r->to - 1);
        else if (r->from > 0) snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", r->from);
        const size_t qn = strlen(method) + strlen(path) + strlen(host) + (r->headers ? strlen(r->headers) : 0) + 256;
        char *q = malloc(qn);
        const int qlen = snprintf(q, qn, "%s %s HTTP/1.1\r\nHost: %s\r\n%s%sContent-Length: %zu\r\nConnection: %s\r\n\r\n",
                                  method, path, host, r->headers ? r->headers : "", range, body_len,
                                  r->keep ? "keep-alive" : "close");
        const int sent = tls_write_all(c, q, (size_t)qlen) && (!body_len || tls_write_all(c, body, body_len));
        free(q);
        if (!sent) {
            conn_drop(c);
            if (reused) { // (the server had closed it: again, on a new one)
                fresh = 1, hop--;
                continue;
            }
            return 0;
        }
        // the head
        size_t cap = 1 << 16, n = 0;
        char *buf = malloc(cap + 1), *head_end = NULL;
        for (;;) {
            if (n == cap) buf = realloc(buf, (cap *= 2) + 1);
            const int got = tls_read(c, buf + n, cap - n);
            if (got <= 0) break;
            n += (size_t)got;
            buf[n] = 0;
            if ((head_end = strstr(buf, "\r\n\r\n"))) break;
        }
        if (!head_end) {
            free(buf);
            conn_drop(c);
            if (reused && !n) { // (closed while it waited: again, on a new one)
                fresh = 1, hop--;
                continue;
            }
            return 0;
        }
        *head_end = 0;
        int st = 0;
        sscanf(buf, "HTTP/1.%*d %d", &st);
        out->status = st;
        if (is_redirect(st)) {
            char *loc = head_value(buf, "Location");
            free(buf);
            conn_drop(c);
            if (!loc) return 1;
            if (loc[0] == '/') snprintf(url, sizeof(url), "https://%s%s", host, loc);
            else snprintf(url, sizeof(url), "%s", loc);
            free(out->location);
            out->location = loc;
            if (st == 303 || ((st == 301 || st == 302) && !strcmp(method, "POST"))) method = "GET", body = NULL, body_len = 0;
            continue;
        }
        char *cl = head_value(buf, "Content-Length"), *cr = head_value(buf, "Content-Range"),
             *te = head_value(buf, "Transfer-Encoding");
        const long long length = cl ? strtoll(cl, NULL, 10) : -1;
        out->total = length;
        if (cr) { // "bytes 100-999/1000": the whole file's size
            const char *sl = strchr(cr, '/');
            if (sl && sl[1] != '*') out->total = strtoll(sl + 1, NULL, 10);
        }
        const int chunked = te && strstr(te, "chunked");
        char *conn_h = head_value(buf, "Connection");
        const int server_closes = conn_h && !strncasecmp(conn_h, "close", 5);
        free(cl), free(cr), free(te), free(conn_h);
        // the body: what came with the head, then the rest (into the same buffer)
        body_to b = { r, out, r->sink && st >= 200 && st < 300, 0, 0 };
        if (b.to_sink && r->head && !r->head(r->sink_ctx, st, length)) b.stopped = 1;
        chunks k = { 0 };
        const char *piece = head_end + 4;
        size_t piece_n = n - (size_t)(piece - buf);
        long long got_total = 0;
        for (;;) {
            if (piece_n) {
                if (chunked) chunks_feed(&k, &b, piece, piece_n);
                else body_put(&b, piece, piece_n);
                got_total += (long long)piece_n;
            }
            if (b.stopped || (chunked && k.state == 3) || (!chunked && length >= 0 && got_total >= length)) break;
            const int got = tls_read(c, buf, cap);
            if (got <= 0) break;
            piece = buf, piece_n = (size_t)got;
        }
        if (!out->body && !b.to_sink) out->body = calloc(1, 1);
        free(buf);
        // kept for the next: the whole body read, its length said (a chunked one: not kept)
        if (kc && !server_closes && !b.stopped && !chunked && length >= 0 && got_total == length) {
            kc->c = c;
            snprintf(kc->host, sizeof(kc->host), "%s", host);
        } else {
            conn_drop(c);
        }
        return 1;
    }
    droid_log("https: too many redirects: %.80s", url);
    return 1;
}

void https_resp_free(https_resp *out) {
    free(out->body), free(out->location);
    out->body = out->location = NULL;
}

char *https_post(const char *host, const char *path, const char *headers, const char *body, int *status) {
    char url[4096];
    snprintf(url, sizeof(url), "https://%s%s", host, path);
    https_req r = { .method = "POST", .url = url, .headers = headers, .body = body, .body_len = body ? strlen(body) : 0 };
    https_resp out;
    *status = 0;
    if (!https_request(&r, &out)) return NULL;
    *status = out.status;
    free(out.location);
    return out.body;
}
