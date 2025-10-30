#include "usb/usb_desc.h"
#include "usb.h"
#include "stdio.h"
#include "usb/usb_rndis.h"

/* Minimal USB core to detect network-capable interfaces and hookup RNDIS
 * Uses existing xhci control-transfer helpers via usb_host.c stubs.
 */

int usb_core_enumerate_and_attach_network(void){
    /* Probe controllers and simple enumerate (usb_host should provide helpers) */
    extern int usb_probe_controllers(void);
    extern int usb_enumerate_device(struct usb_device *dev);
    if(usb_probe_controllers()!=0) return -1;

    struct usb_device d = {0};
    if(usb_enumerate_device(&d)!=0) return -1;

    /* Attempt to fetch config descriptor and detect RNDIS; usb_enumerate_device
     * already assigns addr=1 in simple probe. We call RNDIS attach routine which
     * will be conservative and non-blocking. */
    if(usb_rndis_try_attach(&d)==0){
        serial_puts("usb_core: rndis attached\n");
        return 0;
    }
    serial_puts("usb_core: no rndis device attached\n");
    return -1;
}
