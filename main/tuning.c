/* One switch has three jobs. Wait briefly before delivering a single tap:
 * if a second press arrives it belongs to the double-click, and browsing
 * must not move underneath the newly opened menu. */
#include "tuning.h"
#include <stdio.h>
#include <string.h>

Key button_update(Button *b, bool pressed, int64_t now) {
    if (pressed!=b->sample) { b->sample=pressed; b->changed=now; }
    if (b->waiting && !b->down && now-b->released>=300 &&
        (!pressed || b->changed-b->released>=300)) {
        b->waiting=false;
        return KEY_TAP;
    }
    if (now-b->changed>=25 && pressed!=b->down) {
        b->down=pressed;
        if (pressed) {
            b->pressed=b->changed;
            b->held=false;
        } else if (!b->held) {
            if (b->changed-b->pressed>=800) { b->waiting=false; return KEY_HOLD; }
            if (b->waiting) { b->waiting=false; return KEY_DOUBLE; }
            b->waiting=true;
            b->released=b->changed;
        }
    }
    if (b->down && pressed && !b->held && now-b->pressed>=800) {
        b->held=true;
        b->waiting=false;
        b->repeat=now+450;
        return KEY_HOLD;
    }
    if (b->down && pressed && b->held && now>=b->repeat) {
        b->repeat=now+250;
        return KEY_REPEAT;
    }
    return KEY_NONE;
}

/* Look for a rise and a fall. A 36 us mean fits inside a short or a long
 * reply. Compare it with the measured floor and both ends of this capture
 * so a lone spike or a change in the background doesn't freeze the screen. */
static bool has_envelope(const Capture *burst) {
    enum { EDGE=16, WINDOW=48, RISE=12 }; /* RSSI codes are half a dB. */
    int before=0, after=0;
    for (int i=0; i<EDGE; i++) {
        before+=burst->raw[i];
        after+=burst->raw[SAMPLES-EDGE+i];
    }
    int floor=EDGE*burst->floor;
    if (before<floor) floor=before;
    if (after<floor) floor=after;
    int limit=(floor-EDGE*RISE)*(WINDOW/EDGE);
    int sum=0;
    for (int i=EDGE; i<SAMPLES-EDGE; i++) {
        sum+=burst->raw[i];
        if (i>=EDGE+WINDOW) sum-=burst->raw[i-WINDOW];
        if (i+1>=EDGE+WINDOW && sum<=limit) return true;
    }
    return false;
}

static void record_burst(Scope *s, Column *c, const Capture *burst, int64_t now) {
    enum { HOLD_MS=3000, LIVE_MS=1000 };
    uint8_t peak=255;
    for (int i=0; i<SAMPLES; i++)
        if (burst->raw[i]<peak) peak=burst->raw[i];
    c->triggers++;
    if (peak<c->burst_peak) c->burst_peak=peak;
    if (peak<c->peak) c->peak=peak;

    /* Keep this exact trace for the whole hold. Even in dense traffic the
     * user gets a second of live floor before another envelope can latch. */
    if (s->hold_until_ms && now<s->hold_until_ms+LIVE_MS) return;
    if (!has_envelope(burst)) return;
    s->block=burst->block;
    s->hold_until_ms=now+HOLD_MS;
    s->wave_peak=peak;
    _Static_assert(SAMPLES==2*SCOPE_WIDTH,"two samples per scope pixel");
    for (unsigned x=0; x<SCOPE_WIDTH; x++) {
        uint8_t a=burst->raw[2*x], b=burst->raw[2*x+1];
        s->wave[x]=a<b ? a : b;
    }
}

/* The horizontal axis is elapsed time, independent of the OLED refresh
 * rate. Slots carry their time so a stalled poll leaves a gap on the graph.
 * Lower RSSI codes are stronger: each code is half a dB below zero. */
void scope_sample(Scope *s, uint32_t hz, int64_t now, uint8_t rssi, const Capture *burst) {
    if (s->hz!=hz) { memset(s,0,sizeof(*s)); s->hz=hz; }
    uint32_t tick=(uint32_t)(now/COLUMN_MS);
    Column *c=&s->column[tick%SCOPE_WIDTH];
    if (!c->count || c->tick!=tick) *c=(Column){.tick=tick,.peak=255,.burst_peak=255};
    c->sum+=rssi;
    c->count++;
    if (rssi<c->peak) c->peak=rssi;
    if (burst) record_burst(s,c,burst,now);
}

