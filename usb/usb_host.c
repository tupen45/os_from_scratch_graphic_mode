#include "usb.h"
#include "pci.h"
#include "stdio.h"
#include "io.h"
#include <stdarg.h>
extern int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
static void uart_init_once(void){ static int inited=0; if(inited) return; outb(0x3f8 + 1, 0x00); outb(0x3f8 + 3, 0x80); outb(0x3f8 + 0, 0x01); outb(0x3f8 + 1, 0x00); outb(0x3f8 + 3, 0x03); outb(0x3f8 + 2, 0xC7); outb(0x3f8 + 4, 0x0B); inited=1; }
static void serial_putc(char c){ uart_init_once(); for(int i=0;i<10000;i++){ uint8_t lsr = inb(0x3f8 + 5); if(lsr & 0x20) break; } outb(0x3f8 + 0, (uint8_t)c); }
static void serial_puts(const char *s){ while(s && *s) serial_putc(*s++); }
static int serial_printf(const char *fmt, ...){ char buf[256]; va_list ap; va_start(ap,fmt); int n=vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap); if(n>0) serial_puts(buf); return n; }
#undef printf
#define printf(...) serial_printf(__VA_ARGS__)

/* Minimal USB host skeleton: no real controller implementation yet.
 * Provides stubs so higher layers can be developed incrementally.
 */

int usb_init(void){
    printf("usb: initialized (stub)\n");
    // probe for xHCI controller if present (non-fatal)
    extern int xhci_probe(void);
    xhci_probe();
    return 0;
}
int usb_probe_controllers(void){
    printf("usb: probe controllers\n");
    extern int xhci_probe(void);
    int r = xhci_probe();
    if(r==0){
        /* dump port status for debugging */
        extern void xhci_dump_ports(void);
        xhci_dump_ports();
        /* perform a simple enumerate probe so boot logs show device detection
         * (placeholder until proper control-transfer/address assignment is implemented)
         */
        struct usb_device d = {0};
        if(usb_enumerate_device(&d)==0){
            printf("usb: simple enumerate succeeded addr=%u\n", d.addr);
        } else {
            printf("usb: simple enumerate found no device\n");
        }
    }
    return r;
}
int usb_enumerate_device(struct usb_device *dev){
    printf("usb: enumerate device addr=%u (simple probe)\n", dev?dev->addr:0);
    /* Simple heuristic: if xHCI reports N ports and at least one has a connection
     * (CONNECT bit in PORTSC), claim device at address 1 and return success.
     * This is a placeholder until control transfer and address assignment are implemented.
     */
    extern int xhci_num_ports(void);
    extern uint32_t xhci_read_portsc(int port);
    int n = xhci_num_ports();
    for(int i=1;i<=n;i++){
        uint32_t p = xhci_read_portsc(i);
        /* PORTSC CONNECT status bit is typically bit 0 (CCS) and current connect change in bit 1
         * We'll treat any non-zero PORTSC as an indicator for now.
         */
        if(p){
            if(dev) dev->addr = 1;
            printf("usb: found device on port %d PORTSC=0x%08x -> assigning addr=1\n", i, p);
            return 0;
        }
    }
    printf("usb: no device found during simple enumerate\n");
    return -1;
}
int usb_control_transfer(struct usb_device *dev, uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength, int timeout_ms){
    (void)timeout_ms;
    printf("usb: control_transfer dev=%p bm=0x%02x bReq=0x%02x wVal=0x%04x wIdx=0x%04x wLen=%u\n",
           (void*)dev, bmRequestType, bRequest, wValue, wIndex, (unsigned)wLength);
    extern int xhci_prepare_control_transfer(const void *setup8, uintptr_t setup_phys,
                                             void *data, uintptr_t data_phys, int data_len, int direction_in);
    extern int xhci_submit_command_ring(int trb_count, void *user_buf, void *data_v, int data_len, int direction_in);
    extern void xhci_dump_cmd_ring(int n);
    extern void *kmalloc(size_t sz);

    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xff);
    setup[3] = (uint8_t)((wValue>>8) & 0xff);
    setup[4] = (uint8_t)(wIndex & 0xff);
    setup[5] = (uint8_t)((wIndex>>8) & 0xff);
    setup[6] = (uint8_t)(wLength & 0xff);
    setup[7] = (uint8_t)((wLength>>8) & 0xff);

    /* In a real implementation we must provide a physical address for the setup packet
     * and data buffers. For now we use simple kmalloc and assume identity mapping
     * of virtual->physical (VIRT_TO_PHYS or flat mapping in this kernel).
     */
    void *setup_v = kmalloc(8);
    if(!setup_v) return -1;
    for(int i=0;i<8;i++) ((uint8_t*)setup_v)[i] = setup[i];
    uintptr_t setup_phys = (uintptr_t)setup_v;

    uintptr_t data_phys = 0;
    void *data_v = NULL;
    if(wLength>0 && data){
        data_v = kmalloc(wLength);
        if(!data_v) return -1;
        if((bmRequestType & 0x80) == 0x80){ /* IN */
            /* For IN, we just prepare buffer for controller to write into */
        } else {
            /* For OUT, copy data to buffer */
            for(int i=0;i<wLength;i++) ((uint8_t*)data_v)[i] = ((uint8_t*)data)[i];
        }
        data_phys = (uintptr_t)data_v;
    }

    int direction_in = ((bmRequestType & 0x80) == 0x80);
    int trbs = xhci_prepare_control_transfer(setup_v, setup_phys, data_v, data_phys, wLength, direction_in);
    if(trbs<0) return -1;
    xhci_dump_cmd_ring(trbs);
    /* Submit and wait for completion; pass user buffer so IN data can be copied back. */
    int r = xhci_submit_command_ring(trbs, data, data_v, (int)wLength, direction_in);
    /* free temporary buffers after submit returns (driver copies IN data back on completion) */
    extern void free(void*);
    if(setup_v) free(setup_v);
    if(data_v) free(data_v);
    if(r<0) return -1;
    /* In dry-run mode xhci_submit_command_ring returns 0; indicate success for control transfers */
    return (int)wLength;
}
int usb_bulk_transfer_in(struct usb_device *dev, uint8_t endpoint, void *buf, int len, int timeout_ms){
    (void)dev;(void)endpoint;(void)buf;(void)len;(void)timeout_ms;
    return -1;
}
int usb_bulk_transfer_out(struct usb_device *dev, uint8_t endpoint, const void *buf, int len, int timeout_ms){
    (void)dev;(void)endpoint;(void)buf;(void)len;(void)timeout_ms;
    return -1;
}
int usb_is_rndis_or_ecm(struct usb_device *dev){
    (void)dev; return 0; /* unknown */
}

static void (*nic_rx_cb)(const uint8_t*, int) = NULL;
void usb_nic_register(void (*rx_fn)(const uint8_t *frame, int len)){
    nic_rx_cb = rx_fn;
}
void usb_nic_send(const void *data, int len){
    // Called by net layer to send through USB NIC when real implementation exists.
    (void)data; (void)len;
    printf("usb: usb_nic_send (stub) len=%d\n", len);
}

/* Helper to inject a received frame into kernel net stack (for testing) */
void usb_nic_inject_test_frame(const uint8_t *frame,int len){
    if(nic_rx_cb) nic_rx_cb(frame,len);
}
