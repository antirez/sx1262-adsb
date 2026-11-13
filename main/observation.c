/* The two boards export one recording format for host tools. */
#include "receiver.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void hex(const uint8_t *bytes, size_t n, char *out) {
    const char digits[]="0123456789abcdef";
    for (size_t i=0; i<n; i++) {
        *out++=digits[bytes[i]>>4];
        *out++=digits[bytes[i]&15];
    }
    *out=0;
}

/* newlib-nano on the T-Echo supports 64-bit arithmetic but its printf
 * leaves %lld arguments unread. Keep long uptimes portable on both boards. */
static char *decimal(int64_t value, char buffer[22]) {
    char *p=buffer+21;
    uint64_t n=value<0 ? -(uint64_t)value : (uint64_t)value;
    *p=0;
    do { *--p='0'+n%10; } while ((n/=10));
    if (value<0) *--p='-';
    return p;
}

void observation(char *line, size_t capacity, const Result *r, uint32_t boot,
                 int64_t now, unsigned test_count, unsigned deferred_drops) {
    const Capture *c=&r->capture;
    char raw[2*SAMPLES+1], extra[256]="";
    hex(c->raw,SAMPLES,raw);
    if (r->length) {
        char word[29];
        hex(r->word,r->length,word);
        const char *key=r->evidence_index?"df11_support":r->length==7?"df11":r->word[0]>>3==2?"test_word":"df17";
        snprintf(extra,sizeof(extra),",\"%s\":\"%s\",\"rank\":%d,\"model\":\"%s\",\"test_count\":%u",
                 key,word,r->rank,r->method,test_count);
        size_t used=strlen(extra);
        if (r->evidence_count)
            snprintf(extra+used,sizeof(extra)-used,",\"stack_index\":%u,\"stack_size\":%u",
                     r->evidence_index,r->evidence_count);
        if (r->support_count) {
            const char *key=!strcmp(r->method,"repeated DF17")?"repeat_blocks":"header_blocks";
            used+=snprintf(extra+used,sizeof(extra)-used,",\"%s\":[",key);
            for (unsigned i=0; i<r->support_count; i++)
                used+=snprintf(extra+used,sizeof(extra)-used,"%s%"PRIu32,i?",":"",r->support[i]);
            strcpy(extra+used,"]");
        }
    }
    char t[22], first[22], last[22];
    snprintf(line,capacity,"{\"boot\":\"%08"PRIx32"\",\"block\":%"PRIu32",\"uptime_ms\":%s"
        ",\"capture_ms\":[%s,%s],\"signal_hz\":1090000000,\"dial_hz\":%"PRIu32","
        "\"quiet_code\":%u,\"trigger_code\":%u,\"anchor\":%d,\"preamble_score\":%d,"
        "\"period_us\":0.75,\"trigger_samples\":80,\"history_samples\":84,\"trigger_stride\":1,"
        "\"rf_profile\":\"1 MHz / fast RSSI\",\"sampler\":\"onchip\",\"deferred_drops\":%u,\"raw\":\"%s\"%s}",
        boot,c->block,decimal(now,t),decimal(c->first_ms,first),decimal(c->last_ms,last),c->dial_hz,c->floor,c->threshold,r->anchor,r->score,deferred_drops,raw,extra);
}