void scope_view(const Scope *s, uint32_t hz, int64_t now, ScopeView *v) {
    memset(v,0,sizeof(*v));
    v->level=v->strongest=-1;
    if (s->hz!=hz) return;
    if (now<s->hold_until_ms) {
        v->zoom=true;
        v->block=s->block;
        v->wave_peak=s->wave_peak;
        memcpy(v->wave,s->wave,sizeof(v->wave));
    }
    uint32_t latest=(uint32_t)(now/COLUMN_MS), sum=0, count=0;
    for (unsigned x=0; x<SCOPE_WIDTH; x++) {
        unsigned age=SCOPE_WIDTH-1-x;
        uint32_t tick=latest-age;
        const Column *c=&s->column[tick%SCOPE_WIDTH];
        if (!c->count || c->tick!=tick) continue;
        v->mean[x]=(c->sum+c->count/2)/c->count;
        v->peak[x]=c->peak;
        v->burst_peak[x]=c->burst_peak;
        v->triggers+=c->triggers;
        v->valid[x]=true;
        v->columns++;
        v->samples+=c->count;
        if (age<10) { sum+=c->sum; count+=c->count; }
        if (age<50 && (v->strongest<0 || c->peak<v->strongest)) v->strongest=c->peak;
    }
    if (count) v->level=(sum+count/2)/count;
}

static const uint32_t steps[]={1000,10000,100000,1000000,10000000};
static const char *labels[]={"1 kHz","10 kHz","100 kHz","1 MHz","10 MHz"};

void menu_open(Menu *m, uint32_t hz) {
    *m=(Menu){.open=true,.hz=hz,.step=2,.action=TUNE_UP};
}

void menu_next(Menu *m) {
    m->action=(m->action+1)%(CLEAR_HISTORY+1);
}

void menu_adjust(Menu *m) {
    uint32_t step=steps[m->step];
    switch (m->action) {
    case TUNE_UP: m->hz=m->hz>MAX_HZ-step ? MAX_HZ : m->hz+step; break;
    case TUNE_DOWN: m->hz=m->hz<MIN_HZ+step ? MIN_HZ : m->hz-step; break;
    case STEP_SIZE: m->step=(m->step+1)%(sizeof(steps)/sizeof(*steps)); break;
    case DEFAULT_FREQ: m->hz=DEFAULT_HZ; break;
    case DISCARD:
    case CLEAR_HISTORY: break;  /* The field task handles these actions. */
    }
}

void menu_screen(const Menu *m, const ScopeView *v, const char *message, Screen s) {
    memset(s,0,sizeof(Screen));
    unsigned mhz=m->hz/1000000, khz=m->hz/1000%1000;
    snprintf(s[0],17,"RX %u.%03u MHz",mhz,khz);
    if (message) snprintf(s[1],17,"%.16s",message);
    else if (v->zoom) snprintf(s[1],17,"Hold -%u dBm",(v->wave_peak+1u)/2);
    else if (v->level>=0 && v->strongest>=0)
        snprintf(s[1],17,"A-%u P-%u dBm",((uint8_t)v->level+1u)/2,((uint8_t)v->strongest+1u)/2);
    else strcpy(s[1],"Reading RSSI...");
    switch (m->action) {
    case TUNE_UP: snprintf(s[6],17,"Hold: +%s",labels[m->step]); break;
    case TUNE_DOWN: snprintf(s[6],17,"Hold: -%s",labels[m->step]); break;
    case STEP_SIZE: snprintf(s[6],17,"Hold: step %s",labels[m->step]); break;
    case DEFAULT_FREQ: strcpy(s[6],"Hold: default"); break;
    case DISCARD: strcpy(s[6],"Hold: discard"); break;
    case CLEAR_HISTORY:
        strcpy(s[6],"Clear history");
        strcpy(s[7],"Hold to erase");
        return;
    }
    strcpy(s[7],"Tap next 2x save");
}
