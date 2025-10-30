#pragma once
#include "usb.h"

/* Minimal API: attempt to attach to a device as RNDIS; non-fatal if fails */
int usb_rndis_try_attach(struct usb_device *dev);

/* Send a raw Ethernet frame from kernel through RNDIS if attached */
void usb_rndis_send_frame(const void *data, int len);
