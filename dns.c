#include "dns.h"
#include "net.h"
#include "string.h"
#include "endian.h"
#include "util_net.h"
#include <stdint.h>
#include "rtl8139.h"

#define DNS_CACHE_ENTRIES 8
struct dns_cache_entry { char name[128]; uint32_t ip; };
static struct dns_cache_entry dns_cache[DNS_CACHE_ENTRIES];

// track outstanding queries by qid
#define DNS_OUTSTANDING 8
struct qtrack { uint16_t qid; char name[128]; };
static struct qtrack qtracks[DNS_OUTSTANDING];

// Helper: build DNS name (labels)
static int pack_name(const char *name, uint8_t *out, int out_cap){
    int w = 0;
    const char *p = name;
    while (*p && w < out_cap){
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len <= 0 || len > 63) return -1;
        out[w++] = (uint8_t)len;
        if (w + len >= out_cap) return -1;
        memcpy(out + w, p, len); w += len;
        if (!dot) break; p = dot + 1;
    }
    if (w >= out_cap) return -1;
    out[w++] = 0; // null terminator
    return w;
}

static uint16_t qid_counter = 0x4000;

// store to cache (overwrite first empty or simple rotate)
static void dns_cache_store(const char *name, uint32_t ip){
    if (!name) return;
    for(int i=0;i<DNS_CACHE_ENTRIES;i++) if(dns_cache[i].ip!=0 && strcmp(dns_cache[i].name,name)==0) return;
    for(int i=0;i<DNS_CACHE_ENTRIES;i++) if(dns_cache[i].ip==0){ strncpy(dns_cache[i].name,name,sizeof(dns_cache[i].name)-1); dns_cache[i].name[sizeof(dns_cache[i].name)-1]=0; dns_cache[i].ip=ip; return; }
    // evict entry 0
    strncpy(dns_cache[0].name,name,sizeof(dns_cache[0].name)-1); dns_cache[0].name[sizeof(dns_cache[0].name)-1]=0; dns_cache[0].ip=ip;
}

int dns_get_cached(const char *name, uint32_t *out_ip){
    if (!name) return 0;
    for(int i=0;i<DNS_CACHE_ENTRIES;i++) if(dns_cache[i].ip!=0 && strcmp(dns_cache[i].name,name)==0){ *out_ip = dns_cache[i].ip; return 1; }
    return 0;
}

// store outstanding mapping
static int qtrack_add(uint16_t qid, const char *name){
    for(int i=0;i<DNS_OUTSTANDING;i++) if(qtracks[i].qid==0){ qtracks[i].qid=qid; strncpy(qtracks[i].name,name,sizeof(qtracks[i].name)-1); qtracks[i].name[sizeof(qtracks[i].name)-1]=0; return 1; }
    // evict 0 if full
    qtracks[0].qid = qid; strncpy(qtracks[0].name,name,sizeof(qtracks[0].name)-1); qtracks[0].name[sizeof(qtracks[0].name)-1]=0; return 1;
}
static const char *qtrack_getname(uint16_t qid){
    for(int i=0;i<DNS_OUTSTANDING;i++) if(qtracks[i].qid==qid) return qtracks[i].name;
    return NULL;
}
static void qtrack_clear(uint16_t qid){
    for(int i=0;i<DNS_OUTSTANDING;i++) if(qtracks[i].qid==qid){ qtracks[i].qid=0; qtracks[i].name[0]=0; }
}

// Called by net when a UDP packet from port 53 is received
void dns_on_response(uint32_t src_ip,const uint8_t *data,int len){
    if (len < 12) return;
    uint16_t id = (data[0]<<8)|data[1];
    uint16_t qdcount = (data[4]<<8)|data[5];
    uint16_t ancount = (data[6]<<8)|data[7];

    const char *queried_name = qtrack_getname(id);

    int off = 12;
    // skip question(s)
    for(int qi=0; qi<qdcount; ++qi){
        // skip qname
        while (off < len && data[off] != 0){ int lab = data[off]; off += 1 + lab; }
        off++; if (off+4 > len) return; off += 4; // qtype qclass
    }

    // parse answers
    for(int ai=0; ai<ancount; ++ai){
        if (off+12 > len) return;
        // skip name field (if pointer 2 bytes, else labels)
        if ((data[off] & 0xC0) == 0xC0) off += 2; else { while (off < len && data[off]){ off += 1 + data[off]; } off++; }
        if (off+10 > len) return;
        uint16_t atype = (data[off]<<8)|data[off+1];
        uint16_t aclass = (data[off+2]<<8)|data[off+3];
        uint16_t rdlen = (data[off+8]<<8)|data[off+9];
        off += 10;
        if (off + rdlen > len) return;
        if (atype == 1 && aclass == 1 && rdlen == 4 && queried_name){
            uint32_t a = (data[off]<<24)|(data[off+1]<<16)|(data[off+2]<<8)|(data[off+3]);
            dns_cache_store(queried_name, a);
        }
        off += rdlen;
    }

    // clear outstanding mapping for this qid
    qtrack_clear(id);
}

int dns_query_async(const char *name){
    if (!name) return 0;
    // if cached already, nothing to do
    uint32_t tmp; if (dns_get_cached(name, &tmp)) return 1;

    // build query packet
    uint8_t buf[512]; int off = 0;
    uint16_t id = ++qid_counter;
    buf[off++] = id >> 8; buf[off++] = id & 0xFF;
    buf[off++] = 0x01; buf[off++] = 0x00; // flags RD
    buf[off++] = 0x00; buf[off++] = 0x01; // qdcount
    buf[off++] = 0x00; buf[off++] = 0x00; // ancount
    buf[off++] = 0x00; buf[off++] = 0x00; // nscount
    buf[off++] = 0x00; buf[off++] = 0x00; // arcount

    int n = pack_name(name, buf + off, sizeof(buf) - off);
    if (n < 0) return 0; off += n;
    buf[off++] = 0x00; buf[off++] = 0x01; // type A
    buf[off++] = 0x00; buf[off++] = 0x01; // class IN

    // record qid -> name mapping
    qtrack_add(id, name);

    uint32_t dns_ip = (10<<24)|(0<<16)|(2<<8)|3; // QEMU usernet local resolver
    net_send_udp_ipv4(dns_ip, 53, 50000, buf, off);
    return 1;
}

int dns_resolve(const char *name, uint32_t *out_ip){
    if (!name || !out_ip) return 0;
    if (dns_get_cached(name, out_ip)) return 1;
    dns_query_async(name);
    // wait up to some polls
    for (int i=0;i<20000;i++){
        rtl8139_poll();
        if (dns_get_cached(name, out_ip)) return 1;
    }
    return 0;
}

// hook into UDP path by providing a weak symbol override? We assume net.c calls udp_on_datagram
// Provide a public function to be called from udp_on_datagram in net.c or elsewhere.
