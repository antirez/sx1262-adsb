/* Stop the producer first. The decoder can still find an aircraft in the
 * last few queued captures. Give it time to finish and the field task time
 * to journal the result before turning off either processor. */
#include "sleep.h"

void sleep_received(SleepControl *s, const Result *r) {
    if (r->length && !r->evidence_index && r->capture.last_ms>s->last_packet)
        s->last_packet=r->capture.last_ms;
}

void sleep_radio_stopped(SleepControl *s, int64_t now) {
    s->stopped_at=now;
    atomic_store(&s->phase,RADIO_STOPPED);
}

/* Called by the decoder only after both fresh and deferred work ran out.
 * Its short-reply stack waits for 40 ms of quiet before its last vote. */
void sleep_decoder_idle(SleepControl *s, int64_t now) {
    if (atomic_load(&s->phase)==RADIO_STOPPED && now-s->stopped_at>=50)
        atomic_store(&s->phase,DECODER_DRAINED);
}

bool sleep_due(SleepControl *s, int64_t now, bool results_empty) {
    bool due=s->manual || now-s->last_packet>=SLEEP_AFTER_MS;
    int phase=atomic_load(&s->phase);
    if (phase==LISTENING && due) atomic_store(&s->phase,STOP_RADIO);
    if (phase!=DECODER_DRAINED || !results_empty) return false;
    if (!due) atomic_store(&s->phase,LISTENING); /* A late decode bought another half hour. */
    return due;
}
