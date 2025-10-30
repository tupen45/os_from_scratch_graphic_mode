#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

// Minimal Intel e1000(e) driver (polling-only, simple descriptor rings)
// Provides same API as rtl8139: rtl8139_init(), rtl8139_poll(), nic_tx(), rtl8139_is_ready().

#define INTEL_VENDOR 0x8086
#define E1000_CLASS 0x02

// Common registers (offsets in MMIO BAR)
#define E1000_CTRL   0x00000
#define E1000_STATUS 0x00008

// RX
#define E1000_RCTL   0x00100
#define E1000_RDBAL  0x02800
#define E1000_RDBAH  0x02804
#define E1000_RDLEN  0x02808
#define E1000_RDH    0x02810
#define E1000_RDT    0x02818

// TX
#define E1000_TCTL   0x00400
#define E1000_TDBAL  0x03800
#define E1000_TDBAH  0x03804
#define E1000_TDLEN  0x03808
#define E1000_TDH    0x03810
#define E1000_TDT    0x03818

// descriptor counts
#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 16
#define RX_BUF_SIZE  2048
#define TX_BUF_SIZE  2048

// simplified descriptor formats
struct e1000_rx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};
struct e1000_tx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};

static volatile uint8_t *mmio = NULL;
static int driver_ready = 0;

static struct e1000_rx_desc *rx_ring = NULL;
static uint8_t *rx_bufs[RX_DESC_COUNT];
static uint32_t rx_tail = 0;

static struct e1000_tx_desc *tx_ring = NULL;
static uint8_t *tx_bufs[TX_DESC_COUNT];
static uint32_t tx_tail = 0;
static uint32_t tx_head = 0;

// kmalloc provided by kernel
extern void *kmalloc(size_t sz);

// helper to read/write mmio registers
static inline uint32_t e1000_readl(uint32_t off){ return *((volatile uint32_t*)(mmio + off)); }
static inline void e1000_writel(uint32_t off, uint32_t v){ *((volatile uint32_t*)(mmio + off)) = v; }

// simple pci scan: find first Intel Ethernet device (class 0x02)
static int pci_find_intel_net(uint8_t *out_bus,uint8_t *out_slot,uint8_t *out_func){
    for(uint8_t bus=0; bus<256; ++bus){
        for(uint8_t slot=0; slot<32; ++slot){
            for(uint8_t func=0; func<8; ++func){
                uint16_t ven = pci_config_read16(bus,slot,func,0x00);
                if(ven==0xFFFF) continue;
                uint8_t class = pci_config_read8(bus,slot,func,0x0B);
                if(ven==INTEL_VENDOR && class==E1000_CLASS){
                    if(out_bus) *out_bus=bus; if(out_slot) *out_slot=slot; if(out_func) *out_func=func;
                    return 1;
                }
                uint8_t ht = pci_config_read8(bus,slot,0,0x0E);
                if((ht & 0x80)==0 && func==0) break;
            }
        }
    }
    return 0;
}

