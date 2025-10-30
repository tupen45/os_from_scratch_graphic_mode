#pragma once
#include <stdint.h>

/* --- byte order helpers (freestanding, works on little-endian x86) --- */
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0xFF000000U) >> 24);
}
static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

/* --- IPv4 header checksum (no pseudo-header) --- */
static inline uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *w = (const uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *w++;
        len -= 2;
    }
    if (len == 1) sum += *(const uint8_t*)w;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}
