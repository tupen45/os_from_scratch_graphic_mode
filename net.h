#pragma once
#include <stdint.h>


#define ETH_ADDR_LEN 6


struct net_if {
uint8_t mac[ETH_ADDR_LEN];
uint32_t ip; // host order
uint32_t gw_ip; // host order
uint32_t netmask; // host order
};


extern struct net_if g_netif;


void net_init(uint8_t mac[6]);
void net_set_ipv4(uint32_t ip, uint32_t netmask, uint32_t gw);
uint32_t net_get_ip(void);

// low level
void nic_tx(const void *data, int len);


// high-level helpers
void net_send_udp_ipv4(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
const void *payload, int len);

void net_send_ip(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, int payload_len);

void net_send_udp_ipv4(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                       const void *payload, int len);


// RX entry point from NIC
void net_rx(const uint8_t *frame, int len);