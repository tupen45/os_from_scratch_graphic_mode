#pragma once
#include <stdint.h>

int  rtl8139_init(void);
void rtl8139_poll(void);
void nic_tx(const void *data, int len);

/* NEW: query if driver finished init successfully */
int  rtl8139_is_ready(void);
