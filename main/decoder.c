/* A Mode S bit lasts 1 us. The radio gives us a power reading every 0.75 us,
 * and its filter spreads a pulse across several readings. Comparing two
 * adjacent readings would throw much of the evidence away. The existing C
 * trellis instead scores the waveform predicted by a sequence of bits. */
#include "receiver.h"
#include "selftest.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "demod/preamble.c"
#include "demod/sequence.c"
#include "demod/repeat.c"
#include "demod/df11_stack.c"

#define ASSET(name) extern const uint8_t name##_data[], name##_end[]
#define ASSET_SIZE(name) ((uintptr_t)name##_end - (uintptr_t)name##_data)
ASSET(shape);
ASSET(measured);
ASSET(registers);
ASSET(standard);
ASSET(fast);
ASSET(short_reply);

typedef struct {
    const sx_model *table;
    size_t size;
    const char *name;
} Response;

enum { RF, REG, BAND1090, FAST1090, SHORT };
static Response models[5];
static uint8_t test_shape[4+8*10];
static sx_rank_workspace *work;
/* The short-reply aligner and adaptive long-frame fit run at different
 * stages. Neither keeps useful data in its scratch between calls. */
static union {
    adaptive_scratch adaptive;
    stack_work align;
} *extra;
static repeat_bank *repeats;
static stack_data *short_sum;
static Report publish;
static unsigned drops;

uint32_t modes_crc(const uint8_t *word, size_t length) {
    return sx_crc(word,length);
}

static Response response(const uint8_t *start, const uint8_t *end, const char *name) {
    return (Response){(const sx_model *)start,(uintptr_t)end-(uintptr_t)start,name};
}

/* assets.cmake inserts each measured middle row into the RF model at
 * build time. On boards without PSRAM these two copies must stay in flash. */
_Static_assert(offsetof(sx_model,expected[0][3])==1296 &&
               sizeof(((sx_model *)0)->expected[0])==1792 &&
               sizeof(((sx_model *)0)->expected[0][3])==256,"model layout");

/* The normal preamble template includes the five known DF17 bits. The bench
 * fixture has a different DF, so use only the first ten samples for it. */
static void make_test_shape(void) {
    memcpy(test_shape,shape_data,4);
    test_shape[0] = 10;
    for (int phase = 0; phase < 8; phase++) {
        const int8_t *src = (const int8_t *)shape_data+4+phase*shape_data[0];
        int8_t *dst = (int8_t *)test_shape+4+phase*10;
        int sum = 0, scale = 1, correction = 0;
        for (int i = 0; i < 10; i++) sum += src[i];
        for (int i = 0; i < 10; i++) {
            int amplitude = abs(10*src[i]-sum);
            if (amplitude > scale) scale = amplitude;
        }
        for (int i = 0; i < 10; i++) {
            dst[i] = lrint(48.0*(10*src[i]-sum)/scale);
            correction += dst[i];
        }
        dst[9] -= correction;
    }
}

/* A 112-bit reply fills about twenty 6 us bins. A 56-bit reply fills ten.
 * Count bins that carry both power and modulation: a strong narrow spike
 * can raise the average power, and a steady carrier can have the right
 * duration, but neither contains a train of half-microsecond pulses. */
