#include "net.h"
#include "util_net.h"
#include "string.h"
#include "graphics.h"
#include <stddef.h>
#include <stdint.h>
#include "endian.h"
#include "dns.h"
#include "tcp.h"
#include "stdio.h"


/* --- constants --- */
#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800
#define IP_PROTO_UDP 17
#define IP_PROTO_TCP 6

/* --- headers --- */
#pragma pack(push,1)
struct eth_hdr { uint8_t dst[6]; uint8_t src[6]; uint16_t type; };
struct arp_ipv4 {
    uint16_t htype, ptype; uint8_t hlen, plen; uint16_t oper;
    uint8_t sha[6]; uint32_t spa; uint8_t tha[6]; uint32_t tpa;
};
struct ip_hdr {
    uint8_t ver_ihl, tos; uint16_t tot_len, id, frag;
    uint8_t ttl, proto; uint16_t hdr_chksum; uint32_t src, dst;
};
struct udp_hdr { uint16_t src,dst,len,chksum; };
struct tcp_hdr {
    uint16_t src, dst; uint32_t seq, ack;
    uint8_t offset_reserved; uint8_t flags; uint16_t win; uint16_t chksum; uint16_t urg_ptr;
};
#pragma pack(pop)

/* --- globals --- */
struct net_if g_netif;
static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
struct arp_cache_entry { uint32_t ip; uint8_t mac[6]; };
static struct arp_cache_entry arp_cache[8];
// pending packets waiting for ARP resolution
struct pending_pkt { uint32_t dst_ip; uint8_t proto; int payload_len; uint8_t payload[1500]; };
static struct pending_pkt pending[8];

/* NIC TX (rtl8139.c provides it) */
void nic_tx(const void *data, int len);

/* weak callbacks */
__attribute__((weak))
void udp_on_datagram(uint32_t src_ip,uint16_t src_port,const uint8_t *d,int l){
    (void)src_ip;(void)src_port;(void)d;(void)l;
}
__attribute__((weak))
void tcp_on_segment(uint32_t src_ip,uint16_t src_port,const struct tcp_hdr *th,
                    const uint8_t *pl,int len){
    (void)src_ip;(void)src_port;(void)th;(void)pl;(void)len;
}

/* --- ARP cache helpers --- */
static int arp_lookup(uint32_t ip, uint8_t mac_out[6]){
    for (int i=0;i<8;i++) if (arp_cache[i].ip==ip){
        for(int j=0;j<6;j++) mac_out[j]=arp_cache[i].mac[j];
        return 1;
    }
    return 0;
}
static void arp_store(uint32_t ip,const uint8_t mac[6]){
    for (int i=0;i<8;i++) if (arp_cache[i].ip==ip){
        for(int j=0;j<6;j++) arp_cache[i].mac[j]=mac[j];
        return;
    }
    for (int i=0;i<8;i++) if (arp_cache[i].ip==0){
        arp_cache[i].ip=ip; for(int j=0;j<6;j++) arp_cache[i].mac[j]=mac[j]; return;
    }
    arp_cache[0].ip=ip; for(int j=0;j<6;j++) arp_cache[0].mac[j]=mac[j];
}

// flush pending packets for an IP once we learn its MAC
static void flush_pending_for_ip(uint32_t ip){
    for (int i=0;i<8;i++){
        if (pending[i].dst_ip == ip){
            // attempt to send now that ARP entry exists
            printf("net: flushing pending pkt for %u\n", ip);
            net_send_ip(pending[i].dst_ip, pending[i].proto, pending[i].payload, pending[i].payload_len);
            // clear slot
            pending[i].dst_ip = 0;
            pending[i].payload_len = 0;
        }
    }
}

/* --- init --- */
void net_init(uint8_t mac[6]){
    for(int i=0;i<6;i++) g_netif.mac[i]=mac[i];
    g_netif.ip=0; g_netif.netmask=0; g_netif.gw_ip=0;
    for(int i=0;i<8;i++) arp_cache[i].ip=0;
}
void net_set_ipv4(uint32_t ip,uint32_t netmask,uint32_t gw){
    g_netif.ip=ip; g_netif.netmask=netmask; g_netif.gw_ip=gw;
}
uint32_t net_get_ip(void){ return g_netif.ip; }

/* --- ARP --- */
static void send_arp_request(uint32_t target_ip){
    uint8_t buf[42];
    struct eth_hdr *e=(struct eth_hdr*)buf;
    memcpy(e->dst,bcast,6); memcpy(e->src,g_netif.mac,6);
    e->type=htons(ETH_TYPE_ARP);

    struct arp_ipv4 *a=(struct arp_ipv4*)(buf+sizeof(*e));
    a->htype=htons(1); a->ptype=htons(0x0800);
    a->hlen=6; a->plen=4; a->oper=htons(1);
    memcpy(a->sha,g_netif.mac,6);
    a->spa=htonl(g_netif.ip);
    memset(a->tha,0,6);
    a->tpa=htonl(target_ip);
    nic_tx(buf,sizeof(buf));
}

