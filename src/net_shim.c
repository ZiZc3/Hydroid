// net_shim.c — sockets for the games' ARM code: Android's (Linux numbers and layouts)
// on the host's. The PS5's are FreeBSD's: address families, sockaddr (a length byte
// first), socket options, message flags, poll bits, msghdr and getaddrinfo's list all
// differ, so each call converts on the way in and out. The PC test runner's host is
// Linux: the same numbers (the same code converts nothing), only getaddrinfo's list
// differs (glibc's field order). DNS on the PS5: libc's getaddrinfo, else the console's
// own resolver (sceNetResolver). No SIGPIPE: a closed connection is an error, not a kill.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "android_shim.h"
#include "game_settings.h"

// ---- Linux's numbers (what the game's code uses) ---------------------------------------
#define L_AF_UNSPEC 0
#define L_AF_UNIX 1
#define L_AF_INET 2
#define L_AF_INET6 10
#define L_SOCK_NONBLOCK 0x800
#define L_SOCK_CLOEXEC 0x80000

static int host_family(int f) {
    switch (f) {
    case L_AF_UNSPEC: return AF_UNSPEC;
    case L_AF_UNIX: return AF_UNIX;
    case L_AF_INET: return AF_INET;
    case L_AF_INET6: return AF_INET6;
    }
    return -1;
}

static int linux_family(int f) {
    if (f == AF_INET) return L_AF_INET;
    if (f == AF_INET6) return L_AF_INET6;
    if (f == AF_UNIX) return L_AF_UNIX;
    return L_AF_UNSPEC;
}

// A Linux sockaddr (a 16-bit family first) -> the host's (FreeBSD: length byte, family byte)
static int to_host_addr(const void *la, socklen_t llen, struct sockaddr_storage *h, socklen_t *hlen) {
    if (!la || llen < 2) {
        errno = EINVAL;
        return -1;
    }
    uint16_t fam;
    memcpy(&fam, la, 2);
    memset(h, 0, sizeof(*h));
    size_t n = llen < sizeof(*h) ? llen : sizeof(*h);
    memcpy(h, la, n);
    int hf = host_family(fam);
    if (hf < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    ((struct sockaddr *)h)->sa_family = (sa_family_t)hf;
    if (hf == AF_INET) n = sizeof(struct sockaddr_in);
    else if (hf == AF_INET6) n = sizeof(struct sockaddr_in6);
#ifndef __linux__
    ((struct sockaddr *)h)->sa_len = (uint8_t)n;
#endif
    *hlen = (socklen_t)n;
    return 0;
}

// The host's sockaddr -> Linux's, into the game's buffer (len: its size in, the real size out)
static void to_linux_addr(const struct sockaddr *h, socklen_t hlen, void *la, socklen_t *llen) {
    if (!la || !llen) return;
    unsigned char tmp[sizeof(struct sockaddr_storage)];
    size_t n = hlen < sizeof(tmp) ? hlen : sizeof(tmp);
    memcpy(tmp, h, n);
    uint16_t lf = (uint16_t)linux_family(h->sa_family);
    memcpy(tmp, &lf, 2); // FreeBSD's length + family bytes -> Linux's 16-bit family
    memcpy(la, tmp, n < *llen ? n : *llen);
    *llen = (socklen_t)n;
}

static int linux_msg_flags(int f) { // Linux MSG_* -> the host's; never a SIGPIPE
    int h = 0;
    if (f & 0x1) h |= MSG_OOB;
    if (f & 0x2) h |= MSG_PEEK;
    if (f & 0x4) h |= MSG_DONTROUTE;
    if (f & 0x40) h |= MSG_DONTWAIT;
    if (f & 0x100) h |= MSG_WAITALL;
#ifdef MSG_NOSIGNAL
    h |= MSG_NOSIGNAL;
#endif
    return h;
}

static int net_logged;
#define NET_LOG(...) do { if (net_logged < 300) { net_logged++; droid_log(__VA_ARGS__); } } while (0)
// a call that failed: its name and errno (the host's number), in the log
#define NET_FAIL(rc, ...) do { if ((rc) < 0) { int e_ = errno; NET_LOG(__VA_ARGS__); errno = e_; } } while (0)

// The console's network library, started once before the first socket (Chrome on the
// PS5 does the same)
#ifndef __linux__
int sceNetInit(void);
static void net_init_once(void) { droid_log("net: sceNetInit %#x", sceNetInit()); }
#endif
static void net_init(void) {
#ifndef __linux__
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, net_init_once);
#endif
}

// ---- sockets ---------------------------------------------------------------------------

static void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

// Non-blocking, as the game asked, per fd: the console doesn't keep it reliably (FIONBIO
// on, then an F_GETFL / F_SETFL pair that doesn't see it turns it off). Shadow Fight 2's
// HTTP thread then blocked for good in curl's connection check (recv on a kept connection),
// and its put.php answer was never read. Each recv / send of such a socket: MSG_DONTWAIT.
#define NB_FDS 4096
static unsigned char nb_fd[NB_FDS];
void net_nonblock_set(int fd, int on) {
    if (fd >= 0 && fd < NB_FDS) nb_fd[fd] = on ? 1 : 0;
}
int net_nonblock(int fd) { return fd >= 0 && fd < NB_FDS && nb_fd[fd]; }
static int with_nb(int fd, int host_flags) { return net_nonblock(fd) ? host_flags | MSG_DONTWAIT : host_flags; }

static void type_flags(int fd, int type) {
    net_nonblock_set(fd, type & L_SOCK_NONBLOCK);
    if (type & L_SOCK_NONBLOCK) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int nb = 1;
        ioctl(fd, FIONBIO, &nb); // the console's sockets: fcntl's O_NONBLOCK isn't enough
    }
    if (type & L_SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int n_socket(int domain, int type, int proto) {
    int hf = host_family(domain);
    if (hf < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    net_init();
    int fd = socket(hf, type & 0xf, proto);
    NET_LOG("net: socket(%d, %#x, %d) -> %d%s", domain, type, proto, fd, fd < 0 ? " (failed)" : "");
    if (fd < 0) return -1;
    type_flags(fd, type);
    no_sigpipe(fd);
    if ((type & 0xf) == 1) { // TCP: big buffers, as a downloader sets them - the console's own are
        // small, a cap on the data under way to a far server (Shadow Fight 3 downloaded ~3 MB/s)
        int rcv = 2 << 20, snd = 512 << 10;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        static int told;
        if (!told++) {
            int r = 0, s = 0;
            socklen_t l = sizeof(r);
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &r, &l);
            l = sizeof(s);
            getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &s, &l);
            droid_log("net: TCP buffers %d KB in, %d KB out (asked 2048 / 512)", r >> 10, s >> 10);
        }
    }
    return fd;
}

