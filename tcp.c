#include "tcp.h"
#include "net.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>
#include "endian.h"
#include "stdio.h"


#pragma pack(push,1)
typedef struct {
    uint16_t src, dst;
    uint32_t seq, ack;
    uint8_t  off_res;  // data offset (upper nibble) + reserved
    uint8_t  flags;    // CWR ECE URG ACK PSH RST SYN FIN
    uint16_t win;
    uint16_t chksum;
    uint16_t urgp;
} tcp_hdr_t;
#pragma pack(pop)

tcp_socket_t g_sock; // single socket

// ===== utils =====
static uint16_t csum16(const void* data, int len) {
    const uint16_t *w = (const uint16_t*)data;
    uint32_t sum=0;
    while (len > 1) { sum += *w++; len -= 2; }
    if (len) sum += *(const uint8_t*)w;
    while (sum>>16) sum = (sum&0xFFFF) + (sum>>16);
    return (uint16_t)(~sum);
}

#pragma pack(push,1)
typedef struct {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} pseudo_t;
#pragma pack(pop)

static void tcp_send_segment(tcp_socket_t *s, uint8_t flags, const void *payload, int plen) {
    uint8_t buf[60 + 1460]; // hdr + payload
    uint8_t *p = buf;

    // ---- IP header (filled by net_send_ip later) ----
    // We let net_send_ip build IP; here we only pass TCP payload.

    // ---- TCP header ----
    tcp_hdr_t *th = (tcp_hdr_t*)p;
    th->src = htons(s->local_port);
    th->dst = htons(s->remote_port);
    th->seq = htonl(s->snd_nxt);
    th->ack = htonl(s->rcv_nxt);
    th->off_res = (5<<4);
    th->flags = flags;
    th->win   = htons(4096);
    th->urgp  = 0;
    th->chksum= 0;

    p += sizeof(*th);
    if (plen>0) { memcpy(p, payload, plen); p += plen; }

    // checksum over pseudo + TCP hdr + data
    pseudo_t ph;
    ph.src = htonl(s->local_ip);
    ph.dst = htonl(s->remote_ip);
    ph.zero = 0;
    ph.proto = 6;
    ph.tcp_len = htons((uint16_t)(sizeof(*th)+plen));

    uint8_t csumblk[sizeof(ph) + sizeof(*th) + 1460];
    memcpy(csumblk, &ph, sizeof(ph));
    memcpy(csumblk+sizeof(ph), th, sizeof(*th)+plen);
    th->chksum = csum16(csumblk, (int)(sizeof(ph)+sizeof(*th)+plen));

    // send as IP proto 6
    net_send_ip(s->remote_ip, 6 /*TCP*/, (uint8_t*)th, (int)(sizeof(*th)+plen));
}

static uint16_t pick_ephemeral(void) { static uint16_t p=40000; return p++; }
static uint32_t iss(void){ static uint32_t x=0x12340000; x+=0x1000; return x; }

void tcp_init(void){ memset(&g_sock,0,sizeof(g_sock)); }

int tcp_connect(tcp_socket_t *s, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port) {
    if (g_sock.in_use) return -1;
    memset(s,0,sizeof(*s));
    *s = (tcp_socket_t){0};
    s->in_use = 1;
    s->state = TCP_SYN_SENT;
    s->remote_ip = dst_ip;
    s->remote_port = dst_port;
    s->local_ip = net_get_ip();
    s->local_port = src_port ? src_port : pick_ephemeral();
    s->snd_iss = iss();
    s->snd_nxt = s->snd_iss;
    s->rcv_nxt = 0;
    // tiny rx buf
    static uint8_t rb[8192];
    s->rx_buf = rb; s->rx_cap = sizeof(rb); s->rx_len=0;

    // ARP next hop (gateway if off-subnet) happens inside net_send_ip
    tcp_send_segment(s, 0x02 /*SYN*/, NULL, 0);
    s->snd_nxt += 1;
    g_sock = *s;
    return 0;
}

static void rx_data_into_buf(tcp_socket_t *s, const uint8_t *pl, int len) {
    int can = s->rx_cap - s->rx_len;
    if (len > can) len = can;
    if (len>0) { memcpy(s->rx_buf + s->rx_len, pl, len); s->rx_len += len; }
}

