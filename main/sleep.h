#ifndef ADSB_SLEEP_H
#define ADSB_SLEEP_H
#include "receiver.h"
#include <stdatomic.h>

enum { SLEEP_AFTER_MS = 30*60*1000 };
typedef enum { LISTENING, STOP_RADIO, RADIO_STOPPED, DECODER_DRAINED } SleepPhase;
typedef struct {
    atomic_int phase;
    int64_t last_packet;       /* Field task: newest accepted capture this boot. */
    bool manual;               /* Sleep was explicitly selected in settings. */
    int64_t stopped_at;        /* Radio publishes this before RADIO_STOPPED. */
} SleepControl;

void sleep_received(SleepControl *s, const Result *r);
void sleep_radio_stopped(SleepControl *s, int64_t now);
void sleep_decoder_idle(SleepControl *s, int64_t now);
bool sleep_due(SleepControl *s, int64_t now, bool results_empty);
#endif