static int n_socketpair(int domain, int type, int proto, int sv[2]) {
    int hf = host_family(domain);
    if (hf < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (socketpair(hf, type & 0xf, proto, sv) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        type_flags(sv[i], type);
        no_sigpipe(sv[i]);
    }
    return 0;
}

static void spy_open(int fd, const char *where); // NetSpy, below
static void spy_received(int fd, const uint8_t *b, ssize_t n);

static int n_connect(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage h;
    socklen_t hl;
    if (settings.offline && addr && len >= 8) { // offline: only this console itself
        unsigned short fam;
        memcpy(&fam, addr, 2);
        const unsigned char *ip = (const unsigned char *)addr + 4;
        if (!(fam == 2 && ip[0] == 127)) {
            errno = ENETUNREACH;
            return -1;
        }
    }
    if (to_host_addr(addr, len, &h, &hl) != 0) {
        unsigned short fam = 0;
        if (addr && len >= 2) memcpy(&fam, addr, 2);
        NET_LOG("net: connect(%d): address not usable (family %u, %u bytes), errno %d", fd, fam, (unsigned)len, errno);
        return -1;
    }
    int rc = connect(fd, (struct sockaddr *)&h, hl);
    if (h.ss_family != AF_INET) {
        NET_LOG("net: connect(%d, family %d) -> %d (errno %d)", fd, h.ss_family, rc, rc ? errno : 0);
    } else {
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)&h)->sin_addr, ip, sizeof(ip));
        int e = errno;
        NET_LOG("net: connect(%d, %s:%d) -> %d (errno %d)", fd, ip, ntohs(((struct sockaddr_in *)&h)->sin_port),
                rc, rc ? errno : 0);
        if (rc == 0 || e == EINPROGRESS) {
            char where[64];
            snprintf(where, sizeof(where), "%s:%d", ip, ntohs(((struct sockaddr_in *)&h)->sin_port));
            spy_open(fd, where);
        }
        errno = e;
    }
    return rc;
}

static int n_bind(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage h;
    socklen_t hl;
    if (to_host_addr(addr, len, &h, &hl) != 0) return -1;
    int rc = bind(fd, (struct sockaddr *)&h, hl);
    if (rc != 0) {
        int e = errno;
        int port = 0, any = 0;
        if (h.ss_family == AF_INET) {
            port = ntohs(((struct sockaddr_in *)&h)->sin_port);
            any = ((struct sockaddr_in *)&h)->sin_addr.s_addr == htonl(INADDR_ANY);
        } else if (h.ss_family == AF_INET6) {
            port = ntohs(((struct sockaddr_in6 *)&h)->sin6_port);
            any = !memcmp(&((struct sockaddr_in6 *)&h)->sin6_addr, &in6addr_any, sizeof(in6addr_any));
        }
        // "any address, any port" (Mono binds so before it connects): the system picks
        // both at connect anyway. A title may not bind (EACCES): it's done all the same.
        if (e == EACCES && any && port == 0) {
            NET_LOG("net: bind(%d, any:0) refused by the console: left to connect", fd);
            return 0;
        }
        NET_LOG("net: bind(%d, family %d, port %d) failed, errno %d", fd, h.ss_family, port, e);
        errno = e;
    }
    return rc;
}

static int n_listen(int fd, int backlog) { return listen(fd, backlog); }

static int n_accept4(int fd, void *addr, socklen_t *len, int flags) {
    struct sockaddr_storage h;
    socklen_t hl = sizeof(h);
    int c = accept(fd, (struct sockaddr *)&h, &hl);
    if (c < 0) return -1;
    type_flags(c, flags);
    no_sigpipe(c);
    to_linux_addr((struct sockaddr *)&h, hl, addr, len);
    return c;
}
static int n_accept(int fd, void *addr, socklen_t *len) { return n_accept4(fd, addr, len, 0); }

static int n_getsockname(int fd, void *addr, socklen_t *len) {
    struct sockaddr_storage h;
    socklen_t hl = sizeof(h);
    if (getsockname(fd, (struct sockaddr *)&h, &hl) != 0) return -1;
    to_linux_addr((struct sockaddr *)&h, hl, addr, len);
    return 0;
}

static int n_getpeername(int fd, void *addr, socklen_t *len) {
    struct sockaddr_storage h;
    socklen_t hl = sizeof(h);
    if (getpeername(fd, (struct sockaddr *)&h, &hl) != 0) return -1;
    to_linux_addr((struct sockaddr *)&h, hl, addr, len);
    return 0;
}

