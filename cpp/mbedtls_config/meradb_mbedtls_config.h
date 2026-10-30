// Added to Mbed TLS's own configuration (MBEDTLS_USER_CONFIG_FILE). The server runs each connection on its own thread,
// so the library must lock what they share; ALT, because a MinGW or MSVC build has no pthreads to rely on.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
