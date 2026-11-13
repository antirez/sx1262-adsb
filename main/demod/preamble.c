/* Blind DF17 preamble search in half-decibel SX1262 power readings.
 * At 0.804 us per reading, the known preamble and five DF bits occupy about
 * 16 samples. Rank candidates using their observed shape and sustained rise.
 * All input indices are relative to the first reading in the supplied array.
 */
#include <stdint.h>
#include <stddef.h>
#define PHASE_COUNT 8
#define PRE_SAMPLES 16
#define QUIET_SAMPLES 16
#define BODY_START 18
#define BODY_SAMPLES 128
#define POST_START 153

typedef struct {
    int32_t sample;
    int32_t score;
} candidate_t;

/* Integer square root keeps the native module independent of libgcc and libm. */
static unsigned root32(unsigned value) {
    unsigned result = 0, bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else result >>= 1;
        bit >>= 2;
    }
    return result;
}

static void keep(candidate_t *out, size_t *used, size_t limit, int at, int score) {
    size_t weakest = 0;
    for (size_t i = 0; i < *used; i++) {
        int apart = at - out[i].sample;
        if (apart >= -55 && apart <= 55) {
            if (score > out[i].score) {
                out[i].sample = at;
                out[i].score = score;
            }
            return;
        }
        if (out[i].score < out[weakest].score) weakest = i;
    }
    if (*used < limit) weakest = (*used)++;
    else if (score <= out[weakest].score) return;
    out[weakest].sample = at;
    out[weakest].score = score;
}

static void prepare_norm(const int8_t *shape,int pre_samples,int *shape_norm) {
    for (int p = 0; p < PHASE_COUNT; p++) {
        int s = 0, sq = 0;
        for (int j = 0; j < pre_samples; j++) {
            int x = shape[p*pre_samples+j];
            s += x;
            sq += x*x;
        }
        shape_norm[p] = (int)root32(pre_samples*sq - s*s);
    }
}

static int candidate_score(const uint8_t *raw,int i,const int8_t *shape,int pre_samples,
                           const int *shape_norm,int rise16) {
    int sum = 0, sq = 0;
    for (int j = 0; j < pre_samples; j++) {
        int x = raw[i+j+1];
        sum += x;
        sq += x*x;
    }
    int var = pre_samples*sq - sum*sum;
    if (var > 128) {
        int best = 0;
        int deviation = (int)root32(var);
        for (int p = 0; p < PHASE_COUNT; p++) {
            int dot = 0;
            for (int j = 0; j < pre_samples; j++) {
                /* The signed template sums to zero. Invert raw RSSI. */
                dot -= raw[i+j+1]*shape[p*pre_samples+j];
            }
            if (dot <= 0 || shape_norm[p] == 0) continue;
            int divisor = (deviation*shape_norm[p]+pre_samples/2)/pre_samples;
            if (divisor == 0) continue;
            int r = (1000*dot)/divisor;
            if (r > best) best = r;
        }
        /* Squared normalized correlation, mildly penalized at low SNR. */
        int score = (best*best/100)*rise16/(rise16+48);
        return score;
    }
    return 0;
}

static size_t find_preambles(const uint8_t *raw, size_t n, const int8_t *shape,
                    candidate_t *out, size_t limit, int min_rise, int min_score,
                    int pre_samples, int body_start, int body_samples, int post_start) {
    if (n < (size_t)(post_start + 2*QUIET_SAMPLES+4) || limit == 0) return 0;
    int before = 0, after = 0, body = 0;
    size_t used = 0;
    int shape_norm[PHASE_COUNT];
    prepare_norm(shape,pre_samples,shape_norm);
    size_t first = QUIET_SAMPLES+3;
    for (int j = 0; j < QUIET_SAMPLES; j++) {
        before += raw[j];
        after += raw[first+post_start+j];
    }
    for (int j = 0; j < body_samples; j++) body += raw[first+body_start+j];
    for (size_t i = first; i+post_start+QUIET_SAMPLES < n; i++) {
        /* All three windows slide once per sample. Keep their averaged values
         * in half-dB codes times 16, with lower codes meaning higher power. */
        int quieter = before < after ? before : after;
        /* Exact rejection before division: floor(16*body/body_samples)
         * fits the allowed maximum iff 16*body < (maximum+1)*body_samples.
         * Most samples are quiet, including during a real aircraft pass. */
        if (body*16 < (quieter-min_rise*16+1)*body_samples) {
            int rise16 = quieter - (body_samples==128 ? body>>3 : (body*16)/body_samples);
            int score=candidate_score(raw,(int)i,shape,pre_samples,shape_norm,rise16);
            if (score>=min_score) keep(out,&used,limit,(int)i,score);
        }
        before += (int)raw[i-3] - raw[i-first];
        after += (int)raw[i+post_start+QUIET_SAMPLES] - raw[i+post_start];
        body += (int)raw[i+body_start+body_samples] - raw[i+body_start];
    }
    return used;
}

/* The original 128-byte model retains its ABI. Other rates carry the four
 * window lengths followed by eight zero-mean, signed, phase templates. */
