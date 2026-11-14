#include "sleep.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr); exit(1); } } while (0)

int main(void) {
    SleepControl s={.last_packet=1000};
    int64_t deadline=1000+SLEEP_AFTER_MS;
    Result r={.capture.last_ms=deadline-10};
    sleep_received(&s,&r);                      /* Just noise. */
    r.length=7; r.evidence_index=1;
    sleep_received(&s,&r);                      /* Supporting trace of a previously logged word. */
    CHECK(s.last_packet==1000);
    CHECK(!sleep_due(&s,deadline-1,false) && s.phase==LISTENING);
    CHECK(!sleep_due(&s,deadline,false) && s.phase==STOP_RADIO);
    sleep_radio_stopped(&s,deadline);
    sleep_decoder_idle(&s,deadline+39);
    CHECK(s.phase==RADIO_STOPPED);
    sleep_decoder_idle(&s,deadline+50);
    CHECK(!sleep_due(&s,deadline+51,false) && s.phase==DECODER_DRAINED);
    /* A valid reply recovered from the backlog cancels automatic sleep.
     * A late decode of an older capture cannot move the deadline back. */
    r.evidence_index=0;
    sleep_received(&s,&r);
    r.capture.last_ms-=500; sleep_received(&s,&r);
    CHECK(s.last_packet==deadline-10);
    CHECK(!sleep_due(&s,deadline+52,true) && s.phase==LISTENING);
    deadline=s.last_packet+SLEEP_AFTER_MS;
    CHECK(!sleep_due(&s,deadline,false) && s.phase==STOP_RADIO);
    sleep_radio_stopped(&s,deadline);
    sleep_decoder_idle(&s,deadline+50);
    CHECK(sleep_due(&s,deadline+50,true));
    /* Manual sleep still finishes writing a last-minute valid packet. */
    s=(SleepControl){.manual=true,.last_packet=1000};
    CHECK(!sleep_due(&s,1001,false) && s.phase==STOP_RADIO);
    sleep_radio_stopped(&s,1001);
    r.capture.last_ms=1001; sleep_received(&s,&r);
    sleep_decoder_idle(&s,1051);
    CHECK(!sleep_due(&s,1051,false));
    CHECK(sleep_due(&s,1051,true));
}
