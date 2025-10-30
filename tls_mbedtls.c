// Minimal mbedTLS glue for freestanding kernel
// Provides: int tls_http_get_by_ip(uint32_t ip, const char *host_header, const char *path, char *out, int out_cap);

#include "tls.h"
#include "tcp.h"
#include "stdlib.h"
#include "string.h"
#include "io.h" // for debug prints if needed
#include "rtl8139.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "vendor/mbedtls/include/mbedtls/platform_stubs.h"

#include <stdio.h>

/* Provide ssize_t for freestanding build */
typedef int ssize_t;
#include "vendor/mbedtls/include/mbedtls/net_sockets.h"

// Expose mbedTLS debug buffer accessor from platform_shim
extern const char *mbedtls_get_debug(void);

// No custom calloc/free here — use the kernel's malloc/free implementations (kmalloc_stub.c)

// Provide a simple time stub used by some parts of mbedTLS when macros expect it
long mbedtls_time_stub(long *t){ if(t) *t = 0; return 0; }

// mbedTLS debug callback: append module/level/message to platform buffer via mbedtls_printf
static void mbedtls_debug_cb(void *ctx, int level, const char *file, int line, const char *str){
    (void)ctx; (void)file; (void)line; (void)level;
    mbedtls_printf("%s", str);
}

// TCP send/recv wrappers used by mbedTLS
static int net_send(void *ctx, const unsigned char *buf, size_t len){
    tcp_socket_t *s = (tcp_socket_t*)ctx;
    int ret = tcp_send(s, (const char*)buf, (int)len);
    if(ret < 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return ret;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len){
    tcp_socket_t *s = (tcp_socket_t*)ctx;
    // call tcp_recv which is non-blocking in this kernel design: 0 = nothing yet
    int ret = tcp_recv(s, (char*)buf, (int)len);
    if(ret == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if(ret < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    return ret;
}

int tls_http_get_by_ip(uint32_t ip, const char *host_header, const char *path, char *out, int out_cap){
    if(ip==0) return -1;

    tcp_socket_t *s = malloc(sizeof(tcp_socket_t));
    if(!s) return -1;
    memset(s,0,sizeof(*s));

    if(tcp_connect(s, ip, 443, 0) != 0){ free(s); return -1; }

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    const char *pers = "mbed_tls_kern";
    mbedtls_ctr_drbg_init(&ctr_drbg);

    /* Lightweight internal entropy callback for freestanding testing.
     * Not cryptographically strong but sufficient for functional testing. */
    static uint32_t lfsr = 0x12345678;
    int my_entropy(void *data, unsigned char *buf, size_t len){
        (void)data;
        for(size_t i=0;i<len;i++){
            lfsr ^= lfsr << 13;
            lfsr ^= lfsr >> 17;
            lfsr ^= lfsr << 5;
            buf[i] = (unsigned char)(lfsr & 0xFF);
        }
        return 0;
    }

    if(mbedtls_ctr_drbg_seed(&ctr_drbg, my_entropy, NULL, (const unsigned char*)pers, strlen(pers)) != 0){
        free(s); return -1; }

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    if(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0){
        mbedtls_ssl_config_free(&conf); free(s); return -1; }

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    // enable debug callback (will append into platform_shim buffer)
    mbedtls_ssl_conf_dbg(&conf, mbedtls_debug_cb, NULL);

    // disable certificate verification for prototype
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);

    if(mbedtls_ssl_setup(&ssl, &conf) != 0){ mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); free(s); return -1; }

    // set SNI / hostname for server cert verification and proper TLS handshake
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    if(host_header && host_header[0]){
        mbedtls_ssl_set_hostname(&ssl, host_header);
    }
#endif

    mbedtls_ssl_set_bio(&ssl, s, net_send, net_recv, NULL);

    // perform handshake (simple loop with NIC polling)
    int ret;
    while((ret = mbedtls_ssl_handshake(&ssl)) != 0){
        if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE){
            // pump NIC to advance TCP state
            if (rtl8139_is_ready()) rtl8139_poll();
            continue;
        }
        // failure
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        free(s);
        return -1;
    }

    // Build GET request
    char req[512];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host_header);
    if(n < 0){ goto cleanup_err; }

    // write request
    int w = mbedtls_ssl_write(&ssl, (unsigned char*)req, n);
    if(w <= 0) goto cleanup_err;

    // read response into out buffer
    int total = 0;
    while(total < out_cap - 1){
        int r = mbedtls_ssl_read(&ssl, (unsigned char*)(out + total), out_cap - 1 - total);
        if(r > 0){ total += r; continue; }
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE){
            if (rtl8139_is_ready()) rtl8139_poll();
            continue;
        }
        break; // EOF or error
    }
    out[total] = '\0';

    // cleanup and return
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    tcp_close(s);
    free(s);
    return total;

cleanup_err:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    tcp_close(s);
    free(s);
    return -1;
}