ReplyShape decoder_candidate(const Capture *c) {
    enum { BIN=8, BINS=SAMPLES/BIN };
    int level[BINS], peak=0, active=0;
    bool changing[BINS];
    for (int b=0; b<BINS; b++) {
        const uint8_t *p=c->raw+b*BIN;
        int sum=p[0], variation=0;
        for (int i=1; i<BIN; i++) {
            sum+=p[i];
            variation+=abs((int)p[i]-p[i-1]);
        }
        level[b]=sum;
        changing[b]=variation>=2*(BIN-1); /* One dB per step on average. */
    }
    int before=(level[0]+level[1])/2;
    int after=(level[BINS-2]+level[BINS-1])/2;
    int floor=before<after ? before : after;
    for (int b=0; b<BINS; b++) {
        int rise=floor-level[b];
        if (rise>peak) peak=rise;
    }
    /* The relative threshold excludes the tail of a strong spike. Keep
     * the absolute threshold low enough for a reply near the noise floor. */
    int threshold=peak/3;
    if (threshold<4*BIN) threshold=4*BIN;
    for (int b=0; b<BINS; b++)
        if (changing[b] && floor-level[b]>=threshold) active++;
    if (active<7) return NO_REPLY;
    if (active<14) {
        int score;
        return sx_short_gate(c->raw,SAMPLES,750000,0,SAMPLES,&score)>=0 ? SHORT_REPLY : NO_REPLY;
    }
    candidate_t hit[2];
    if (sx_preambles_model(c->raw,SAMPLES,shape_data,ASSET_SIZE(shape),hit,2,4,3500) ||
        sx_preambles_model(c->raw,SAMPLES,test_shape,sizeof(test_shape),hit,2,4,3500)) return LONG_REPLY;
    return NO_REPLY;
}

/* A completed Result is always checked independently of the search that
 * proposed it. The same checks protect the host replay and the OLED. */
static bool accept(Result *r, int rank, const char *method, int df) {
    size_t length = df == 11 ? 7 : 14;
    if (rank <= 0 || r->word[0]>>3 != df || modes_crc(r->word,length)) return false;
    if (df == 2 && memcmp(r->word+1,"TEST",4)) return false;
    r->rank = rank;
    r->method = method;
    r->length = length;
    return true;
}

static bool try_model(Result *r, int which) {
    Response m = models[which];
    int rank = sx_decode_latch(r->capture.raw,SAMPLES,r->anchor,m.table,m.size,
                               work,sizeof(*work),r->word);
    return accept(r,rank,m.name,17);
}

bool decoder_self_test(void) {
    Result r = {0};
    candidate_t candidates[12];
    memcpy(r.capture.raw,garden_raw,SAMPLES);
    int n = sx_preambles_model(garden_raw,SAMPLES,shape_data,ASSET_SIZE(shape),candidates,12,4,3500);
    for (int i = 0; i < n; i++) {
        r.anchor = candidates[i].sample;
        if (try_model(&r,RF) && !memcmp(r.word,garden_word,sizeof(garden_word))) return true;
    }
    return false;
}

static void position(Result *r, candidate_t p) {
    r->anchor = p.sample;
    r->score = p.score;
}

/* A long reply may be hard enough that fitting it would hold up newer
 * captures. Keep its original timestamp and come back when the queue drains. */
#ifndef MAX_PENDING
#define MAX_PENDING 96
#endif
enum FitStage { FIT_1090, FIT_MEASURED, FIT_HEADER, FIT_RESIDUAL, FIT_REPEAT, FIT_DONE };
typedef struct {
    Result r;
    enum FitStage stage;
    bool strong;
} Pending;
static Pending *pending;
static unsigned head, count;

typedef struct {
    int64_t when;
    uint32_t block;
    uint8_t bits[27];
} Track;

static Track known[32], missed[40];
static unsigned next_known, next_miss;
static Result family[16];
static unsigned family_slot;
static uint8_t address_hint[7];
static int64_t hint_ms = -60001, short_ms = -2001, short_logged = -30001;
static unsigned short_count, short_checked;
static struct { Capture capture; int anchor, score; } short_group[24];

static void remember(const Result *r) {
    Track *t = &known[next_known++ % 32];
    t->when = r->capture.last_ms;
    t->block = r->capture.block;
    memcpy(t->bits,r->word,4);
    for (int i = 0; i < 40; i++)
        if (missed[i].block == t->block) missed[i].block = 0;
}

static bool recent(const Track *t, const Capture *c, int limit) {
    int64_t age = c->first_ms-t->when;
    return t->block && t->block < c->block && age >= 40 && age <= limit;
}

