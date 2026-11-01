#ifndef HTTPS_H
#define HTTPS_H

#include <stddef.h>

// An HTTPS POST from Hydroid's own code (https.c): headers as "Name: value\r\n" lines; the
// response's body (malloc'd, NUL-terminated) and its HTTP status, NULL when it couldn't connect
char *https_post(const char *host, const char *path, const char *headers, const char *body, int *status);

// Any request (a game's service, a download). Bodies are bytes (with lengths);
// redirects (301/302/303/307/308) are followed. With a sink, the body goes to it as it comes
// (a download: nothing kept in memory); sink returns 0 to stop. from > 0 asks for the rest from
// that byte (resume: a 206 answer goes on from there)
typedef struct {
    const char *method;  // "GET", "POST"
    const char *url;     // "https://host/path?query"
    const char *headers; // extra "Name: value\r\n" lines, or NULL
    const void *body;    // or NULL
    size_t body_len;
    long long from;                                       // resume, or 0
    long long to;                                         // a part only: up to this byte (not in it), or 0
    int (*sink)(void *ctx, const void *data, size_t len); // or NULL: the body in https_resp
    void *sink_ctx;
    // before the body (with a sink): the answer's status and the body's length (-1 not said); 0
    // stops it (a resume a server answered with the whole file again: 200, not 206)
    int (*head)(void *ctx, int status, long long length);
    // keep the connection for this thread's next request to the same server (pictures:
    // no new TLS handshake each time); a kept one the server closed is opened again by itself
    int keep;
} https_req;

typedef struct {
    int status;      // the HTTP status, 0 when it couldn't connect
    char *body;      // without a sink: malloc'd, NUL-terminated (it can hold NULs: use len)
    size_t len;
    long long total; // the body's Content-Length (the whole file's for a 206), -1 not said
    char *location;  // the last redirect's target (malloc'd), or NULL
} https_resp;

int https_request(const https_req *r, https_resp *out); // 1 when an answer came (any status)
void https_resp_free(https_resp *out);

#endif