// a socket's traffic (a request that never finished: did its data go out, did an answer come
// back?): every send and every recv that got something or failed, up to 600 lines; the
// "nothing yet" recvs (EAGAIN) counted per socket and said every 5000
static int traffic_lines;
static unsigned again_count[1024];
// ---- NetSpy (the debug tool): every connection, readable ----------------------------------
// tools.txt "NetSpy": a line when a connection opens (the server's address, its name from the
// TLS hello - that part isn't encrypted - or the HTTP Host), an HTTP request's first line when
// it's plain, and a summary when it ends (bytes each way, how long, who closed it). And an
// alarm: an answer that arrived but sits unread for 3 s (Shadow Fight 2's put.php answer did,
// for days: its HTTP thread was stuck elsewhere).
#define SPY_FDS 1024
static struct {
    int on;
    char where[64], name[96];
    int64_t opened, last_in, last_out;
    unsigned long long out, in;
    int stall_told;
} spy[SPY_FDS];
static int spy_on = -1;
static pthread_mutex_t spy_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t spy_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void *spy_watch(void *arg);
static int netspy(void) {
    if (spy_on < 0) {
        spy_on = droid_tool("NetSpy");
        if (spy_on) {
            pthread_t t;
            if (pthread_create(&t, NULL, spy_watch, NULL) == 0) pthread_detach(t);
            droid_log("NetSpy: on (connections, requests, answers left unread)");
        }
    }
    return spy_on;
}
// memmem (not in the console's libc)
static const char *spy_find(const uint8_t *h, size_t hn, const char *n, size_t nn) {
    for (size_t i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, n, nn)) return (const char *)h + i;
    return NULL;
}
static const char *spy_label(int fd) { return spy[fd].name[0] ? spy[fd].name : spy[fd].where; }
static void spy_open(int fd, const char *where) {
    if (!netspy() || fd < 0 || fd >= SPY_FDS) return;
    pthread_mutex_lock(&spy_lock);
    memset(&spy[fd], 0, sizeof(spy[fd]));
    spy[fd].on = 1;
    snprintf(spy[fd].where, sizeof(spy[fd].where), "%s", where);
    spy[fd].opened = spy[fd].last_in = spy_now();
    pthread_mutex_unlock(&spy_lock);
    droid_log("NetSpy: #%d connect %s", fd, where);
}
// the TLS ClientHello's server name, or a plain HTTP request's first line and Host
static void spy_sent(int fd, const uint8_t *b, ssize_t n) {
    if (!netspy() || fd < 0 || fd >= SPY_FDS || !spy[fd].on || n <= 0) return;
    if (!spy[fd].out && n > 43 && b[0] == 0x16 && b[5] == 0x01) { // TLS hello: walk to its extensions
        size_t at = 43;
        if (at < (size_t)n) at += 1 + b[at];                          // session id
        if (at + 2 <= (size_t)n) at += 2 + ((size_t)b[at] << 8 | b[at + 1]); // cipher suites
        if (at < (size_t)n) at += 1 + b[at];                          // compression
        size_t end = at + 2 <= (size_t)n ? at + 2 + ((size_t)b[at] << 8 | b[at + 1]) : 0;
        for (at += 2; end && at + 4 <= end && at + 4 <= (size_t)n;) {
            unsigned type = (unsigned)b[at] << 8 | b[at + 1], len = (unsigned)b[at + 2] << 8 | b[at + 3];
            if (type == 0 && at + 9 <= (size_t)n) { // server_name: list, type, length, the name
                unsigned nl = (unsigned)b[at + 7] << 8 | b[at + 8];
                if (at + 9 + nl <= (size_t)n && nl < sizeof(spy[fd].name)) {
                    memcpy(spy[fd].name, b + at + 9, nl);
                    spy[fd].name[nl] = 0;
                    droid_log("NetSpy: #%d is %s (TLS)", fd, spy[fd].name);
                }
                break;
            }
            at += 4 + len;
        }
    } else if (!spy[fd].out && n > 8 && (!memcmp(b, "GET ", 4) || !memcmp(b, "POST ", 5) || !memcmp(b, "PUT ", 4) ||
                                         !memcmp(b, "HEAD ", 5))) {
        size_t line = strcspn((const char *)b, "\r\n");
        const char *host = spy_find(b, (size_t)n, "\nHost: ", 7);
        if (host) {
            size_t hl = strcspn(host + 7, "\r\n");
            snprintf(spy[fd].name, sizeof(spy[fd].name), "%.*s", (int)(hl < 90 ? hl : 90), host + 7);
        }
        droid_log("NetSpy: #%d %s: %.*s", fd, spy_label(fd), (int)(line < 200 ? line : 200), (const char *)b);
    }
    if (!spy[fd].out && !spy[fd].name[0]) { // not TLS with a name, not HTTP: its first bytes
        char hex[3 * 48 + 1];
        int k = 0;
        for (ssize_t i = 0; i < n && i < 48; i++) k += snprintf(hex + k, sizeof(hex) - (size_t)k, "%02x ", b[i]);
        droid_log("NetSpy: #%d %s first bytes (%zd): %s", fd, spy_label(fd), n, hex);
    }
    spy[fd].out += (unsigned long long)n;
    spy[fd].last_out = spy_now();
}
static void spy_received(int fd, const uint8_t *b, ssize_t n) {
    if (!netspy() || fd < 0 || fd >= SPY_FDS || !spy[fd].on) return;
    if (n > 0) {
        if (!spy[fd].in && n > 12 && !memcmp(b, "HTTP/1.", 7)) {
            size_t line = strcspn((const char *)b, "\r\n");
            droid_log("NetSpy: #%d %s answers: %.*s", fd, spy_label(fd), (int)(line < 120 ? line : 120), (const char *)b);
        }
        spy[fd].in += (unsigned long long)n;
        spy[fd].last_in = spy_now();
        spy[fd].stall_told = 0;
    } else if (n == 0) {
        droid_log("NetSpy: #%d %s: closed by the server (%llu B out, %llu B in, %.1f s)", fd, spy_label(fd),
                  spy[fd].out, spy[fd].in, (spy_now() - spy[fd].opened) / 1e9);
        spy[fd].on = 0;
    }
}
void netspy_closed(int fd) { // the game closed it (bionic_shim's close)
    if (spy_on <= 0 || fd < 0 || fd >= SPY_FDS || !spy[fd].on) return;
    droid_log("NetSpy: #%d %s: closed by the game (%llu B out, %llu B in, %.1f s)", fd, spy_label(fd), spy[fd].out,
              spy[fd].in, (spy_now() - spy[fd].opened) / 1e9);
    spy[fd].on = 0;
}
// the alarm: an answer waiting, unread, for 3 s. And every 15 s the open connections: bytes
// each way, how long since data came in, what waits unread (a download that stalls shows)
static void *spy_watch(void *arg) {
    (void)arg;
    for (int tick = 1;; tick++) {
        sleep(1);
        int64_t t = spy_now();
        if (tick % 15 == 0) {
            for (int fd = 0; fd < SPY_FDS; fd++) {
                if (!spy[fd].on) continue;
                int waiting = 0;
                ioctl(fd, FIONREAD, &waiting);
                droid_log("NetSpy: #%d %s open %.0f s: %llu B out, %llu B in, last data %.0f s ago, %d B unread", fd,
                          spy_label(fd), (t - spy[fd].opened) / 1e9, spy[fd].out, spy[fd].in,
                          (t - spy[fd].last_in) / 1e9, waiting);
            }
        }
        for (int fd = 0; fd < SPY_FDS; fd++) {
            // a request sent since the last read: its answer waiting is a stall (a kept
            // connection the server talks on while it's idle is not)
            if (!spy[fd].on || spy[fd].stall_told) continue;
            int waiting = 0;
            if (ioctl(fd, FIONREAD, &waiting) != 0 || waiting <= 0) continue;
            double idle = (t - spy[fd].last_in) / 1e9;
            // a kept connection the server talks on while idle sends a little: a real stall is
            // an answer left there (a request since the last read) or a lot of data
            if (idle < 3 || (spy[fd].last_out <= spy[fd].last_in && waiting < 65536)) continue;
            spy[fd].stall_told = 1;
            droid_log("NetSpy: #%d %s: %d bytes arrived and wait unread for %.0f s - the game's network thread is "
                      "stuck somewhere (ThreadScope: what it does)", fd, spy_label(fd), waiting, idle);
        }
    }
    return NULL;
}

static void traffic(const char *what, int fd, size_t n, ssize_t r, int e) {
    if (r < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
        if (fd >= 0 && fd < 1024 && ++again_count[fd] % 5000 == 0 && traffic_lines < 600) {
            traffic_lines++;
            droid_log("net: %s(%d): nothing yet, %u times", what, fd, again_count[fd]);
        }
        return;
    }
    if (traffic_lines++ < 600)
        if (r < 0) droid_log("net: %s(%d, %zu) -> %zd errno %d", what, fd, n, r, e);
        else droid_log("net: %s(%d, %zu) -> %zd", what, fd, n, r);
}
static ssize_t n_send(int fd, const void *buf, size_t n, int flags) {
    ssize_t r = send(fd, buf, n, with_nb(fd, linux_msg_flags(flags)));
    int e = errno;
    spy_sent(fd, buf, r);
    traffic("send", fd, n, r, e);
    errno = e;
    return r;
}
static ssize_t n_recv(int fd, void *buf, size_t n, int flags) {
    ssize_t r = recv(fd, buf, n, with_nb(fd, linux_msg_flags(flags)));
    int e = errno;
    if (!(flags & 2)) spy_received(fd, buf, r); // not a MSG_PEEK
    traffic("recv", fd, n, r, e);
    errno = e;
    return r;
}