/* --- RX demux --- */
void net_rx(const uint8_t *frame,int len){
    if (len < 14) return;
    const struct eth_hdr *e=(const struct eth_hdr*)frame;
    uint16_t type=ntohs(e->type);

    if (type == ETH_TYPE_ARP){
        if (len < 14 + (int)sizeof(struct arp_ipv4)) return;
        const struct arp_ipv4 *a=(const struct arp_ipv4*)(frame+14);
        if (ntohs(a->htype)==1 && ntohs(a->ptype)==0x0800 && a->hlen==6 && a->plen==4){
            uint32_t spa=ntohl(a->spa), tpa=ntohl(a->tpa);
            if (spa) {
                arp_store(spa, a->sha);
                // flush any queued packets for this IP
                flush_pending_for_ip(spa);
            }
            (void)tpa; // you could reply if tpa == g_netif.ip
        }
        return;
    }

    if (type == ETH_TYPE_IP){
        if (len < 14+20) return;
        const struct ip_hdr *ip=(const struct ip_hdr*)(frame+14);
        if ((ip->ver_ihl>>4) != 4) return;
        int ihl=(ip->ver_ihl & 0xF)*4; if (len < 14+ihl) return;
        int pl_len = (int)ntohs(ip->tot_len) - ihl;
        if (pl_len < 0) return;
        const uint8_t *pl = (const uint8_t*)ip + ihl;

        if (ip->proto == IP_PROTO_UDP){
            if (pl_len < (int)sizeof(struct udp_hdr)) return;
            const struct udp_hdr *u = (const struct udp_hdr*)pl;
            uint16_t src_port = ntohs(u->src);
            uint16_t dst_port = ntohs(u->dst);
            // If DNS response from server (src port 53), hand to dns module
            if (src_port == 53) {
                dns_on_response(ntohl(ip->src), pl+8, pl_len-8);
            }
            udp_on_datagram(ntohl(ip->src), ntohs(u->src), pl+8, pl_len-8);
        } else if (ip->proto == IP_PROTO_TCP){
            if (pl_len < (int)sizeof(struct tcp_hdr)) return;
            // pl points to the TCP header followed by payload; pass whole tcp segment
            const uint8_t *tcp_seg = pl;
            // hand the tcp header+payload buffer to tcp layer (tcp_on_rx expects header at start)
            tcp_on_rx(tcp_seg, pl_len, ntohl(ip->src), ntohl(ip->dst));
        }
    }
}

/* --- SEND helpers --- */

/* send an L4 payload as an IPv4 packet (proto set by caller) */
void net_send_ip(uint32_t dst_ip, uint8_t proto,
                 const uint8_t *payload, int payload_len)
{
    uint32_t next_hop = dst_ip;
    // if outside subnet, send via gateway
    if (((dst_ip ^ g_netif.ip) & g_netif.netmask) != 0) {
        next_hop = g_netif.gw_ip;
    }

    uint8_t buf[1514]; int off=0;

    /* Ethernet */
    struct eth_hdr *e=(struct eth_hdr*)buf;
    uint8_t dst_mac[6];
    if (!arp_lookup(next_hop, dst_mac)) {
        // enqueue while ARP resolves
        for (int i=0;i<8;i++){
            if (pending[i].dst_ip == 0){
                pending[i].dst_ip = dst_ip;   // still track original dst
                pending[i].proto = proto;
                if (payload_len > (int)sizeof(pending[i].payload))
                    payload_len = (int)sizeof(pending[i].payload);
                pending[i].payload_len = payload_len;
                memcpy(pending[i].payload, payload, (size_t)payload_len);
                break;
            }
        }
        send_arp_request(next_hop);  // ARP the gateway, not remote IP
        return;
    }
    memcpy(e->dst, dst_mac, 6); memcpy(e->src, g_netif.mac, 6);
    e->type = htons(ETH_TYPE_IP);
    off += sizeof(*e);

    /* IPv4 header */
    struct ip_hdr *ip=(struct ip_hdr*)(buf+off);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(*ip) + payload_len));
    ip->id = htons(1);
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = htonl(g_netif.ip);
    ip->dst = htonl(dst_ip);
    ip->hdr_chksum = 0;
    ip->hdr_chksum = ip_checksum(ip, sizeof(*ip));
    off += sizeof(*ip);

    memcpy(buf+off, payload, (size_t)payload_len);
    off += payload_len;

    nic_tx(buf, off);
}


/* convenience UDP builder (kept for your existing code) */
void net_send_udp_ipv4(uint32_t dst_ip,uint16_t dst_port,uint16_t src_port,
                       const void *payload,int len){
    uint8_t seg[8 + 1472];
    struct udp_hdr *u = (struct udp_hdr*)seg;
    u->src = htons(src_port);
    u->dst = htons(dst_port);
    u->len = htons((uint16_t)(8 + len));
    u->chksum = 0; /* skipping UDP checksum for now */
    memcpy(seg+8, payload, (size_t)len);
    net_send_ip(dst_ip, IP_PROTO_UDP, seg, 8+len);
}

/* optional: minimal SYN helper */
void net_send_tcp_syn(uint32_t dst_ip,uint16_t dst_port,uint16_t src_port,uint32_t seq){
    uint8_t seg[sizeof(struct tcp_hdr)];
    struct tcp_hdr *t=(struct tcp_hdr*)seg;
    t->src = htons(src_port);
    t->dst = htons(dst_port);
    t->seq = htonl(seq);
    t->ack = 0;
    t->offset_reserved = (5 << 4);
    t->flags = 0x02; /* SYN */
    t->win = htons(65535);
    t->chksum = 0;   /* your tcp.c should compute pseudo-header checksum if needed */
    t->urg_ptr = 0;
    net_send_ip(dst_ip, IP_PROTO_TCP, seg, sizeof(*t));
}
