#include "pci.h"
#include "io.h"


static inline uint32_t pci_config_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset){
return (uint32_t)(0x80000000u |
((uint32_t)bus << 16) |
((uint32_t)slot << 11) |
((uint32_t)func << 8) |
(offset & 0xFC));
}


uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset){
outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus,slot,func,offset));
return inl(PCI_CONFIG_DATA);
}


uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset){
uint32_t v = pci_config_read32(bus,slot,func,offset & 0xFC);
int shift = (offset & 2) * 8;
return (uint16_t)((v >> shift) & 0xFFFF);
}


uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset){
uint32_t v = pci_config_read32(bus,slot,func,offset & 0xFC);
int shift = (offset & 3) * 8;
return (uint8_t)((v >> shift) & 0xFF);
}


void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value){
outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus,slot,func,offset));
outl(PCI_CONFIG_DATA, value);
}


int pci_find_device(uint16_t vendor, uint16_t device,
uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func){
for(uint8_t bus=0; bus<256; ++bus){
for(uint8_t slot=0; slot<32; ++slot){
for(uint8_t func=0; func<8; ++func){
uint16_t v = pci_config_read16(bus,slot,func,0x00);
if(v == 0xFFFF) continue; // no device
uint16_t ven = v;
uint16_t dev = pci_config_read16(bus,slot,func,0x02);
if(ven==vendor && dev==device){
if(out_bus) *out_bus=bus; if(out_slot) *out_slot=slot; if(out_func) *out_func=func;
return 1;
}
// if header type says single func, break early
uint8_t ht = pci_config_read8(bus,slot,0,0x0E);
if((ht & 0x80)==0 && func==0) break;
}
}
}
return 0;
}