#ifndef DNS_H
#define DNS_H

#include <stdint.h>

// Resolve an A record for 'name'. Returns 1 on success and writes host-order IPv4 to out_ip.
int dns_resolve(const char *name, uint32_t *out_ip);

// Asynchronous API: start a query (returns 1 if query sent or already cached)
int dns_query_async(const char *name);
// Check cache: returns 1 and fills out_ip if present
int dns_get_cached(const char *name, uint32_t *out_ip);

// Called by net when a UDP packet from port 53 is received
void dns_on_response(uint32_t src_ip, const uint8_t *data, int len);

#endif
