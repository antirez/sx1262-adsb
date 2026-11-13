#ifndef LILYGO_RECEIVER_H
#define LILYGO_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SAMPLES = 256, HISTORY = 84, AVERAGE = 80 };

typedef struct {
    uint32_t block, dial_hz;
    int64_t first_ms, last_ms; /* Last armed poll and first completed poll. */
    uint8_t floor, threshold, raw[SAMPLES];
} Capture;

typedef struct {
    Capture capture;
    uint8_t word[14];
    unsigned length;           /* Zero means an undecoded power recording. */
    int anchor, score, rank;
    const char *method;
    unsigned evidence_index, evidence_count; /* One-based for a DF11 support trace. */
    unsigned support_count;
    uint32_t support[5];
} Result;

typedef void (*Report)(const Result *result);

/* When the input queue backs up, reserve space for plausible short or long
 * replies. Call after decoder_init. */
typedef enum { NO_REPLY, SHORT_REPLY, LONG_REPLY } ReplyShape;
ReplyShape decoder_candidate(const Capture *capture);

/* One decoder owns its scratch, tracks and pending captures. Call these from
 * the same task. A second task can queue radio captures at any time. */
bool decoder_init(Report report);
bool decoder_self_test(void);
void decoder_capture(const Capture *capture);
bool decoder_idle(int64_t now_ms);
unsigned decoder_evictions(void);
uint32_t modes_crc(const uint8_t *word, size_t length);

void observation(char *line, size_t capacity, const Result *r, uint32_t boot,
                 int64_t now, unsigned test_count, unsigned deferred_drops);

#endif
