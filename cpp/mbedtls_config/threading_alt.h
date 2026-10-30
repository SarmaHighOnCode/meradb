// The mutex Mbed TLS keeps in its structures with MBEDTLS_THREADING_ALT; src/tls.cpp makes it a std::mutex.
#pragma once
typedef struct mbedtls_threading_mutex_t {
    void* handle;
} mbedtls_threading_mutex_t;