static int distance_from_header(const uint8_t *profile, const uint8_t *header) {
    int wrong = 0, total = 0;
    for (int i = 0; i < 27; i++) {
        int bit = i+5, weight = abs(profile[i]-128);
        total += weight;
        if ((profile[i]>128) != !!(header[bit/8] & (128>>(bit%8)))) wrong += weight;
    }
    return total >= 160 ? 1024*wrong/total : 1024;
}

typedef struct {
    uint8_t bits[4];
    int distance;
    bool verified;
    uint32_t source[3];
} Header;

static int plausible_headers(const Capture *c, const uint8_t *profile, Header headers[2]) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (!recent(&known[i],c,60000)) continue;
        int d = distance_from_header(profile,known[i].bits);
        if (d > 327) continue;
        int j = 0;
        while (j < n && memcmp(headers[j].bits,known[i].bits,4)) j++;
        if (j < n) {
            if (known[i].block > headers[j].source[0]) headers[j].source[0] = known[i].block;
            continue;
        }
        j = 0;
        while (j < n && (d > headers[j].distance ||
               (d == headers[j].distance && memcmp(known[i].bits,headers[j].bits,4)>0))) j++;
        if (j == 2) continue;
        if (n && j == 0) headers[1] = headers[0];
        memcpy(headers[j].bits,known[i].bits,4);
        headers[j].distance = d;
        headers[j].verified = true;
        headers[j].source[0] = known[i].block;
        if (n < 2) n++;
    }
    if (n == 2) return n;
    uint8_t bank[40][27], guesses[2][7];
    uint32_t blocks[40];
    int rows = 0;
    for (int i = 0; i < 40; i++) {
        Track *t = &missed[(next_miss+i)%40];
        if (recent(t,c,12000)) {
            blocks[rows] = t->block;
            memcpy(bank[rows++],t->bits,27);
        }
    }
    int guesses_found = sx_header_guesses(bank[0],rows,profile,guesses[0]);
    for (int i = 0; i < guesses_found && n < 2; i++) {
        if (n && !memcmp(headers[0].bits,guesses[i],4)) continue;
        memcpy(headers[n].bits,guesses[i],4);
        for (int j=0; j<3; j++) headers[n].source[j] = blocks[guesses[i][4+j]];
        headers[n++].verified = false;
    }
    return n;
}

static bool looks_like_status(const uint8_t *p) {
    int type = 0, reserved = 0, wrong_type = 0, wrong_reserved = 0;
    for (int i = 0; i < 40; i++) {
        int value = p[27+i]-128, weight = abs(value);
        if (i < 8) {
            type += weight;
            if ((value>0) != !!(0xe1 & (128>>i))) wrong_type += weight;
        } else {
            reserved += weight;
            if (value > 0) wrong_reserved += weight;
        }
    }
    return type>=32 && reserved>=128 && 3*wrong_type<=type &&
           3*wrong_reserved<=reserved && 4*(wrong_type+wrong_reserved)<=type+reserved;
}

/* An address heard earlier can constrain 24 bits of a weak burst. Require
 * the rest of the word, including parity, to agree. Discard competing words. */
