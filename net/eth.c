#include "net.h"
#include "stdio.h"
#include <string.h>

/* Minimal Ethernet helpers used by usb_rndis/higher-level code */

void eth_send_frame(const void *data, int len){
    /* Forward to nic_tx (driver-specific) */
    extern void nic_tx(const void *data, int len);
    nic_tx(data, len);
}

void eth_handle_rx(const uint8_t *frame,int len){
    /* hand to net layer */
    net_rx(frame,len);
}
