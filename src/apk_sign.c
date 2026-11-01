// apk_sign.c — the APK's signing certificate, as Android's PackageManager gives it to a
// game: games hash it (SHA-1, Base64) to check they're the real thing, and some use it as
// a key (Shadow Fight 2: without it a C# ArgumentNullException stops the start).
// Store APKs are signed with an APK Signing Block (v2/v3) just before the zip's central
// directory; the first signer's first certificate (DER) is the one Android reports.

#include "apk_sign.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- SHA-1 -------------------------------------------------------------------------------
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t b[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + i;
            b[i] = p < len ? data[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total) {
            uint64_t bits = (uint64_t)len * 8;
            for (int i = 0; i < 8; i++) b[63 - i] = (uint8_t)(bits >> (8 * i));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else f = bb ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(bb, 30), bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static void base64(const uint8_t *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

// ---- the signing block -------------------------------------------------------------------
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

// a length-prefixed (u32) item at *p inside [*p, end): its start and size; *p moves past it
static int item(const uint8_t **p, const uint8_t *end, const uint8_t **start, uint32_t *size) {
    if (end - *p < 4) return -1;
    uint32_t n = u32(*p);
    if ((size_t)(end - *p - 4) < n) return -1;
    *start = *p + 4;
    *size = n;
    *p += 4 + n;
    return 0;
}

// the first certificate of the first signer, in a v2 / v3 scheme value
static int first_cert(const uint8_t *v, uint32_t vn, const uint8_t **cert, uint32_t *cn) {
    const uint8_t *p = v, *end = v + vn, *signers, *signer, *signed_data, *digests, *certs;
    uint32_t sn, gn, dn, dgn, csn;
    if (item(&p, end, &signers, &sn)) return -1; // the signers
    p = signers;
    if (item(&p, signers + sn, &signer, &gn)) return -1; // the first signer
    p = signer;
    if (item(&p, signer + gn, &signed_data, &dn)) return -1; // its signed data
    p = signed_data;
    if (item(&p, signed_data + dn, &digests, &dgn)) return -1; // digests, then certificates
    if (item(&p, signed_data + dn, &certs, &csn)) return -1;
    p = certs;
    return item(&p, certs + csn, cert, cn);
}

// the certificate itself (DER), as PackageManager's Signature.toByteArray gives it
unsigned char *droid_apk_cert;
unsigned droid_apk_cert_len;

int apk_signature(const char *apk, char *out, size_t out_size) {
    if (out_size) out[0] = 0;
    FILE *f = fopen(apk, "rb");
    if (!f) return -1;
    int rc = -1;
    uint8_t *block = NULL;
    // the end of central directory record (no zip comment in store APKs; up to 64 KB allowed)
    uint8_t tail[65536 + 22];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tn = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
    fseek(f, size - tn, SEEK_SET);
    if (fread(tail, 1, (size_t)tn, f) != (size_t)tn) goto out;
    long eocd = -1;
    for (long i = tn - 22; i >= 0; i--)
        if (u32(tail + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) goto out;
    uint32_t cd = u32(tail + eocd + 16);
    uint8_t foot[24]; // the block's size and its magic, just before the central directory
    if (cd < 24 || fseek(f, cd - 24, SEEK_SET) || fread(foot, 1, 24, f) != 24) goto out;
    if (memcmp(foot + 8, "APK Sig Block 42", 16)) goto out; // not v2/v3 signed
    uint64_t bn = u64(foot); // the pairs and this footer
    if (bn < 24 || bn > cd || bn > (64u << 20)) goto out;
    block = malloc(bn);
    if (!block || fseek(f, (long)(cd - bn), SEEK_SET) || fread(block, 1, bn, f) != bn) goto out;
    // the pairs: u64 length, u32 id, value; v3 (0xf05368c0) holds the current certificate
    const uint8_t *cert = NULL;
    uint32_t cn = 0;
    for (int want = 0; want < 2 && !cert; want++) {
        uint32_t id = want == 0 ? 0xf05368c0 : 0x7109871a;
        for (uint64_t p = 0; p + 12 <= bn - 24;) {
            uint64_t len = u64(block + p);
            if (len < 4 || p + 8 + len > bn - 24) break;
            if (u32(block + p + 8) == id && first_cert(block + p + 12, (uint32_t)(len - 4), &cert, &cn) == 0) break;
            cert = NULL;
            p += 8 + len;
        }
    }
    if (!cert || out_size < 29) goto out;
    free(droid_apk_cert);
    droid_apk_cert = malloc(cn);
    if (droid_apk_cert) {
        memcpy(droid_apk_cert, cert, cn);
        droid_apk_cert_len = cn;
    }
    uint8_t digest[20];
    sha1(cert, cn, digest);
    base64(digest, 20, out);
    rc = 0;
out:
    free(block);
    fclose(f);
    return rc;
}

// The certificate's SHA-1 as 40 upper-case hex digits (Google's APIs ask for it as
// X-Android-Cert): 0, or -1 before apk_signature found one
int apk_cert_sha1_hex(char out[41]) {
    out[0] = 0;
    if (!droid_apk_cert || !droid_apk_cert_len) return -1;
    uint8_t d[20];
    sha1(droid_apk_cert, droid_apk_cert_len, d);
    for (int i = 0; i < 20; i++) snprintf(out + i * 2, 3, "%02X", d[i]);
    return 0;
}