static bool try_history(Result *r) {
    const int order[] = {BAND1090, RF, REG};
    uint8_t profiles[3][67], combined[67];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        Response m = models[order[i]];
        if (sx_status_profile(r->capture.raw,SAMPLES,r->anchor,m.table,m.size,
                              work,sizeof(*work),profiles[n])>0) n++;
    }
    if (n < 2) return false;
    for (int i = 0; i < 67; i++) {
        int a = profiles[0][i], b = profiles[1][i];
        if (n == 2) combined[i] = (a+b)/2;
        else {
            int c = profiles[2][i], lo = a < b ? a : b, hi = a > b ? a : b;
            combined[i] = c < lo ? lo : c > hi ? hi : c;
        }
    }
    Header headers[2];
    int nh = plausible_headers(&r->capture,combined,headers);
    bool status = looks_like_status(combined);
    uint8_t first[14];
    int first_rank = 0;
    const char *method = "DF17 history";
    for (int mode = 0; mode < 2; mode++) {
        if (mode && (first_rank || !status)) break;
        for (int h = mode ? -1 : 0; h < nh; h++) {
            if (mode && h >= 0 && !headers[h].verified) continue;
            const uint8_t *bits = h < 0 ? NULL : headers[h].bits;
            for (int i = 0; i < (mode ? 1 : 3); i++) {
                Response m = models[order[i]];
                int rank = (mode ? sx_decode_status : sx_decode_header)(r->capture.raw,
                    SAMPLES,r->anchor,m.table,m.size,work,sizeof(*work),bits,h<0?0:4,r->word);
                if (rank <= 0) continue;
                if (first_rank && memcmp(first,r->word,14)) return false;
                if (!first_rank) {
                    memcpy(first,r->word,14);
                    first_rank = rank;
                    if (h>=0) {
                        r->support_count = headers[h].verified ? 1 : 3;
                        memcpy(r->support,headers[h].source,r->support_count*sizeof(*r->support));
                    }
                    if (mode) method = "DF17 status";
                }
            }
        }
    }
    if (first_rank) {
        memcpy(r->word,first,14);
        return accept(r,first_rank,method,17);
    }
    Track *t = &missed[next_miss++ % 40];
    t->when = r->capture.last_ms;
    t->block = r->capture.block;
    memcpy(t->bits,combined,27);
    return false;
}

/* Short all-call replies carry the transmitter's address. Several aligned
 * replies must agree independently before that address can help a long one. */
static void collect_short(const Capture *c) {
    int score;
    int at = sx_short_gate(c->raw,SAMPLES,750000,0,SAMPLES,&score);
    if (at < 0) return;
    if (c->last_ms-short_ms > 2000 || short_count == 24) {
        memset(short_sum,0,sizeof(*short_sum));
        short_count = short_checked = 0;
    }
    Response m = models[SHORT];
    if (sx_stack_add(c->raw,SAMPLES,at,m.table,m.size,short_sum,sizeof(*short_sum),
                     &extra->align,sizeof(extra->align)) > 0) {
        short_group[short_count].capture = *c;
        short_group[short_count].anchor = at;
        short_group[short_count++].score = score;
        short_ms = c->last_ms;
    }
}

static void confirm_family(void) {
    for (int i = 0; i < 16; i++) {
        Result *r = &family[i];
        if (!r->length || llabs(r->capture.last_ms-hint_ms)>60000 ||
            memcmp(r->word+1,address_hint+1,3)) continue;
        publish(r);
        for (unsigned j = 0; j < count; j++) {
            Pending *p = &pending[(head+j)%MAX_PENDING];
            if (p->r.capture.block == r->capture.block) p->stage = FIT_DONE;
        }
        r->length = 0;
    }
}

static void check_short(int64_t now) {
    if (short_count < 5 || short_count == short_checked || now-short_ms < 40) return;
    short_checked = short_count;
    Response m = models[SHORT];
    uint8_t words[6][14], raw[160], *winner = NULL;
    int votes[6] = {0}, used = 0;
    for (int phase = 0; phase < 6; phase++) {
        if (sx_stack_phase(short_sum,sizeof(*short_sum),m.table,m.size,phase,raw,160)!=160) return;
        uint8_t word[14];
        if (sx_decode11(raw,160,30,m.table,m.size,work,sizeof(*work),word)<=0) continue;
        if (used && memcmp(words[0]+1,word+1,3)) return;
        int i = 0;
        while (i < used && memcmp(words[i],word,7)) i++;
        if (i == used) memcpy(words[used++],word,7);
        votes[i]++;
    }
    for (int i = 0; i < used; i++) {
        if (votes[i] < 2 || modes_crc(words[i],7)) continue;
        if (winner) return;
        winner = words[i];
    }
    if (!winner) return;
    bool show = short_ms-short_logged >= 30000 || memcmp(address_hint,winner,7);
    memcpy(address_hint,winner,7);
    hint_ms = short_ms;
    if (show) {
        Result r = {.capture=short_group[short_count-1].capture,.evidence_count=short_count};
        memcpy(r.word,winner,7);
        if (accept(&r,1,"DF11 stack",11)) {
            publish(&r);
            for (unsigned i = 0; i < short_count; i++) {
                r.evidence_index = i+1;
                r.capture = short_group[i].capture;
                r.anchor = short_group[i].anchor;
                r.score = short_group[i].score;
                publish(&r);
            }
        }
        short_logged = short_ms;
    }
    confirm_family();
}

