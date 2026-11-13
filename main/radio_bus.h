#ifndef LILYGO_RADIO_BUS_H
#define LILYGO_RADIO_BUS_H
#include "radio.h"

/* Keep ESP-IDF status codes for the existing USB diagnostics. */
enum { RADIO_SPI_BYTES=264, RADIO_BAD_STATE=0x103, RADIO_TIMEOUT=0x107,
       RADIO_BAD_RESPONSE=0x108 };
int radio_exchange(const uint8_t *tx, uint8_t *rx, size_t n);
void radio_reset_pin(bool high);
uint32_t radio_clock_us(void);
void radio_delay_us(unsigned us);
unsigned radio_tcxo(void);
#endif