static ssize_t n_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, socklen_t len) {
    if (!addr) return send(fd, buf, n, with_nb(fd, linux_msg_flags(flags)));
    struct sockaddr_storage h;
    socklen_t hl;
    if (to_host_addr(addr, len, &h, &hl) != 0) return -1;
    return sendto(fd, buf, n, with_nb(fd, linux_msg_flags(flags)), (struct sockaddr *)&h, hl);
}

static ssize_t n_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, socklen_t *len) {
    struct sockaddr_storage h;
    socklen_t hl = sizeof(h);
    ssize_t r = recvfrom(fd, buf, n, with_nb(fd, linux_msg_flags(flags)), (struct sockaddr *)&h, &hl);
    if (!(flags & 2)) { // NetSpy: what comes in through recvfrom too (C#'s Socket reads so)
        int e = errno;
        spy_received(fd, buf, r);
        errno = e;
    }
    if (r >= 0 && addr && len) to_linux_addr((struct sockaddr *)&h, hl, addr, len);
    return r;
}

// Linux's (arm64) struct msghdr
typedef struct {
    void *name;
    uint32_t namelen;
    struct iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
} l_msghdr;

static ssize_t n_sendmsg(int fd, const l_msghdr *m, int flags) {
    struct msghdr h;
    memset(&h, 0, sizeof(h));
    struct sockaddr_storage a;
    socklen_t al = 0;
    if (m->name && to_host_addr(m->name, m->namelen, &a, &al) != 0) return -1;
    h.msg_name = m->name ? &a : NULL;
    h.msg_namelen = al;
    h.msg_iov = m->iov;
    h.msg_iovlen = (int)m->iovlen;
    if (m->controllen) NET_LOG("net: sendmsg with control data (left out)");
    return sendmsg(fd, &h, with_nb(fd, linux_msg_flags(flags)));
}

static ssize_t n_recvmsg(int fd, l_msghdr *m, int flags) {
    struct msghdr h;
    memset(&h, 0, sizeof(h));
    struct sockaddr_storage a;
    h.msg_name = m->name ? &a : NULL;
    h.msg_namelen = m->name ? sizeof(a) : 0;
    h.msg_iov = m->iov;
    h.msg_iovlen = (int)m->iovlen;
    ssize_t r = recvmsg(fd, &h, with_nb(fd, linux_msg_flags(flags)));
    if (r < 0) return r;
    if (m->name) {
        socklen_t ll = m->namelen;
        to_linux_addr((struct sockaddr *)&a, h.msg_namelen, m->name, &ll);
        m->namelen = ll;
    }
    m->controllen = 0;
    m->flags = (h.msg_flags & MSG_TRUNC) ? 0x20 : 0;
    return r;
}

static int n_shutdown(int fd, int how) { return shutdown(fd, how); }

// Socket options: (Linux level, Linux name) -> the host's. Unknown ones: accepted, ignored.
static int sockopt(int level, int name, int *hl, int *hn) {
    static const struct { int ll, ln, hl, hn; } map[] = {
        { 1, 2, SOL_SOCKET, SO_REUSEADDR }, { 1, 3, SOL_SOCKET, SO_TYPE }, { 1, 4, SOL_SOCKET, SO_ERROR },
        { 1, 5, SOL_SOCKET, SO_DONTROUTE }, { 1, 6, SOL_SOCKET, SO_BROADCAST }, { 1, 7, SOL_SOCKET, SO_SNDBUF },
        { 1, 8, SOL_SOCKET, SO_RCVBUF }, { 1, 9, SOL_SOCKET, SO_KEEPALIVE }, { 1, 10, SOL_SOCKET, SO_OOBINLINE },
        { 1, 13, SOL_SOCKET, SO_LINGER }, { 1, 18, SOL_SOCKET, SO_RCVLOWAT }, { 1, 19, SOL_SOCKET, SO_SNDLOWAT },
        { 1, 20, SOL_SOCKET, SO_RCVTIMEO }, { 1, 21, SOL_SOCKET, SO_SNDTIMEO }, { 1, 30, SOL_SOCKET, SO_ACCEPTCONN },
#ifdef SO_REUSEPORT
        { 1, 15, SOL_SOCKET, SO_REUSEPORT },
#endif
        { 6, 1, IPPROTO_TCP, TCP_NODELAY }, { 6, 2, IPPROTO_TCP, TCP_MAXSEG },
#ifdef TCP_KEEPIDLE
        { 6, 4, IPPROTO_TCP, TCP_KEEPIDLE }, { 6, 5, IPPROTO_TCP, TCP_KEEPINTVL }, { 6, 6, IPPROTO_TCP, TCP_KEEPCNT },
#endif
        { 0, 1, IPPROTO_IP, IP_TOS }, { 0, 2, IPPROTO_IP, IP_TTL },
        { 0, 32, IPPROTO_IP, IP_MULTICAST_IF }, { 0, 33, IPPROTO_IP, IP_MULTICAST_TTL },
        { 0, 34, IPPROTO_IP, IP_MULTICAST_LOOP }, { 0, 35, IPPROTO_IP, IP_ADD_MEMBERSHIP },
        { 0, 36, IPPROTO_IP, IP_DROP_MEMBERSHIP },
        { 41, 26, IPPROTO_IPV6, IPV6_V6ONLY }, { 41, 16, IPPROTO_IPV6, IPV6_UNICAST_HOPS },
        { 41, 17, IPPROTO_IPV6, IPV6_MULTICAST_IF }, { 41, 18, IPPROTO_IPV6, IPV6_MULTICAST_HOPS },
        { 41, 19, IPPROTO_IPV6, IPV6_MULTICAST_LOOP },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].ll == level && map[i].ln == name) {
            *hl = map[i].hl;
            *hn = map[i].hn;
            return 1;
        }
    return 0;
}

static int n_setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
    int hl, hn;
    if (!sockopt(level, name, &hl, &hn)) {
        NET_LOG("net: setsockopt(level %d, option %d): not mapped, ignored", level, name);
        return 0;
    }
    int rc = setsockopt(fd, hl, hn, val, len);
    NET_FAIL(rc, "net: setsockopt(%d, level %d, option %d) failed, errno %d", fd, level, name, errno);
    return rc;
}

static int n_getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
    int hl, hn;
    if (!sockopt(level, name, &hl, &hn)) {
        if (val && len && *len >= sizeof(int)) memset(val, 0, sizeof(int));
        return 0;
    }
    int rc = getsockopt(fd, hl, hn, val, len);
    NET_FAIL(rc, "net: getsockopt(%d, level %d, option %d) failed, errno %d", fd, level, name, errno);
    if (rc == 0 && hl == SOL_SOCKET && hn == SO_ERROR && val && *(int *)val)
        NET_LOG("net: socket %d error %d (connect's result)", fd, *(int *)val);
    if (rc == 0 && hl == SOL_SOCKET && hn == SO_ERROR && val) { // an errno: Linux's number
        extern int droid_linux_errno(int e);
        *(int *)val = droid_linux_errno(*(int *)val);
    }
    return rc;
}