void tcp_on_rx(const uint8_t *ip_pkt, int ip_len, uint32_t src_ip, uint32_t dst_ip) {
    (void)dst_ip;
    if (!g_sock.in_use) return;
    if (src_ip != g_sock.remote_ip) return;
    if (ip_len < (int)sizeof(tcp_hdr_t)) return;

    const tcp_hdr_t *th = (const tcp_hdr_t*)ip_pkt;
    uint16_t dport = ntohs(th->dst), sport = ntohs(th->src);
    if (dport != g_sock.local_port || sport != g_sock.remote_port) return;

    int hdrlen = (th->off_res>>4)*4;
    if (ip_len < hdrlen) return;
    const uint8_t *pl = ip_pkt + hdrlen;
    int plen = ip_len - hdrlen;

    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);

    // RST?
    if (th->flags & 0x04) { g_sock.state = TCP_CLOSED; g_sock.in_use=0; return; }

    switch (g_sock.state) {
        case TCP_SYN_SENT:
            if ((th->flags & 0x12) == 0x12) { // SYN+ACK
                g_sock.rcv_nxt = seq + 1;
                // ACK our SYN
                tcp_send_segment(&g_sock, 0x10 /*ACK*/, NULL, 0);
                printf("tcp: received SYN+ACK from %u, acking and entering ESTABLISHED\n", src_ip);
                g_sock.state = TCP_ESTABLISHED;
            }
            break;
        case TCP_ESTABLISHED:
            if (plen>0 && seq == g_sock.rcv_nxt) {
                rx_data_into_buf(&g_sock, pl, plen);
                g_sock.rcv_nxt += plen;
                tcp_send_segment(&g_sock, 0x10 /*ACK*/, NULL, 0);
                printf("tcp: got %d bytes, rcv_nxt=%u\n", plen, g_sock.rcv_nxt);
            }
            if (th->flags & 0x01 /*FIN*/) {
                g_sock.rcv_nxt += 1;
                // ACK FIN
                tcp_send_segment(&g_sock, 0x10 /*ACK*/, NULL, 0);
                // we also FIN
                tcp_send_segment(&g_sock, 0x11 /*FIN+ACK*/, NULL, 0);
                g_sock.snd_nxt += 1;
                g_sock.state = TCP_FIN_WAIT1;
            }
            break;
        case TCP_FIN_WAIT1:
            if (th->flags & 0x10 /*ACK*/) {
                g_sock.state = TCP_FIN_WAIT2;
            }
            break;
        case TCP_FIN_WAIT2:
            if (th->flags & 0x01 /*FIN*/) {
                g_sock.rcv_nxt += 1;
                tcp_send_segment(&g_sock, 0x10 /*ACK*/, NULL, 0);
                g_sock.state = TCP_TIME_WAIT;
            }
            break;
        default: break;
    }
}

int tcp_send(tcp_socket_t *s, const void *data, int len) {
    if (s->state != TCP_ESTABLISHED) return -1;
    if (len <= 0) return 0;
    tcp_send_segment(s, 0x18 /*PSH+ACK*/, data, len);
    s->snd_nxt += (uint32_t)len;
    return len;
}

int tcp_recv(tcp_socket_t *s, void *out, int maxlen) {
    if (s->rx_len <= 0) return 0;
    if (maxlen > s->rx_len) maxlen = s->rx_len;
    memcpy(out, s->rx_buf, maxlen);
    // shift
    memmove(s->rx_buf, s->rx_buf + maxlen, s->rx_len - maxlen);
    s->rx_len -= maxlen;
    return maxlen;
}

int tcp_close(tcp_socket_t *s) {
    if (!s->in_use) return 0;
    if (s->state == TCP_ESTABLISHED) {
        tcp_send_segment(s, 0x11 /*FIN+ACK*/, NULL, 0);
        s->snd_nxt += 1;
        s->state = TCP_FIN_WAIT1;
        return 0;
    }
    s->in_use=0; s->state=TCP_CLOSED;
    return 0;
}

void tcp_poll(void) { /* no timers yet */ }
