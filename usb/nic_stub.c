#include "rtl8139.h" // for nic_tx prototype
#include "usb/usb.h"
#include "net.h"
#include "stdio.h"

// This module provides a toggleable NIC backend that forwards packets to the usb_nic_send stub.

static int use_usb_nic = 0;

void usb_nic_enable(void){
    use_usb_nic = 1;
}
void usb_nic_disable(void){
    use_usb_nic = 0;
}

// Provide a weak nic_tx so real drivers can override this symbol.
__attribute__((weak))
void nic_tx(const void *data, int len){
    if (use_usb_nic){
        usb_nic_send(data, len);
        return;
    }
    printf("nic_stub: usb not enabled, dropping packet len=%d\n", len);
}

// provide an entry to receive frames from USB stub
static void usb_rx_hook(const uint8_t *frame,int len){
    printf("usb_nic: received frame len=%d -> net_rx\n", len);
    net_rx(frame,len);
}

// init function to register hook
int usb_nic_init(void){
    usb_nic_register(usb_rx_hook);
    // by default leave usb_nic disabled until user enables it
    return 0;
}
