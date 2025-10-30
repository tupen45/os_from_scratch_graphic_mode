// Minimal TLS stub that will be implemented with mbedTLS later.
// For now, provide a placeholder implementation that returns -1 to indicate not available.

#include "tls.h"

int tls_http_get_by_ip(uint32_t ip, const char *host_header, const char *path, char *out, int out_cap){
    (void)ip; (void)host_header; (void)path; (void)out; (void)out_cap;
    return -1; // not implemented yet
}
