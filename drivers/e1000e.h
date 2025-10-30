#pragma once
#include <stdint.h>

// Expose same interface as rtl8139.h so existing code can use the new driver
int  rtl8139_init(void);
void rtl8139_poll(void);
void nic_tx(const void *data, int len);
int  rtl8139_is_ready(void);
