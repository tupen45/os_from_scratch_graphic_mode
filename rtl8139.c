// rtl8139.c
// Safer RTL8139 driver for simple kernels (QEMU-friendly).
// - 4 preallocated DMA tx buffers
// - copy payload into DMA buffer before kickstarting TX
// - RX ring handling & rtl8139_poll() guarded until init completes

#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

// Vendor/Device IDs for Realtek RTL8139
#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

// IO registers offsets
#define RL_IDR0     0x00 // MAC[0]
#define RL_RBSTART  0x30
#define RL_CMD      0x37
#define RL_CMD_RX_EN (1<<3)
#define RL_CMD_TX_EN (1<<2)
#define RL_CAPR     0x38
#define RL_IMR      0x3C
#define RL_ISR      0x3E
#define RL_RCR      0x44
#define RL_TCR      0x40
#define RL_CONFIG1  0x52

// RX buffer size
#define RL_RXBUF 8192

// TX buffer settings
#define TX_SLOTS 4
#define TX_BUF_SIZE 1792  // safe headroom for ethernet frames

static uint16_t io_base;
static uint8_t *rx_buf; // physically contiguous
static uint32_t rx_off;

// TX DMA buffers and index
static uint8_t *tx_bufs[TX_SLOTS];
static int tx_idx = 0;

// driver ready flag
static int rtl8139_initialized = 0;

// Inline IO helpers (access IO ports at io_base + reg)
static inline uint8_t  rl_inb(uint16_t r){ return inb(io_base + r); }
static inline uint16_t rl_inw(uint16_t r){ return inw(io_base + r); }
static inline uint32_t rl_inl(uint16_t r){ return inl(io_base + r); }
static inline void rl_outb(uint16_t r, uint8_t v){  outb(io_base + r, v); }
static inline void rl_outw(uint16_t r, uint16_t v){ outw(io_base + r, v); }
static inline void rl_outl(uint16_t r, uint32_t v){ outl(io_base + r, v); }

// simple bump allocator stub provided by kernel
extern void *kmalloc(size_t size);

// If you have virt->phys translation, replace this macro.
#ifndef VIRT_TO_PHYS
#define VIRT_TO_PHYS(v) ((uint32_t)(uintptr_t)(v))
#endif

// Read MAC from IDR0..IDR5
static void read_mac(uint8_t mac[6]){
    for(int i=0;i<6;i++) mac[i]=rl_inb(RL_IDR0+i);
}

int rtl8139_init(void){
    uint8_t bus,slot,func;
    if(!pci_find_device(RTL8139_VENDOR, RTL8139_DEVICE, &bus,&slot,&func)) return -1;

    // Enable IO + Bus Mastering in PCI Command register
    uint32_t cmdsts = pci_config_read32(bus,slot,func,0x04);
    cmdsts |= 0x0005; // IO space + Bus Master
    pci_config_write32(bus,slot,func,0x04,cmdsts);

    // BAR0 gives IO base
    uint32_t bar0 = pci_config_read32(bus,slot,func,0x10);
    io_base = (uint16_t)(bar0 & ~0x3);

    // Power on
    rl_outb(RL_CONFIG1, 0x00);

    // Software reset
    rl_outb(RL_CMD, 0x10);
    while(rl_inb(RL_CMD) & 0x10) { /* wait */ }

    // Allocate RX buffer (8K + 16 + 1500 recommended)
    rx_buf = (uint8_t*)kmalloc(RL_RXBUF + 16 + 1500);
    if(!rx_buf) return -1;
    rx_off = 0;
    rl_outl(RL_RBSTART, VIRT_TO_PHYS(rx_buf));

    // Receive Configuration: accept broadcast & phys match; wrap + RBLEN=8K
    rl_outl(RL_RCR, 0x0005 | (1<<7) | (1<<4)); // AB|AM + WRAP + RBLEN=8K

    // Transmit config: defaults ok
    rl_outl(RL_TCR, 0x00000000);

    // Allocate TX buffers (DMA-safe)
    for(int i=0;i<TX_SLOTS;i++){
        tx_bufs[i] = (uint8_t*)kmalloc(TX_BUF_SIZE);
        if(!tx_bufs[i]) return -1;
    }
    tx_idx = 0;

    // Enable interrupts: RXOK | TXOK (example mask)
    rl_outw(RL_IMR, 0x0005);

    // Enable RX/TX
    rl_outb(RL_CMD, RL_CMD_RX_EN | RL_CMD_TX_EN);

    uint8_t mac[6]; read_mac(mac);
    net_init(mac);

    rtl8139_initialized = 1;   // mark driver ready
    return 0;
}

// expose readiness to kernel
int rtl8139_is_ready(void){
    return rtl8139_initialized;
}

// nic_tx: copy into DMA-safe buffer then kick the NIC
void nic_tx(const void *data, int len){
    if (!rtl8139_initialized) return;

    // enforce ethernet min/max
    if(len < 60) len = 60;
    if(len > TX_BUF_SIZE) len = TX_BUF_SIZE;

    // copy into DMA buffer for slot tx_idx
    memcpy(tx_bufs[tx_idx], data, len);

    // compute TSADn and TSDn offsets
    uint16_t tsad_reg = 0x20 + tx_idx*4; // TSADn
    uint16_t tsd_reg  = 0x10 + tx_idx*4; // TSDn

    // physical address for NIC DMA
    uint32_t phys = VIRT_TO_PHYS(tx_bufs[tx_idx]);

    // write buffer physical address then length (kick)
    rl_outl(tsad_reg, phys);
    rl_outl(tsd_reg, (uint32_t)(len & 0x1FFF)); // writing starts TX

    // advance ring
    tx_idx = (tx_idx + 1) & (TX_SLOTS - 1);
}

// Call this from IRQ handler or poll periodically
void rtl8139_poll(void){
    if (!rtl8139_initialized) return;
    if (!rx_buf) return;

    uint16_t isr = rl_inw(RL_ISR);
    if(!isr) return;
    // Acknowledge ISR bits we read
    rl_outw(RL_ISR, isr);

    // RX OK
    if(isr & 0x01){
        for(;;){
            // Each RX frame: [status(2)][len(2)][data...]
            uint16_t *hdr = (uint16_t*)(rx_buf + rx_off);
            uint16_t status = hdr[0];
            uint16_t plen   = hdr[1];
            if(!(status & 0x01)) break; // no more frames
            uint8_t *pkt = (uint8_t*)(rx_buf + rx_off + 4);
            int frame_len = (int)plen - 4; // strip CRC
            if(frame_len < 0) frame_len = 0;

            // hand packet to network layer
            net_rx(pkt, frame_len);

            // advance offset (align to dword)
            rx_off = (rx_off + 4 + plen + 3) & ~3U;
            // tell NIC we've consumed up to CAPR = rx_off - 16 (per datasheet)
            rl_outw(RL_CAPR, (uint16_t)((rx_off - 16) & 0xFFFF));
        }
    }

    // (optional) handle TXOK/ERROR bits here
}