int sx_shape_valid(const uint8_t *shape, size_t size) {
    if (size == PHASE_COUNT*PRE_SAMPLES) return 1;
    if (size < 4 || shape[0] < 10 || shape[0] > 28) return 0;
    return size == 4u+PHASE_COUNT*shape[0] && shape[1] >= shape[0] &&
           shape[2] >= 60 && shape[3] >= (unsigned)shape[1]+shape[2];
}

size_t sx_preambles_model(const uint8_t *raw, size_t n, const uint8_t *shape, size_t shape_size,
                    candidate_t *out, size_t limit, int min_rise, int min_score) {
    if (!sx_shape_valid(shape,shape_size)) return 0;
    if (shape_size == PHASE_COUNT*PRE_SAMPLES)
        return find_preambles(raw,n,(const int8_t*)shape,out,limit,min_rise,min_score,
                              PRE_SAMPLES,BODY_START,BODY_SAMPLES,POST_START);
    return find_preambles(raw,n,(const int8_t*)(shape+4),out,limit,min_rise,min_score,
                          shape[0],shape[1],shape[2],shape[3]);
}
size_t sx_preambles(const uint8_t *raw, size_t n, const int8_t *shape,
                    candidate_t *out, size_t limit, int min_rise, int min_score) {
    return find_preambles(raw,n,shape,out,limit,min_rise,min_score,
                          PRE_SAMPLES,BODY_START,BODY_SAMPLES,POST_START);
}

/* The SPI page reader can find all positions that pass the energy test while
 * the peripheral shifts in the following page. Finishing the template scores
 * afterward gives exactly the same positions and scores as the full scan. */
typedef struct {
    candidate_t *positions;
    int capacity,used,at,before,after,body,body_start,body_samples,post,rise;
} sx_gate;

static void setup_gate(sx_gate *g,candidate_t *positions,int capacity,
                       const uint8_t *shape,size_t shape_len,int rise) {
    g->positions=positions;g->capacity=capacity;g->used=0;g->at=0;g->rise=rise;
    g->body_start=shape_len==PHASE_COUNT*PRE_SAMPLES?BODY_START:shape[1];
    g->body_samples=shape_len==PHASE_COUNT*PRE_SAMPLES?BODY_SAMPLES:shape[2];
    g->post=shape_len==PHASE_COUNT*PRE_SAMPLES?POST_START:shape[3];
}

static void advance_gate(sx_gate *g,const uint8_t *raw,int available) {
    int stop=available-g->post-QUIET_SAMPLES;
    if(stop<=QUIET_SAMPLES+3 || g->used<0)return;
    if(g->at==0){
        g->at=QUIET_SAMPLES+3;g->before=0;g->after=0;g->body=0;
        for(int j=0;j<QUIET_SAMPLES;j++){
            g->before+=raw[j];g->after+=raw[g->at+g->post+j];
        }
        for(int j=0;j<g->body_samples;j++)g->body+=raw[g->at+g->body_start+j];
    }
    int before=g->before,after=g->after,body=g->body;
    int bs=g->body_start,bl=g->body_samples,post=g->post,limit=g->rise*16;
    for(int i=g->at;i<stop;i++){
        int quieter=before<after?before:after;
        if(body*16<(quieter-limit+1)*bl){
            if(g->used>=g->capacity){g->used=-1;return;}
            candidate_t *p=g->positions+g->used++;
            p->sample=i;p->score=quieter-(body*16)/bl;
        }
        before+=(int)raw[i-3]-raw[i-QUIET_SAMPLES-3];
        after+=(int)raw[i+post+QUIET_SAMPLES]-raw[i+post];
        body+=(int)raw[i+bs+bl]-raw[i+bs];
    }
    g->before=before;g->after=after;g->body=body;g->at=stop;
}

/* Host replay also uses this function to validate the incremental scanner. */
int sx_energy(const uint8_t *raw,int n,const uint8_t *shape,size_t shape_len,
              candidate_t *positions,int capacity,int rise) {
    if(!sx_shape_valid(shape,shape_len)||n<0||capacity<0||rise<1)return -2;
    sx_gate g;setup_gate(&g,positions,capacity,shape,shape_len,rise);
    for(int i=64;i<n;i+=64)advance_gate(&g,raw,i);
    advance_gate(&g,raw,n);
    return g.used;
}

int sx_from_energy(const uint8_t *raw,int n,const uint8_t *bytes,size_t slen,
                   const candidate_t *positions,int count,candidate_t *out,int cap,int score_min) {
    if(!sx_shape_valid(bytes,slen)||n<0||count<0||cap<1)return -1;
    int legacy=slen==PHASE_COUNT*PRE_SAMPLES;
    int pre=legacy?PRE_SAMPLES:bytes[0],last=legacy?POST_START:bytes[3];
    const int8_t *shape=(const int8_t *)(legacy?bytes:bytes+4);
    int norm[PHASE_COUNT];prepare_norm(shape,pre,norm);
    size_t used=0;
    for(int k=0;k<count;k++){
        int i=positions[k].sample,rise=positions[k].score;
        if(i<QUIET_SAMPLES+3||i+last+QUIET_SAMPLES>=n||rise<=0||rise>4080)return -1;
        int score=candidate_score(raw,i,shape,pre,norm,rise);
        if(score>=score_min)keep(out,&used,cap,i,score);
    }
    return (int)used;
}
