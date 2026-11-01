#pragma once
#include <stddef.h>

// The APK's signing certificate as Base64(SHA-1(certificate)) (what a game gets from
// Signature.toByteArray() hashed): 0, or -1 when the APK has no v2/v3 signing block.
int apk_signature(const char *apk, char *out, size_t out_size);

// The certificate's SHA-1 as hex (X-Android-Cert); after apk_signature
int apk_cert_sha1_hex(char out[41]);