int rtl8139_init(void){
    uint8_t bus,slot,func;
    if(!pci_find_intel_net(&bus,&slot,&func)) return -1;

    // enable mem + bus master
    uint32_t cmd = pci_config_read32(bus,slot,func,0x04);
    cmd |= 0x0006; // Memory space + Bus Master
    pci_config_write32(bus,slot,func,0x04,cmd);

    // read BAR0 (memory)
    uint32_t bar0 = pci_config_read32(bus,slot,func,0x10);
    uint32_t base = bar0 & ~0xF;
    mmio = (volatile uint8_t*)(uintptr_t)base;
    if(!mmio) return -1;

    // allocate RX ring
    rx_ring = (struct e1000_rx_desc*)kmalloc(sizeof(struct e1000_rx_desc)*RX_DESC_COUNT);
    if(!rx_ring) return -1;
    for(int i=0;i<RX_DESC_COUNT;i++){
        rx_bufs[i] = (uint8_t*)kmalloc(RX_BUF_SIZE);
        rx_ring[i].buffer_addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    rx_tail = RX_DESC_COUNT - 1;
    e1000_writel(E1000_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    e1000_writel(E1000_RDBAH, 0);
    e1000_writel(E1000_RDLEN, RX_DESC_COUNT * sizeof(struct e1000_rx_desc));
    e1000_writel(E1000_RDH, 0);
    e1000_writel(E1000_RDT, rx_tail);

    // allocate TX ring
    tx_ring = (struct e1000_tx_desc*)kmalloc(sizeof(struct e1000_tx_desc)*TX_DESC_COUNT);
    if(!tx_ring) return -1;
    for(int i=0;i<TX_DESC_COUNT;i++){
        tx_bufs[i] = (uint8_t*)kmalloc(TX_BUF_SIZE);
        tx_ring[i].buffer_addr = (uint64_t)(uintptr_t)tx_bufs[i];
        tx_ring[i].status = 0;
    }
    tx_head = tx_tail = 0;
    e1000_writel(E1000_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    e1000_writel(E1000_TDBAH, 0);
    e1000_writel(E1000_TDLEN, TX_DESC_COUNT * sizeof(struct e1000_tx_desc));
    e1000_writel(E1000_TDH, 0);
    e1000_writel(E1000_TDT, 0);

    // bring up RX/TX: set RCTL and TCTL minimal bits
    // RCTL: EN (bit 1), strip CRC (bit 26)
    e1000_writel(E1000_RCTL, (1<<1) | (1<<26));
    // TCTL: enable (bit 1), pad short packets (bit 3)
    e1000_writel(E1000_TCTL, (1<<1) | (1<<3));

    // read MAC from registers (RAL/RAH at 0x5400/0x5404 etc) - try common offsets
    uint32_t ral = e1000_readl(0x5400);
    uint32_t rah = e1000_readl(0x5404);
    uint8_t mac[6];
    mac[0] = ral & 0xFF; mac[1] = (ral>>8)&0xFF; mac[2] = (ral>>16)&0xFF; mac[3] = (ral>>24)&0xFF;
    mac[4] = rah & 0xFF; mac[5] = (rah>>8)&0xFF;

    net_init(mac);
    driver_ready = 1;
    return 0;
}

int rtl8139_is_ready(void){ return driver_ready; }

void nic_tx(const void *data, int len){
    if(!driver_ready) return;
    if(len > TX_BUF_SIZE) len = TX_BUF_SIZE;
    // check free descriptor
    uint32_t next = tx_tail;
    // if status indicates busy, skip (very simple)
    // copy data into buffer
    memcpy(tx_bufs[next], data, len);
    tx_ring[next].buffer_addr = (uint64_t)(uintptr_t)tx_bufs[next];
    tx_ring[next].length = (uint16_t)len;
    tx_ring[next].cmd = 0x1B; // RS|IFCS|EOP|RS? use conservative
    tx_ring[next].status = 0;
    // advance TDT
    tx_tail = (tx_tail + 1) % TX_DESC_COUNT;
    e1000_writel(E1000_TDT, tx_tail);
}

void rtl8139_poll(void){
    if(!driver_ready) return;
    // handle RX: check descriptors starting from (RDT+1) wrapping
    uint32_t rd = e1000_readl(E1000_RDT);
    // descriptors with status bit set indicate packet present. Loop all.
    for(uint32_t i=0;i<RX_DESC_COUNT;i++){
        if(rx_ring[i].status & 0x01){
            int len = rx_ring[i].length;
            if(len > 0 && rx_bufs[i]) net_rx(rx_bufs[i], len);
            // clear status and advance tail pointer
            rx_ring[i].status = 0;
            // update RDT to indicate we consumed descriptor
            e1000_writel(E1000_RDT, i);
        }
    }
}