// poll: Linux's bits -> the host's and back (POLLWRNORM / WRBAND differ on FreeBSD)
static short poll_to_host(short e) {
    short h = 0;
    if (e & 0x1) h |= POLLIN;
    if (e & 0x2) h |= POLLPRI;
    if (e & 0x4) h |= POLLOUT;
    if (e & 0x8) h |= POLLERR;
    if (e & 0x10) h |= POLLHUP;
    if (e & 0x20) h |= POLLNVAL;
    if (e & 0x40) h |= POLLRDNORM;
    if (e & 0x80) h |= POLLRDBAND;
    if (e & 0x100) h |= POLLWRNORM;
    if (e & 0x200) h |= POLLWRBAND;
    return h;
}
static short poll_to_linux(short h) {
    short e = 0;
    if (h & POLLIN) e |= 0x1;
    if (h & POLLPRI) e |= 0x2;
    if (h & POLLOUT) e |= 0x4;
    if (h & POLLERR) e |= 0x8;
    if (h & POLLHUP) e |= 0x10;
    if (h & POLLNVAL) e |= 0x20;
    if (h & POLLRDNORM) e |= 0x40;
    if (h & POLLRDBAND) e |= 0x80;
    if (h & POLLWRNORM) e |= 0x100;
    if (h & POLLWRBAND) e |= 0x200;
    return e;
}

#ifndef __linux__
// The console's poll misses sockets becoming readable, the peer closing above all: curl waited
// forever on Google's connection after its last bytes (Shadow Fight 2's internet check), and
// every request after it queued behind. So the sockets asked for POLLIN are also looked at
// ourselves (a peek that doesn't block): before poll waits, and between its 100 ms slices.
static int peek_ready(struct pollfd *fds, nfds_t n) {
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        if (fds[i].fd < 0 || !(fds[i].events & (POLLIN | POLLRDNORM))) continue;
        int type = 0;
        socklen_t tl = sizeof(type);
        if (getsockopt(fds[i].fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) continue;
        char c;
        ssize_t r = recv(fds[i].fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r > 0) fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
        else if (r == 0) fds[i].revents |= (fds[i].events & (POLLIN | POLLRDNORM)) | POLLHUP; // closed
        else continue;
        ready++;
    }
    return ready;
}
int arm_sig_waiting(void);           // arm_cpu.cpp: a signal this thread should take now
int droid_game_in(const char *list); // game_settings.c
#define SIGNAL_SAFE_WAIT_GAMES "com.nekki.vector" // (the same list as arm_imports.c's futex waits)
static int signal_safe_waits(void) {
    static int on = -1;
    if (on < 0) on = droid_game_in(SIGNAL_SAFE_WAIT_GAMES) != 0;
    return on;
}
// poll, and our peeks: what's ready (the host's bits), or 0 when the time ran out
static int poll_sockets(struct pollfd *fds, nfds_t n, int timeout) {
    int64_t left = timeout;
    for (;;) {
        for (nfds_t i = 0; i < n; i++) fds[i].revents = 0;
        int slice = timeout < 0 ? 100 : (int)(left < 100 ? left : 100);
        int r = poll(fds, n, peek_ready(fds, n) ? 0 : slice);
        if (r < 0) return r;
        int peeked = peek_ready(fds, n); // after the wait too: a close poll didn't say
        if (r > 0 || peeked) {
            int count = 0;
            for (nfds_t i = 0; i < n; i++) count += fds[i].revents != 0;
            return count;
        }
        // SIGNAL_SAFE_WAIT_GAMES: a signal for this thread (a GC's pause) ends the wait, as
        // Linux's EINTR - the SIGUSR2 that should have stopped it may have come between two
        // slices and been lost. Vector's story download: the download thread sat in poll(-1)
        // with the GC's pause pending, the GC waited for it for ever (2026-11-01).
        if (signal_safe_waits() && arm_sig_waiting()) {
            static int told;
            if (told++ < 30) droid_log("sig: a poll wait saw its pending signal (EINTR)");
            errno = EINTR;
            return -1;
        }
        if (timeout >= 0 && (left -= slice) <= 0) return 0;
    }
}
#endif

