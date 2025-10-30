// http.h
#ifndef HTTP_H
#define HTTP_H
#include <stdint.h>

int http_get_by_ip(uint32_t ip, const char *host_header, const char *path,
                   char *out, int out_cap);

// Like http_get_by_ip but allows specifying a TCP port (host-order packed ip)
int http_get_by_ip_port(uint32_t ip, uint16_t port, const char *host_header, const char *path,
                        char *out, int out_cap);

// Debug info filled by http_get_by_ip()
extern int http_last_ret;       // last return code from http_get_by_ip()
extern int http_last_raw_len;   // number of raw bytes read from the socket
extern int http_last_tcp_state; // TCP state observed while attempting connect (set by http_get_by_ip)

#endif
