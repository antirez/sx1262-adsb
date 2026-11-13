#ifndef LILYGO_CPR_H
#define LILYGO_CPR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    unsigned lat, lon;
    int64_t ms;
    bool valid;
} CprSample;

/* Feed CRC-checked DF17 words for one aircraft and one boot in time order.
 * fix_ms is the time of the position, even if later reports have no fix. */
typedef struct {
    CprSample sample[2];
    double lat, lon;
    int64_t fix_ms;
    bool fixed, surface;
} CprTrack;

void cpr_update(CprTrack *track, const uint8_t word[14], int64_t ms);
#endif