static int n_poll(struct pollfd *fds, nfds_t n, int timeout) {
    short keep[64];
    short *ev = n <= 64 ? keep : malloc(n * sizeof(short));
    if (!ev) {
        errno = ENOMEM;
        return -1;
    }
    for (nfds_t i = 0; i < n; i++) {
        ev[i] = fds[i].events;
        fds[i].events = poll_to_host(fds[i].events);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#ifdef __linux__
    int r = poll(fds, n, timeout);
#else
    int r = poll_sockets(fds, n, timeout);
#endif
    int pe = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long took = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    for (nfds_t i = 0; i < n; i++) {
        fds[i].events = ev[i];
        fds[i].revents = r > 0 ? poll_to_linux(fds[i].revents) : 0;
    }
    // what a waiting request polls for, and gets: the first ones, then every one that waited
    // long (over 500 ms) or failed (where a request that never finishes sits)
    static int poll_lines;
    if (n <= 6 && ((poll_lines < 60) || ((took > 500 || r < 0) && poll_lines < 400))) {
        poll_lines++;
        char t[320];
        int at = r < 0 ? snprintf(t, sizeof(t), "net: poll(%d ms, took %ld) -> %d (error, errno %d):", timeout, took, r, pe)
                       : snprintf(t, sizeof(t), "net: poll(%d ms, took %ld) -> %d:", timeout, took, r);
        for (nfds_t i = 0; i < n && at < (int)sizeof(t) - 30; i++)
            at += snprintf(t + at, sizeof(t) - at, " fd %d %#x->%#x", fds[i].fd, ev[i], fds[i].revents);
        droid_log("%s", t);
    }
    errno = pe;
    if (ev != keep) free(ev);
    return r;
}

// The network interfaces, as a phone on Wi-Fi lists them (SIOCGIF*: Linux's struct ifreq, 40
// bytes: the name, then the answer): lo 127.0.0.1 and wlan0. Genshin Impact took "not
// supported" for an empty list it never checked and wrote through it (BAD write at 0x51).
typedef struct {
    char name[16];
    union {
        struct { uint16_t family, port; uint8_t addr[4]; uint8_t zero[8]; } in; // sockaddr_in
        int16_t flags;
        int32_t index;
        struct { uint16_t family; uint8_t data[14]; } hw;
        uint8_t pad[24];
    } u;
} linux_ifreq;
static const struct { const char *name; uint8_t ip[4], mask[4]; int16_t flags; } ifaces[] = {
    { "lo", { 127, 0, 0, 1 }, { 255, 0, 0, 0 }, 0x49 },         // UP | LOOPBACK | RUNNING
    { "wlan0", { 192, 168, 1, 50 }, { 255, 255, 255, 0 }, 0x1043 }, // UP | BROADCAST | RUNNING | MULTICAST
};
static int iface_answer(unsigned long req, linux_ifreq *r) {
    for (int i = 0; i < 2; i++) {
        if (strncmp(r->name, ifaces[i].name, 16)) continue;
        memset(&r->u, 0, sizeof(r->u));
        switch (req) {
        case 0x8913: r->u.flags = ifaces[i].flags; return 0;                     // SIOCGIFFLAGS
        case 0x8915: r->u.in.family = 2; memcpy(r->u.in.addr, ifaces[i].ip, 4); return 0; // SIOCGIFADDR
        case 0x891b: r->u.in.family = 2; memcpy(r->u.in.addr, ifaces[i].mask, 4); return 0; // SIOCGIFNETMASK
        case 0x8919: r->u.in.family = 2; memcpy(r->u.in.addr, ifaces[i].ip, 3); r->u.in.addr[3] = 255; return 0; // BRDADDR
        case 0x8921: r->u.index = 1500; return 0;                                 // SIOCGIFMTU
        case 0x8933: r->u.index = i + 1; return 0;                                // SIOCGIFINDEX
        case 0x8927: r->u.hw.family = i ? 1 : 772; // SIOCGIFHWADDR: ether / loopback
            if (i) { static const uint8_t mac[6] = { 0x02, 0x00, 0x5e, 0x10, 0x20, 0x30 }; memcpy(r->u.hw.data, mac, 6); }
            return 0;
        }
    }
    errno = ENODEV;
    return -1;
}

static int n_ioctl(int fd, unsigned long req, void *arg) {
    if (req == 0x8912 && arg) { // SIOCGIFCONF: struct ifconf { int len; (pad) char *buf; }
        int *len = arg;
        linux_ifreq *buf = *(linux_ifreq **)((char *)arg + 8);
        int room = *len / (int)sizeof(linux_ifreq), n = 0;
        for (int i = 0; i < 2 && (!buf || n < room); i++, n++) {
            if (!buf) continue; // the size asked: all of them
            memset(&buf[n], 0, sizeof(buf[n]));
            snprintf(buf[n].name, 16, "%s", ifaces[i].name);
            buf[n].u.in.family = 2;
            memcpy(buf[n].u.in.addr, ifaces[i].ip, 4);
        }
        *len = n * (int)sizeof(linux_ifreq);
        return 0;
    }
    if ((req & 0xff00) == 0x8900 && arg) return iface_answer(req, arg); // the other SIOCGIF* of one name
    if (req == 0x541B) return ioctl(fd, FIONREAD, arg); // FIONREAD
    if (req == 0x5421) { // FIONBIO
        if (arg) net_nonblock_set(fd, *(int *)arg);
        return ioctl(fd, FIONBIO, arg);
    }
    NET_LOG("net: ioctl(%d, %#lx): not supported", fd, req);
    errno = ENOTTY;
    return -1;
}

// ---- names: getaddrinfo & co ------------------------------------------------------------

// bionic's struct addrinfo (BSD's field order, Linux's sockaddr inside)
typedef struct b_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct b_addrinfo *ai_next;
} b_addrinfo;

// bionic's EAI_* (BSD's numbers)
enum { B_EAI_AGAIN = 2, B_EAI_BADFLAGS = 3, B_EAI_FAIL = 4, B_EAI_FAMILY = 5, B_EAI_MEMORY = 6,
       B_EAI_NONAME = 8, B_EAI_SERVICE = 9, B_EAI_SOCKTYPE = 10, B_EAI_SYSTEM = 11 };

#ifdef __linux__ // the PC test runner: glibc's getaddrinfo answers
static int bionic_eai(int e) {
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return B_EAI_AGAIN;
    case EAI_BADFLAGS: return B_EAI_BADFLAGS;
    case EAI_FAIL: return B_EAI_FAIL;
    case EAI_FAMILY: return B_EAI_FAMILY;
    case EAI_MEMORY: return B_EAI_MEMORY;
    case EAI_NONAME: return B_EAI_NONAME;
    case EAI_SERVICE: return B_EAI_SERVICE;
    case EAI_SOCKTYPE: return B_EAI_SOCKTYPE;
    case EAI_SYSTEM: return B_EAI_SYSTEM;
    }
    return B_EAI_FAIL;
}

static int host_ai_flags(int f) { // bionic AI_* -> the host's
    int h = 0;
    if (f & 0x1) h |= AI_PASSIVE;
    if (f & 0x2) h |= AI_CANONNAME;
    if (f & 0x4) h |= AI_NUMERICHOST;
    if (f & 0x8) h |= AI_NUMERICSERV;
    if (f & 0x400) h |= AI_ADDRCONFIG;
    return h;
}
#endif

static b_addrinfo *new_node(const struct sockaddr *a, socklen_t alen, int socktype, int proto, const char *canon) {
    b_addrinfo *b = calloc(1, sizeof(b_addrinfo) + sizeof(struct sockaddr_storage));
    if (!b) return NULL;
    b->ai_family = linux_family(a->sa_family);
    b->ai_socktype = socktype;
    b->ai_protocol = proto;
    b->ai_addr = b + 1;
    socklen_t ll = sizeof(struct sockaddr_storage);
    to_linux_addr(a, alen, b->ai_addr, &ll);
    b->ai_addrlen = ll;
    if (canon) b->ai_canonname = strdup(canon);
    return b;
}

static void n_freeaddrinfo(b_addrinfo *b) {
    while (b) {
        b_addrinfo *next = b->ai_next;
        free(b->ai_canonname);
        free(b);
        b = next;
    }
}

#ifndef __linux__
// The console's resolver (libc's getaddrinfo may have no DNS servers in a title)
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int rid, const char *host, struct in_addr *addr, int timeout, int retry, int flags);
int sceNetResolverDestroy(int rid);

static int console_resolve(const char *host, struct in_addr *out) {
    static int pool = -1;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (pool < 0) pool = sceNetPoolCreate("droid_dns", 16 * 1024, 0);
    pthread_mutex_unlock(&lock);
    if (pool < 0) return -1;
    int rid = sceNetResolverCreate("droid_dns", pool, 0);
    if (rid < 0) return -1;
    int rc = sceNetResolverStartNtoa(rid, host, out, 0, 0, 0);
    sceNetResolverDestroy(rid);
    return rc;
}
#endif

// The console's DNS server may not answer (some point at one that blocks Sony's update
// servers; then the resolver fails for every name). Then the name is asked of public DNS
// servers directly, over UDP (Chrome-PS5 does the same): Cloudflare's, then Google's.
static int dns_ask(const char *server, const char *name, struct in_addr *out) {
    unsigned char q[300], r[1500];
    int n = 0;
    uint16_t id = (uint16_t)(rand() & 0xffff);
    q[n++] = id >> 8; q[n++] = id & 0xff;
    q[n++] = 0x01; q[n++] = 0x00; // a standard query, recursion wanted
    q[n++] = 0; q[n++] = 1;       // one question
    memset(q + n, 0, 6); n += 6;  // no answers, authorities, extras
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + (int)len + 6 > (int)sizeof(q)) return -1;
        q[n++] = (unsigned char)len;
        memcpy(q + n, p, len);
        n += (int)len;
        p += len;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; // type A
    q[n++] = 0; q[n++] = 1; // class IN
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
#ifndef __linux__
    to.sin_len = sizeof(to);