static bool try_family(Result *r, candidate_t *positions, int n, bool hint) {
    Result found = {0};
    Response a = models[BAND1090], b = models[RF];
    for (int i = 0; i < n; i++) {
        position(r,positions[i]);
        int rank = sx_decode_family(r->capture.raw,SAMPLES,r->anchor,a.table,a.size,b.table,b.size,
                                    work,sizeof(*work),r->word);
        if (!accept(r,rank,"DF11-confirmed",17)) continue;
        if (found.length && memcmp(found.word,r->word,14)) return false;
        if (!found.length || r->score > found.score) found = *r;
    }
    if (!found.length) return false;
    family[family_slot++%16] = found;
    if (hint && !memcmp(found.word+1,address_hint+1,3)) {
        confirm_family();
        return true;
    }
    return false;
}

static void defer(Result *r) {
    int at = r->anchor;
    if (at < 12 || at+173 > SAMPLES) return;
    const uint8_t *raw = r->capture.raw;
    int before = 0, after = 0, body = 0;
    for (int i = 0; i < 8; i++) {
        before += raw[at-12+i];
        after += raw[at+165+i];
    }
    for (int i = 16; i < 144; i++) body += raw[at+i];
    int rise = 16*(before<after ? before : after)-body;
    if (rise < 6*128) return;
    if (count == MAX_PENDING) {
        head = (head+1)%MAX_PENDING;
        count--;
        drops++;
    }
    Pending *p = &pending[(head+count++)%MAX_PENDING];
    p->r = *r;
    p->strong = rise >= 14*128;
    p->stage = p->strong ? FIT_1090 : rise >= 12*128 ? FIT_HEADER : FIT_REPEAT;
}

unsigned decoder_evictions(void) {
    return drops;
}

bool decoder_idle(int64_t now_ms) {
    check_short(now_ms);
    if (!count) return false;
    Pending *p = &pending[head];
    Result *r = &p->r;
    Response m = models[REG];
    int rank = 0;
    bool ok = false;
    enum FitStage stage = p->stage;
    if (stage != FIT_DONE) p->stage++;
    switch (stage) {
    case FIT_1090:
    case FIT_MEASURED:
        m = models[stage==FIT_1090 ? BAND1090 : RF];
        rank = sx_decode_adaptive(r->capture.raw,SAMPLES,r->anchor,m.table,m.size,work,
                                  sizeof(*work),&extra->adaptive,sizeof(extra->adaptive),r->word);
        ok = accept(r,rank,"per-burst response",17);
        if (ok) remember(r);
        break;
    case FIT_HEADER:
        ok = try_history(r);
        if (!p->strong) p->stage = FIT_REPEAT;
        break;
    case FIT_RESIDUAL:
        r->support_count = 0;
        rank = sx_decode_residual(r->capture.raw,SAMPLES,r->anchor,m.table,m.size,
                                  work,sizeof(*work),r->word);
        ok = accept(r,rank,"paired residual",17);
        break;
    case FIT_REPEAT: {
        r->support_count = 0;
        uint32_t support[6];
        rank = sx_repeat_add(r->capture.raw,SAMPLES,r->anchor,m.table,m.size,work,sizeof(*work),
                             repeats,sizeof(*repeats),r->capture.block,r->capture.last_ms,r->word,support);
        ok = accept(r,rank,"repeated DF17",17);
        if (ok && support[0]<=5) {
            r->support_count = support[0];
            memcpy(r->support,support+1,r->support_count*sizeof(*support));
        }
        break;
    }
    case FIT_DONE:
        break;
    }
    if (ok) {
        for (int i = 0; i < 16; i++)
            if (family[i].capture.block == r->capture.block) family[i].length = 0;
        publish(r);
    }
    if (ok || p->stage == FIT_DONE) {
        head = (head+1)%MAX_PENDING;
        count--;
    }
    return true;
}

