// http.c -- minimal HTTP GET over your tcp.c layer (no DNS)
// Exposes: int http_get_by_ip(uint32_t ip, const char *host_header, const char *path, char *out, int out_cap)
// - ip: IPv4 in host-order packed as (A<<24)|(B<<16)|(C<<8)|D
// - host_header: used for the Host: request header
// - path: e.g. "/"
// - out: buffer to receive HTTP body (NOT headers); will be null-terminated if >0 bytes returned
// - returns: number of bytes written into out, 0 = nothing, negative = error

#include "http.h"
#include "tcp.h"
#include "string.h"
#include <stdint.h>
#include "rtl8139.h"   // needed so we can poll NIC during waits

// Debug exports
int http_last_ret = 0;
int http_last_raw_len = 0;
int http_last_tcp_state = 0;

// Helper: parse dotted IPv4 from string without stdio. Returns 1 on success and sets *out.
static int parse_ipv4_literal(const char *s, uint32_t *out){
    if (!s) return 0;
    unsigned parts[4] = {0,0,0,0};
    int p = 0;
    const char *cur = s;
    while (*cur && p < 4){
        // parse number
        if (*cur < '0' || *cur > '9') return 0;
        unsigned val = 0;
        while (*cur >= '0' && *cur <= '9'){
            val = val * 10 + (unsigned)(*cur - '0');
            cur++;
        }
        if (val > 255) return 0;
        parts[p++] = val;
        if (*cur == '.') cur++;
        else break;
    }
    if (p != 4) return 0;
    *out = ((uint32_t)parts[0]<<24)|((uint32_t)parts[1]<<16)|((uint32_t)parts[2]<<8)|((uint32_t)parts[3]);
    return 1;
}

// Helper: parse HTTP status code from raw response start. Returns 0 if not found.
static int parse_http_status(const char *raw){
    if (!raw) return 0;
    if (strncmp(raw, "HTTP/", 5) != 0) return 0;
    const char *p = raw + 5;
    // skip version digits and slash/dot until space
    while (*p && *p != ' ') p++;
    if (!*p) return 0;
    // skip space
    p++;
    int status = 0;
    while (*p >= '0' && *p <= '9'){
        status = status * 10 + (*p - '0');
        p++;
    }
    return status;
}

int http_get_by_ip(uint32_t ip, const char *host_header, const char *path,
                   char *out, int out_cap)
{
    return http_get_by_ip_port(ip, 80, host_header, path, out, out_cap);
}

