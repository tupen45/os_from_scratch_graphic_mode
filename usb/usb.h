#pragma once

#include <stdint.h>

/* High-level USB types for the kernel (minimal) */

#define USB_DIR_IN  0x80
#define USB_DIR_OUT 0x00

#define USB_TYPE_STANDARD (0x00 << 5)
#define USB_TYPE_CLASS    (0x01 << 5)
#define USB_TYPE_VENDOR   (0x02 << 5)

#define USB_RECIP_DEVICE 0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02

struct usb_device {
    uint8_t bus; /* TODO: support multiple host controllers */
    uint8_t addr;
    uint8_t speed;
};

/* Minimal protos */
int usb_init(void);
int usb_probe_controllers(void);
int usb_enumerate_device(struct usb_device *dev);

int xhci_probe(void);

/* xHCI helpers exposed for basic probing */
int xhci_num_ports(void);
uint32_t xhci_read_portsc(int port);
void xhci_dump_ports(void);

/* Command ring scaffolding */
int xhci_init_command_ring(void);
void *xhci_command_ring_virt(void);
uintptr_t xhci_command_ring_phys(void);

/* Control transfer helper (synchronous, blocking) */
int usb_control_transfer(struct usb_device *dev, uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength, int timeout_ms);

/* Bulk transfer helpers (async or blocking) - stubs for now */
int usb_bulk_transfer_in(struct usb_device *dev, uint8_t endpoint, void *buf, int len, int timeout_ms);
int usb_bulk_transfer_out(struct usb_device *dev, uint8_t endpoint, const void *buf, int len, int timeout_ms);

/* RNDIS/CDC detection helper */
int usb_is_rndis_or_ecm(struct usb_device *dev);

/* nic backend registration */
void usb_nic_register(void (*rx_fn)(const uint8_t *frame, int len));
void usb_nic_send(const void *data, int len);
