#include "net.h"
#include "graphics.h"
#include "string.h"
#include <stdint.h>


// Very naive JSON printer (assumes payload is ASCII)
static void draw_payload_line(const char *s){ draw_string(20, 20, s, 0x000000); }


// Override the weak handler to display UDP data
void udp_on_datagram(uint32_t src_ip, uint16_t src_port,
                     const uint8_t *data, int len) {
    (void)src_ip; (void)src_port;

    // Simple clipping
    if (len > 1000) len = 1000;

    char buf[1001];
    memcpy(buf, data, len);
    buf[len] = 0;

    // Very naive: search for "title"
    char *p = buf;
    int y = 100; // start drawing lower
    while ((p = strstr(p, "\"title\""))) {
        p = strchr(p, ':');
        if (!p) break;
        p++;
        while (*p == '"' || *p == ' ' || *p == ':') p++;
        char title[64]; int i=0;
        while (*p && *p!='"' && i<63) title[i++] = *p++;
        title[i]=0;

        draw_string(50, y, title, 0x000000);
        y += 20;
        if (y > 350) break; // stop if too many
    }
}



// Helper: IPv4 dotted quad to u32
static uint32_t ip4(uint8_t a,uint8_t b,uint8_t c,uint8_t d){ return (a<<24)|(b<<16)|(c<<8)|d; }


void net_demo_send_json(void){
// Example: send to QEMU slirp host at 192.168.31.235 from src port 6001
const char *json = "{\"hello\":\"tbhcr\",\"from\":\"MyOS\"}";
net_send_udp_ipv4(ip4(192,168,31,235), 6000, 6001, json, (int)strlen(json));
}