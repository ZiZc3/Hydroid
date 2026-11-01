// imangi_xls.c — Imangi's ImangiXLSHelper (com.imangi.imangiutilities, Java we don't have), as its
// code is (apktool of Temple Run 2 1.137): the game's shipped config files come to Java as text,
// Java decodes them and answers through UnitySendMessage("ImangiXLSHelper", "OnParse(List)Complete").
// Temple Run 2 waited on its loading picture for that answer.
//   Parse(text):     DoParse(text)
//   ParseList(text): Base64 -> [n][4-byte lengths, interleaved][bytes, interleaved] -> each one
//                    DoParse -> a JSON array of the results
//   DoParse(text):   AES-128-CBC/PKCS7; key = BigInteger(1, SHA-512(secret)).toByteArray()[18..34),
//                    IV = Base64(text[0..24)), data = Base64 of the rest in 1024-character pieces
// A failure gives null (Java's catch), sent as "" (its onPostExecute).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "imangi_xls.h"
#include "apk.h"
#include "android_shim.h"

// ---- SHA-512 ------------------------------------------------------------------------------------
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
    0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
    0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
    0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
    0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
    0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t h[8], const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 80; i++) {
        const uint64_t t1 = k + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        const uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

static void sha512(const uint8_t *m, size_t n, uint8_t out[64]) {
    uint64_t h[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                      0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
    size_t i = 0;
    for (; i + 128 <= n; i += 128) sha512_block(h, m + i);
    uint8_t last[256] = { 0 };
    const size_t r = n - i;
    memcpy(last, m + i, r);
    last[r] = 0x80;
    const size_t total = r + 17 <= 128 ? 128 : 256;
    const uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) last[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    sha512_block(h, last);
    if (total == 256) sha512_block(h, last + 128);
    for (int j = 0; j < 8; j++)
        for (int b = 0; b < 8; b++) out[j * 8 + b] = (uint8_t)(h[j] >> (56 - 8 * b));
}

// ---- AES-128 decryption -------------------------------------------------------------------------
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
    0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
    0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
    0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static uint8_t INV_SBOX[256];

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

static void aes128_expand(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            const uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon, t[1] = SBOX[t[2]], t[2] = SBOX[t[3]], t[3] = SBOX[u];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}

static void aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
    for (int round = 9; round >= 0; round--) {
        uint8_t t[16]; // inverse shift rows (state column-major: s[c*4+r])
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
        for (int i = 0; i < 16; i++) s[i] = INV_SBOX[t[i]] ^ rk[round * 16 + i];
        if (round == 0) break;
        for (int c = 0; c < 4; c++) { // inverse mix columns
            const uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
            s[c * 4 + 0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            s[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            s[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
            s[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
    }
    memcpy(out, s, 16);
}

// ---- Base64 (Android's DEFAULT decoder: whitespace skipped, '=' ends it; anything else: an error) --
static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// decodes n characters into out (room for n * 3 / 4 + 3); the length, or -1 on a bad character
static long b64_decode(const char *in, size_t n, uint8_t *out) {
    uint32_t acc = 0;
    int bits = 0;
    long o = 0;
    for (size_t i = 0; i < n; i++) {
        const int c = (unsigned char)in[i];
        if (c == '=') break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const int v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

// ---- DoParse ------------------------------------------------------------------------------------
// Its key is a text in the game's own code (ImangiXLSHelper). It isn't kept here: the game's
// classes*.dex are searched for an 80-character text with this fingerprint (the first 16 bytes
// of SHA-512("hydroid-fp:" + the text): it can't be turned back into the key).
#define SECRET_LEN 80
static const uint8_t SECRET_FP[16] = { 0x78, 0x5f, 0xd2, 0x4c, 0x4b, 0xdb, 0x8d, 0xd8,
                                       0xca, 0x8d, 0xf8, 0xd7, 0x04, 0x77, 0x30, 0x5f };
static char SECRET[SECRET_LEN + 1];
static int secret_state; // 0 not looked for, 1 found, -1 not in this game

static void sha512(const uint8_t *msg, size_t len, uint8_t out[64]);
static int is_secret(const uint8_t *p) {
    uint8_t buf[11 + SECRET_LEN], d[64];
    memcpy(buf, "hydroid-fp:", 11);
    memcpy(buf + 11, p, SECRET_LEN);
    sha512(buf, sizeof(buf), d);
    return memcmp(d, SECRET_FP, sizeof(SECRET_FP)) == 0;
}
// a dex string: its length as ULEB128 (80: one byte 0x50), the text, a 0 byte
static void secret_visit(const char *name, void *ctx) {
    (void)ctx;
    const size_t nl = strlen(name);
    if (secret_state == 1 || nl < 4 || strcmp(name + nl - 4, ".dex") || strchr(name, '/')) return;
    size_t size = 0;
    uint8_t *dex = apk_load(name, &size);
    if (!dex) return;
    for (size_t i = 0; i + SECRET_LEN + 2 <= size && secret_state != 1; i++) {
        if (dex[i] != SECRET_LEN || dex[i + 1 + SECRET_LEN] != 0) continue;
        size_t k = 0;
        while (k < SECRET_LEN && dex[i + 1 + k] >= 0x20 && dex[i + 1 + k] < 0x7f) k++;
        if (k == SECRET_LEN && is_secret(dex + i + 1)) {
            memcpy(SECRET, dex + i + 1, SECRET_LEN);
            secret_state = 1;
        }
    }
    free(dex);
}
static int find_secret(void) {
    if (secret_state) return secret_state == 1;
    secret_state = -1;
    apk_each("classes", secret_visit, NULL);
    droid_log("imangi: the XLS key %s in the game's code", secret_state == 1 ? "found" : "NOT found");
    return secret_state == 1;
}

static int key_ready;
static uint8_t round_keys[176];

static int key_setup(void) {
    if (key_ready) return 1;
    if (!find_secret()) return 0;
    for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
    uint8_t digest[64], big[65];
    sha512((const uint8_t *)SECRET, strlen(SECRET), digest);
    // BigInteger(1, digest).toByteArray(): leading zero bytes gone, a 0 byte first if the top bit is set
    int start = 0;
    while (start < 63 && digest[start] == 0) start++;
    int n = 0;
    if (digest[start] & 0x80) big[n++] = 0;
    memcpy(big + n, digest + start, (size_t)(64 - start));
    n += 64 - start;
    uint8_t key[16] = { 0 };
    for (int i = 0; i < 16; i++) key[i] = 18 + i < n ? big[18 + i] : 0; // Arrays.copyOfRange(18, 34)
    aes128_expand(key, round_keys);
    key_ready = 1;
    return 1;
}

static char *do_parse(const char *text, size_t len) {
    if (!key_setup()) return NULL;
    if (len < 24) return NULL;
    uint8_t iv[20];
    if (b64_decode(text, 24, iv) != 16) return NULL; // IvParameterSpec: 16 bytes
    uint8_t *data = malloc(len * 3 / 4 + 1024);
    if (!data) return NULL;
    long have = 0;
    for (size_t at = 24; at < len;) {
        const size_t piece = len - at < 1024 ? len - at : 1024;
        const long got = b64_decode(text + at, piece, data + have);
        if (got < 0) {
            free(data);
            return NULL;
        }
        have += got;
        at += piece;
    }
    if (have == 0 || have % 16) {
        free(data);
        return NULL;
    }
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (long i = 0; i < have; i += 16) {
        uint8_t plain[16], cipher[16];
        memcpy(cipher, data + i, 16);
        aes128_decrypt_block(round_keys, cipher, plain);
        for (int j = 0; j < 16; j++) data[i + j] = plain[j] ^ prev[j];
        memcpy(prev, cipher, 16);
    }
    const int pad = data[have - 1];
    if (pad < 1 || pad > 16 || pad > have) {
        free(data);
        return NULL;
    }
    for (int j = 1; j <= pad; j++)
        if (data[have - j] != pad) {
            free(data);
            return NULL;
        }
    have -= pad;
    char *out = malloc((size_t)have + 1);
    if (out) {
        memcpy(out, data, (size_t)have);
        out[have] = 0;
    }
    free(data);
    return out;
}

char *imangi_xls_parse(const char *text) { return text ? do_parse(text, strlen(text)) : NULL; }

// ---- ParseList ----------------------------------------------------------------------------------
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;
static void put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void put_json_string(sbuf *b, const char *s) {
    put(b, "\"", 1);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char e[8];
        switch (c) {
        case '"': put(b, "\\\"", 2); break;
        case '\\': put(b, "\\\\", 2); break;
        case '\n': put(b, "\\n", 2); break;
        case '\r': put(b, "\\r", 2); break;
        case '\t': put(b, "\\t", 2); break;
        case '\b': put(b, "\\b", 2); break;
        case '\f': put(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                e[0] = '\\', e[1] = 'u', e[2] = '0', e[3] = '0', e[4] = hex[c >> 4], e[5] = hex[c & 15];
                put(b, e, 6);
            } else {
                put(b, (const char *)&c, 1);
            }
        }
    }
    put(b, "\"", 1);
}

char *imangi_xls_parse_list(const char *text, int *count) {
    if (count) *count = 0;
    if (!text) return NULL;
    const size_t len = strlen(text);
    uint8_t *raw = malloc(len * 3 / 4 + 4);
    if (!raw) return NULL;
    const long have = b64_decode(text, len, raw);
    if (have < 1) {
        free(raw);
        return NULL;
    }
    const int n = raw[0];
    sbuf out = { 0 };
    put(&out, "[", 1);
    for (int i = 0; i < n; i++) {
        long idx = i + 1;
        if (idx + 3L * n >= have) goto bad;
        const uint32_t size = ((uint32_t)raw[idx] << 24) | ((uint32_t)raw[idx + n] << 16) |
                              ((uint32_t)raw[idx + 2 * n] << 8) | raw[idx + 3 * n];
        long at = idx + 4L * n;
        if ((uint64_t)at + (uint64_t)size * (uint64_t)n > (uint64_t)have + (uint64_t)n) goto bad;
        char *one = malloc((size_t)size + 1);
        if (!one) goto bad;
        for (uint32_t k = 0; k < size; k++, at += n) {
            if (at >= have) {
                free(one);
                goto bad;
            }
            one[k] = (char)raw[at];
        }
        one[size] = 0;
        char *parsed = do_parse(one, size);
        free(one);
        if (i) put(&out, ",", 1);
        if (parsed) put_json_string(&out, parsed);
        else put(&out, "null", 4);
        free(parsed);
    }
    put(&out, "]", 1);
    free(raw);
    if (count) *count = n;
    return out.p;
bad: // Java's catch: the whole answer null
    free(raw);
    free(out.p);
    return NULL;
}
