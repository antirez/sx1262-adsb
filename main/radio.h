#ifndef LILYGO_RADIO_H
#define LILYGO_RADIO_H
#include "receiver.h"


enum { SCAN_ARMED = 0x0f, SCAN_COMPLETE = 0xff, SCAN_WORDS = 386 };
uint32_t sampler_word(unsigned index, bool test);
int radio_bus_init(void);
int radio_start(bool test);
int radio_tune(uint32_t hz);
int radio_rssi(uint8_t *code);
int radio_self_test(void);
int radio_state(uint8_t *state);
int radio_read(uint8_t raw[SAMPLES]);
int radio_arm(uint8_t threshold);
int radio_finish(void);
int radio_calibrate(uint8_t *floor, uint8_t *threshold);
#endif
