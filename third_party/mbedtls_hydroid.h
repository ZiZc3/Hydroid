// mbedtls_hydroid.h — mbedTLS 3.6 for Hydroid's own HTTPS (src/https.c): Firebase Remote Config
// for the games that use it. Our sockets (net_shim's), not mbedTLS's; TLS 1.2 (every Google API
// speaks it); random numbers from the platform's arc4random_buf (https.c's mbedtls_hardware_poll).
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
