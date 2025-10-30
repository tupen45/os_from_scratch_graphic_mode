#ifndef TCP_H
#define TCP_H
#include <stdint.h>

typedef enum {
    TCP_CLOSED=0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    uint8_t  in_use;
    tcp_state_t state;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;

    uint32_t snd_iss;   // initial send seq
    uint32_t snd_nxt;   // next seq to send
    uint32_t rcv_nxt;   // next seq expected

    // very small recv buffer
    uint8_t *rx_buf;
    int rx_cap;
    int rx_len;

    // app-close requested?
    uint8_t close_after_send;
} tcp_socket_t;

void tcp_init(void);
void tcp_on_rx(const uint8_t *ip_pkt, int ip_len, uint32_t src_ip, uint32_t dst_ip);
int  tcp_connect(tcp_socket_t *s, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port);
int  tcp_send(tcp_socket_t *s, const void *data, int len);
int  tcp_recv(tcp_socket_t *s, void *out, int maxlen); // non-blocking: returns bytes copied (0 = nothing yet)
int  tcp_close(tcp_socket_t *s);
extern tcp_socket_t g_sock;

#endif
