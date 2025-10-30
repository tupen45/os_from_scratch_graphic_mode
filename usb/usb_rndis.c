#include "usb/usb_rndis.h"
#include "usb/usb_desc.h"
#include "usb.h"
#include "net.h"
#include "stdio.h"
#include <string.h>

/* Very small RNDIS stub: it attempts to perform GET_DESCRIPTOR calls to
 * discover if device is likely RNDIS (vendor: microsoft RNDIS uses class 0xE0 etc.)
 * For this minimal implementation we simply mark success on heuristic match.
 */

static int rndis_attached = 0;

int usb_rndis_try_attach(struct usb_device *dev){
    (void)dev;
    /* Heuristic: call usb_is_rndis_or_ecm if available, else attempt a basic check */
    extern int usb_is_rndis_or_ecm(struct usb_device *dev);
    int ok = 0;
    ok = usb_is_rndis_or_ecm(dev);
    if(ok){
        rndis_attached = 1;
        serial_puts("usb_rndis: heuristic matched, attached\n");
        return 0;
    }
    serial_puts("usb_rndis: heuristic not matched\n");
    return -1;
}

void usb_rndis_send_frame(const void *data, int len){
    if(!rndis_attached) return;
    /* Map to net layer: use nic_tx which may be provided by usb_nic implementation */
    nic_tx(data, len);
}