void decoder_capture(const Capture *c) {
    static uint32_t previous_hz;
    if (previous_hz!=c->dial_hz) {
        /* A repeat or an address hint belongs to the channel that heard it.
         * Raw captures are tagged by the radio before entering the queue. */
        drops+=count;
        head=count=short_count=short_checked=next_known=next_miss=family_slot=0;
        short_ms=-2001;
        hint_ms=-60001;
        short_logged=-30001;
        memset(known,0,sizeof(known));
        memset(missed,0,sizeof(missed));
        memset(family,0,sizeof(family));
        memset(address_hint,0,sizeof(address_hint));
        memset(repeats,0,sizeof(*repeats));
        memset(short_sum,0,sizeof(*short_sum));
        previous_hz=c->dial_hz;
    }
    Result r = {.capture=*c};
    candidate_t positions[12], best = {0}, fixture[3];
    int n = sx_preambles_model(c->raw,SAMPLES,shape_data,ASSET_SIZE(shape),positions,12,4,3500);
    bool hint = c->last_ms >= hint_ms && c->last_ms-hint_ms <= 60000;
    collect_short(c);
    for (int i = 0; i < n; i++) {
        position(&r,positions[i]);
        if (r.score > best.score) best = positions[i];
        if (try_model(&r,RF) || try_model(&r,REG)) goto independent;
    }
    if (hint) {
        Response m = models[REG];
        for (int i = 0; i < n; i++) {
            position(&r,positions[i]);
            int rank = sx_decode_tracked(c->raw,SAMPLES,r.anchor,m.table,m.size,work,
                                         sizeof(*work),r.word,address_hint,7);
            if (accept(&r,rank,"DF11-assisted",17)) goto done;
        }
    }
    for (int i = 0; i < n; i++) {
        position(&r,positions[i]);
        if (try_model(&r,BAND1090)) goto independent;
    }
    position(&r,best);
    if (n && try_model(&r,FAST1090)) goto independent;
    if (n && try_family(&r,positions,n,hint)) return;
    int nf = sx_preambles_model(c->raw,SAMPLES,test_shape,sizeof(test_shape),fixture,3,4,3500);
    Response m = models[BAND1090];
    for (int i = 0; i < nf; i++) {
        position(&r,fixture[i]);
        int rank = sx_decode_test(c->raw,SAMPLES,r.anchor,m.table,m.size,work,sizeof(*work),r.word);
        if (accept(&r,rank,"TEST",2)) goto done;
    }
    position(&r,best);
    r.length = 0;
    if (n) defer(&r);
    goto done;
independent:
    remember(&r);
done:
    publish(&r);
}

bool decoder_init(Report report) {
    publish = report;
    models[RF] = response(measured_data,measured_end,"measured RF");
    models[REG] = response(registers_data,registers_end,"register updates");
    models[SHORT] = response(short_reply_data,short_reply_end,"short replies");
    models[BAND1090] = response(standard_data,standard_end,"1090 MHz RF");
    models[FAST1090] = response(fast_data,fast_end,"1090 MHz fast RSSI");
    make_test_shape();
    work = calloc(1,sizeof(*work));
    extra = calloc(1,sizeof(*extra));
    repeats = calloc(1,sizeof(*repeats));
    short_sum = calloc(1,sizeof(*short_sum));
    pending = calloc(MAX_PENDING,sizeof(*pending));
    return work && extra && repeats && short_sum && pending;
}