#endif
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, server, &to.sin_addr);
    int found = -1;
    for (int attempt = 0; attempt < 2 && found != 0; attempt++) {
        if (sendto(s, q, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to)) != n) break;
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, 1500) <= 0) continue;
        ssize_t got = recv(s, r, sizeof(r), 0);
        if (got < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 0x0f) != 0) continue;
        int answers = (r[6] << 8) | r[7];
        int at = n; // the answers follow our question, echoed
        for (int i = 0; i < answers && at + 12 <= got; i++) {
            if ((r[at] & 0xc0) == 0xc0) at += 2; // a name: a pointer, or labels
            else {
                while (at < got && r[at]) at += r[at] + 1;
                at++;
            }
            if (at + 10 > got) break;
            int type = (r[at] << 8) | r[at + 1], rdlen = (r[at + 8] << 8) | r[at + 9];
            at += 10;
            if (type == 1 && rdlen == 4 && at + 4 <= got) {
                memcpy(out, r + at, 4);
                found = 0;
                break;
            }
            at += rdlen; // a CNAME first: its target's A follows
        }
    }
    close(s);
    return found;
}

// Names already looked up (a game asks the same few again and again)
static struct { char name[128]; struct in_addr ip; } dns_cache[64];
static int dns_cached;
static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;

static int own_resolve(const char *name, struct in_addr *out) {
    static const char *const servers[] = { "1.1.1.1", "8.8.8.8", "1.0.0.1" };
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++)
        if (dns_ask(servers[i], name, out) == 0) {
            char shown[64];
            inet_ntop(AF_INET, out, shown, sizeof(shown));
            NET_LOG("net: resolve %s -> %s (DNS server %s)", name, shown, servers[i]);
            return 0;
        }
    return -1;
}

static int service_port(const char *service, int numeric_only) {
    if (!service || !*service) return 0;
    char *end;
    long p = strtol(service, &end, 10);
    if (*end == 0) return (int)p;
    if (numeric_only) return -1;
    if (!strcmp(service, "http")) return 80;
    if (!strcmp(service, "https")) return 443;
    if (!strcmp(service, "ftp")) return 21;
    return -1;
}

#ifndef __linux__
// The PS5: the SDK's libc getaddrinfo is a stub that always fails, so it's all here:
// no name (a server's "any", else loopback), a number (IPv4 / IPv6), or the console's
// resolver for a name (IPv4).
static int n_getaddrinfo(const char *node, const char *service, const b_addrinfo *hints, b_addrinfo **res) {
    int flags = hints ? hints->ai_flags : 0;
    int want = hints ? hints->ai_family : L_AF_UNSPEC;
    *res = NULL;
    if (want != L_AF_UNSPEC && want != L_AF_INET && want != L_AF_INET6) return B_EAI_FAMILY;
    int port = service_port(service, flags & 0x8);
    if (port < 0) return B_EAI_SERVICE;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;
    socklen_t len = sizeof(*v4);
    v4->sin_family = AF_INET;
    if (!node || !*node) {
        if (want == L_AF_INET6) {
            v6->sin6_family = AF_INET6;
            len = sizeof(*v6);
            if (!(flags & 0x1)) v6->sin6_addr = in6addr_loopback;
        } else {
            v4->sin_addr.s_addr = htonl((flags & 0x1) ? INADDR_ANY : INADDR_LOOPBACK);
        }
    } else if (want != L_AF_INET6 && inet_pton(AF_INET, node, &v4->sin_addr) == 1) {
        // a number
    } else if (want != L_AF_INET && inet_pton(AF_INET6, node, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        len = sizeof(*v6);
    } else if (flags & 0x4) { // AI_NUMERICHOST, and not a number
        return B_EAI_NONAME;
    } else if (want == L_AF_INET6) {
        return B_EAI_NONAME; // the resolver: IPv4 only
    } else {
        int r = -1;
        pthread_mutex_lock(&dns_lock);
        for (int i = 0; i < dns_cached && r != 0; i++)
            if (!strcmp(dns_cache[i].name, node)) {
                v4->sin_addr = dns_cache[i].ip;
                r = 0;
            }
        pthread_mutex_unlock(&dns_lock);
        if (r != 0) {
            // the console's resolver, until it fails once: then straight to the DNS servers
            // (it fails only after its timeout, seconds: Shadow Fight 2's requests gave up
            // first, "Cannot resolve destination host", and the game quit)
            static volatile int console_dns_bad;
            r = console_dns_bad ? -1 : console_resolve(node, &v4->sin_addr);
            if (r == 0) {
                char shown[64];
                inet_ntop(AF_INET, &v4->sin_addr, shown, sizeof(shown));
                NET_LOG("net: resolve %s -> %s", node, shown);
            } else {
                if (!console_dns_bad)
                    NET_LOG("net: resolve %s: the console's resolver failed (%#x): DNS servers from now on", node, r);
                console_dns_bad = 1;
                r = own_resolve(node, &v4->sin_addr);
            }
            if (r == 0 && strlen(node) < sizeof(dns_cache[0].name)) {
                pthread_mutex_lock(&dns_lock);
                int slot = dns_cached < 64 ? dns_cached++ : rand() % 64;
                snprintf(dns_cache[slot].name, sizeof(dns_cache[slot].name), "%s", node);
                dns_cache[slot].ip = v4->sin_addr;
                pthread_mutex_unlock(&dns_lock);
            }
        }
        if (r != 0) {
            NET_LOG("net: resolve %s failed", node);
            return B_EAI_NONAME;
        }
    }
    ((struct sockaddr *)&ss)->sa_len = (uint8_t)len;
    if (ss.ss_family == AF_INET) v4->sin_port = htons((uint16_t)port);
    else v6->sin6_port = htons((uint16_t)port);
    int types[2] = { SOCK_STREAM, SOCK_DGRAM };
    int nt = 2;
    if (hints && hints->ai_socktype) {
        types[0] = hints->ai_socktype;
        nt = 1;
    }
    b_addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < nt; i++) {
        b_addrinfo *b = new_node((struct sockaddr *)&ss, len, types[i],
                                 types[i] == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP, (flags & 0x2) && i == 0 ? node : NULL);
        if (!b) break;
        *tail = b;
        tail = &b->ai_next;
    }
    *res = head;
    return head ? 0 : B_EAI_MEMORY;
}
#else
// The PC test runner: glibc's getaddrinfo, its list rewritten in bionic's layout.
// DROID_OWN_DNS=1: names through our own DNS client instead (the console's fallback, tested).
static int n_getaddrinfo(const char *node, const char *service, const b_addrinfo *hints, b_addrinfo **res) {
    struct in_addr own;
    if (getenv("DROID_OWN_DNS") && node && inet_pton(AF_INET, node, &own) != 1 &&
        (!hints || hints->ai_family != L_AF_INET6) && own_resolve(node, &own) == 0) {
        int port = service_port(service, 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)(port < 0 ? 0 : port));
        sa.sin_addr = own;
        int type = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
        *res = new_node((struct sockaddr *)&sa, sizeof(sa), type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP, NULL);
        return *res ? 0 : B_EAI_MEMORY;
    }
    struct addrinfo h, *list = NULL;
    memset(&h, 0, sizeof(h));
    if (hints) {
        h.ai_flags = host_ai_flags(hints->ai_flags);
        h.ai_family = host_family(hints->ai_family);
        if (h.ai_family < 0) return B_EAI_FAMILY;
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
    }
    *res = NULL;
    int rc = getaddrinfo(node, service, hints ? &h : NULL, &list);
    if (rc != 0) {
        NET_LOG("net: getaddrinfo(%s, %s) failed: %d", node ? node : "-", service ? service : "-", rc);
        return bionic_eai(rc);
    }
    b_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        b_addrinfo *b = new_node(a->ai_addr, a->ai_addrlen, a->ai_socktype, a->ai_protocol,
                                 a == list ? a->ai_canonname : NULL);
        if (!b) break;
        *tail = b;
        tail = &b->ai_next;
    }
    freeaddrinfo(list);
    *res = head;
    NET_LOG("net: getaddrinfo(%s, %s) -> %s", node ? node : "-", service ? service : "-", head ? "found" : "nothing usable");
    return head ? 0 : B_EAI_NONAME;
}
#endif