// New: support connecting to a specific TCP port (host-order packed ip)
int http_get_by_ip_port(uint32_t ip, uint16_t port, const char *host_header, const char *path,
                   char *out, int out_cap)
{
    if (!host_header || !path || !out || out_cap <= 0) { http_last_ret = -1; return -1; }

    // make local mutable buffers for possible redirect updates
    char hostbuf[128];
    char pathbuf[1024];
    strncpy(hostbuf, host_header, sizeof(hostbuf)-1); hostbuf[sizeof(hostbuf)-1]=0;
    strncpy(pathbuf, path, sizeof(pathbuf)-1); pathbuf[sizeof(pathbuf)-1]=0;

    // init debug
    http_last_ret = -999;
    http_last_raw_len = 0;

    const int MAX_REDIRECTS = 5;
    for (int redir = 0; redir <= MAX_REDIRECTS; ++redir){
        tcp_init();
        http_last_tcp_state = 0;
        if (tcp_connect(&g_sock, ip, port, 0) != 0) { http_last_ret = -2; return -2; }

        // wait for SYN/ACK with a longer timeout
        int waited = 0;
        while (g_sock.state != TCP_ESTABLISHED && waited < 300000) {
            rtl8139_poll();
            http_last_tcp_state = g_sock.state;
            waited++;
        }
        if (g_sock.state != TCP_ESTABLISHED){ tcp_close(&g_sock); http_last_ret = -3; return -3; }

        // build request
        char req[512];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: MyOS/0.1\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         pathbuf, hostbuf);
        if (n <= 0) { tcp_close(&g_sock); http_last_ret = -4; return -4; }

        tcp_send(&g_sock, req, n);

        // Read raw HTTP into a temporary buffer and then extract body.
        int total_raw = 0;
        #define RAW_CAP 32768
        static char raw[RAW_CAP];

        while (total_raw < RAW_CAP - 1) {
            rtl8139_poll(); // pump NIC regularly so we actually receive TCP segments

            int got = tcp_recv(&g_sock, raw + total_raw, RAW_CAP - 1 - total_raw);
            if (got > 0) {
                total_raw += got;
            } else {
                // break if connection closing
                if (g_sock.state == TCP_FIN_WAIT1 || g_sock.state == TCP_FIN_WAIT2 || g_sock.state == TCP_TIME_WAIT)
                    break;
                // otherwise spin and let poll bring in more packets
            }
        }
        raw[total_raw] = '\0';
        tcp_close(&g_sock);

        http_last_raw_len = total_raw;

        if (total_raw <= 0) { http_last_ret = 0; return 0; }

        // Check status code for redirects
        int status = parse_http_status(raw);
        if (status >= 300 && status < 400){
            // find Location header
            char *loc = strstr(raw, "Location:");
            if (!loc) loc = strstr(raw, "location:");
            if (!loc){ http_last_ret = -12; return -12; }
            // skip to value
            loc = strchr(loc, ':'); if(!loc) { http_last_ret = -12; return -12; }
            loc++;
            while (*loc == ' ' || *loc == '\t') loc++;

            // copy location into small buffer
            char locbuf[1024]; int li=0;
            while (*loc && *loc!='\r' && *loc!='\n' && li < (int)sizeof(locbuf)-1) locbuf[li++]=*loc++;
            locbuf[li]=0;

            // if https -> give up (no TLS)
            if (strncmp(locbuf, "https://", 8) == 0){ http_last_ret = -11; return -11; }

            // if absolute http://host/... then parse host and path
            if (strncmp(locbuf, "http://", 7) == 0){
                const char *p = locbuf + 7;
                // extract host (up to / or :)
                char newhost[128]; int ni=0;
                while (*p && *p != '/' && *p != ':' && ni < (int)sizeof(newhost)-1) newhost[ni++] = *p++;
                newhost[ni]=0;
                // extract path
                const char *newpath = p;
                if (!*newpath) newpath = "/";

                // try parse host as dotted IP
                uint32_t newip;
                if (parse_ipv4_literal(newhost, &newip)){
                    ip = newip; // update ip to the literal and keep Host header as original or set to newhost
                    strncpy(hostbuf, newhost, sizeof(hostbuf)-1); hostbuf[sizeof(hostbuf)-1]=0;
                    strncpy(pathbuf, newpath, sizeof(pathbuf)-1); pathbuf[sizeof(pathbuf)-1]=0;
                    // follow redirect
                    continue;
                } else {
                    // host is a domain name different from our host_header -> we cannot resolve
                    // If host equals current host header, just update path and continue
                    if (strcmp(newhost, hostbuf) == 0){
                        strncpy(pathbuf, newpath, sizeof(pathbuf)-1); pathbuf[sizeof(pathbuf)-1]=0;
                        continue;
                    }
                    http_last_ret = -13; // redirect to different hostname (no DNS)
                    return -13;
                }
            }

            // if location is absolute path starting with '/'
            if (locbuf[0] == '/'){
                strncpy(pathbuf, locbuf, sizeof(pathbuf)-1); pathbuf[sizeof(pathbuf)-1]=0;
                continue; // follow redirect
            }

            // otherwise unknown form
            http_last_ret = -14;
            return -14;
        }

        // Not a redirect (or reached final). Find start of body
        char *body = strstr(raw, "\r\n\r\n");
        if (body) body += 4; else body = raw;

        // copy up to out_cap-1 bytes and terminate
        int body_len = (int)strlen(body);
        if (body_len > out_cap - 1) body_len = out_cap - 1;
        if (body_len > 0) memcpy(out, body, body_len);
        out[body_len] = '\0';

        http_last_ret = body_len;
        return body_len;
    }

    http_last_ret = -15; // too many redirects
    return -15;
}

