#include "net.h"
#include "stdio.h"

/* Minimal DHCP placeholder: in a proper implementation this would send DHCPDISCOVER
 * and parse DHCPOFFER from the phone. For tethering integration we'll skip actual
 * DHCP and rely on static IPs or existing QEMU NAT. Placeholder provided for later.
 */

int dhcp_request_ip(void){
    serial_puts("dhcp: DHCP request placeholder - not implemented\n");
    return -1;
}