static const char *n_gai_strerror(int e) {
    switch (e) {
    case B_EAI_AGAIN: return "Temporary failure in name resolution";
    case B_EAI_FAIL: return "Non-recoverable failure in name resolution";
    case B_EAI_FAMILY: return "ai_family not supported";
    case B_EAI_MEMORY: return "Memory allocation failure";
    case B_EAI_NONAME: return "No address associated with hostname";
    case B_EAI_SERVICE: return "Service not supported for socket type";
    case B_EAI_SOCKTYPE: return "Socket type not supported";
    }
    return "Unknown error";
}

// getnameinfo: numbers only (no reverse lookups: they could hang a game)
static int n_getnameinfo(const void *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv,
                         socklen_t servlen, int flags) {
    (void)flags;
    struct sockaddr_storage h;
    socklen_t hl;
    if (to_host_addr(sa, salen, &h, &hl) != 0) return B_EAI_FAMILY;
    const void *addr;
    int port;
    if (h.ss_family == AF_INET) {
        addr = &((struct sockaddr_in *)&h)->sin_addr;
        port = ntohs(((struct sockaddr_in *)&h)->sin_port);
    } else if (h.ss_family == AF_INET6) {
        addr = &((struct sockaddr_in6 *)&h)->sin6_addr;
        port = ntohs(((struct sockaddr_in6 *)&h)->sin6_port);
    } else {
        return B_EAI_FAMILY;
    }
    if (host && hostlen && !inet_ntop(h.ss_family, addr, host, hostlen)) return B_EAI_SYSTEM;
    if (serv && servlen) snprintf(serv, servlen, "%d", port);
    return 0;
}

static int n_inet_pton(int af, const char *src, void *dst) {
    int hf = host_family(af);
    if (hf != AF_INET && hf != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_pton(hf, src, dst);
}

static const char *n_inet_ntop(int af, const void *src, char *dst, socklen_t n) {
    int hf = host_family(af);
    if (hf != AF_INET && hf != AF_INET6) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    return inet_ntop(hf, src, dst, n);
}

// gethostbyname: through getaddrinfo, IPv4 (struct hostent: the same on both)
static struct hostent *n_gethostbyname(const char *name) {
    static __thread struct hostent he;
    static __thread struct in_addr ip;
    static __thread char *list[2];
    static __thread char hname[256];
    b_addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = L_AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (n_getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return NULL;
    memcpy(&ip, (char *)res->ai_addr + 4, 4); // Linux sockaddr_in: family, port, address
    n_freeaddrinfo(res);
    snprintf(hname, sizeof(hname), "%s", name);
    list[0] = (char *)&ip;
    list[1] = NULL;
    he.h_name = hname;
    he.h_aliases = list + 1;
    he.h_addrtype = L_AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}

#define N(name, fn) { name, (void *)fn }
// every name the game looks up, its hints and the answer (a game that never got a server:
// which call failed, and why)
static int logged_getaddrinfo(const char *node, const char *service, const b_addrinfo *hints, b_addrinfo **res) {
    if (settings.offline && node && strcmp(node, "localhost") && strcmp(node, "127.0.0.1")) { // offline
        *res = NULL;
        return B_EAI_NONAME;
    }
    static int shown;
    int log = shown < 60;
    if (log) {
        shown++;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (log)
        NET_LOG("net: getaddrinfo(%s, %s, family %d, type %d, flags %#x)... (t=%lld ms)", node ? node : "(null)",
                service ? service : "(null)", hints ? hints->ai_family : -1, hints ? hints->ai_socktype : -1,
                hints ? hints->ai_flags : 0, (long long)a.tv_sec * 1000 + a.tv_nsec / 1000000);
    int r = n_getaddrinfo(node, service, hints, res);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (log)
        NET_LOG("net: getaddrinfo(%s) -> %d in %ld ms", node ? node : "(null)", r,
                (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000));
    return r;
}

static const struct { const char *name; void *fn; } table[] = {
    N("socket", n_socket), N("socketpair", n_socketpair), N("connect", n_connect), N("bind", n_bind),
    N("listen", n_listen), N("accept", n_accept), N("accept4", n_accept4), N("getsockname", n_getsockname),
    N("getpeername", n_getpeername), N("send", n_send), N("recv", n_recv), N("sendto", n_sendto),
    N("recvfrom", n_recvfrom), N("sendmsg", n_sendmsg), N("recvmsg", n_recvmsg), N("shutdown", n_shutdown),
    N("setsockopt", n_setsockopt), N("getsockopt", n_getsockopt), N("poll", n_poll), N("ioctl", n_ioctl),
    N("getaddrinfo", logged_getaddrinfo), N("freeaddrinfo", n_freeaddrinfo), N("gai_strerror", n_gai_strerror),
    N("getnameinfo", n_getnameinfo), N("inet_pton", n_inet_pton), N("inet_ntop", n_inet_ntop),
    N("gethostbyname", n_gethostbyname),
};

void *net_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}

// A name's IPv4 address for Hydroid's own code (https.c): the console's resolver, then public
// DNS servers directly; the PC's libc there
int net_host_lookup(const char *name, struct in_addr *out) {
#ifndef __linux__
    if (console_resolve(name, out) == 0) return 0;
    return own_resolve(name, out);
#else
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
#endif
}
