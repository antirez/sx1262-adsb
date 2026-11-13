/* Receiver-only DF17 recovery. Both calibrated fits select their timing and
 * message family without seeing the parity syndrome or an aircraft address.
 * Reliable decisions are fixed. The primary parity estimate can differ in
 * at most one of its two least reliable positions. */
#include "me_patterns.h"

typedef struct {
    uint8_t word[14], weak[112];
    int family;
} sx_soft_word;

typedef struct {
    sx_list_workspace list;
    sx_soft_word response[2];
    int32_t confidence[112];
} sx_family_workspace;

typedef char sx_family_fits_existing_work[(sizeof(sx_family_workspace)<=sizeof(sx_rank_workspace))?1:-1];

static int frame_bit(const uint8_t *word, int bit) {
    return (word[bit/8] >> (7-bit%8)) & 1;
}

static void flip_frame_bit(uint8_t *word, int bit) {
    word[bit/8] ^= (uint8_t)(128u >> (bit%8));
}

static void constrain_me(sx_workspace *w, const uint8_t *pattern) {
    /* Bits 0..4 retain DF17. Bits 5..31 (CA and ICAO) stay free. */
    w->known_n=91;
    for (int n=8;n<91;n++) {
        int bits=0, values=0;
        for (int k=0;k<7;k++) {
            int bit=n-3+k-32;
            if (bit>=0 && bit<56 && frame_bit(pattern,bit)) {
                bits|=1<<(6-k);
                values|=frame_bit(pattern+7,bit)<<(6-k);
            }
        }
        w->known_mask[n]=(uint8_t)bits;
        w->known_value[n]=(uint8_t)values;
    }
}

static int trial_cost(sx_workspace *w, sx_trial trial) {
    int best=w->costs[0][0];
    for(int s=1;s<64;s++) if(w->costs[0][s]<best) best=w->costs[0][s];
    return 7*(int)trial.score+3*best;
}

static void bit_confidence(sx_family_workspace *w) {
    /* Backward distances use the excess costs saved by the forward trellis. */
    int32_t back[64], earlier[64];
    int best=w->list.base.costs[0][0];
    for(int s=1;s<64;s++) if(w->list.base.costs[0][s]<best) best=w->list.base.costs[0][s];
    for(int s=0;s<64;s++) back[s]=w->list.base.costs[0][s]-best;
    for(int n=111;n>=2;n--) {
        int cost[2]={SX_INF,SX_INF};
        for(int s=0;s<64;s++) {
            earlier[s]=SX_INF;
            if(back[s]<cost[s&1]) cost[s&1]=back[s];
        }
        if(n+3<112) w->confidence[n+3]=cost[0]>cost[1]?cost[0]-cost[1]:cost[1]-cost[0];
        for(int s=0;s<64;s++) {
            if(back[s]>=SX_INF) continue;
            int winner=w->list.base.decisions[n][s];
            for(int b=0;b<2;b++) {
                int pred=(s>>1)+32*b;
                int value=back[s]+(b==winner?0:w->list.delta[n][s]);
                if(value<earlier[pred]) earlier[pred]=value;
            }
        }
        memcpy(back,earlier,sizeof(back));
    }
}

static void mark_weak(sx_soft_word *response, const int32_t *confidence,
                      int start, int stop, int count) {
    const uint8_t *mask=me_patterns[response->family];
    for(int j=0;j<count;j++) {
        int best=-1;
        for(int bit=start;bit<stop;bit++) {
            if(response->weak[bit] || (bit>=32 && bit<88 && frame_bit(mask,bit-32))) continue;
            if(best<0 || confidence[bit]<confidence[best]) best=bit;
        }
        if(best<0) break;
        response->weak[best]=1;
    }
}

static int fit_response(const uint8_t *raw,size_t n,int anchor,const sx_model *m,size_t len,
                        sx_family_workspace *w,int index) {
    sx_workspace *base=&w->list.base;
    sx_soft_word *answer=&w->response[index];
    int count=decode_window_mode(raw,n,anchor,m,len,base,sizeof(*base),NULL,NULL,112,1);
    if(count<=0) return count;
    int best_cost=0,chosen=0;
    for(int t=0;t<count;t++) {
        decode_one(raw,m,base,&base->chosen[t],answer->word,NULL,112);
        int cost=trial_cost(base,base->chosen[t]);
        if(t==0 || cost<best_cost) {best_cost=cost;chosen=t;}
    }
    sx_trial timing=base->chosen[chosen];
    int winner=0,runner=0x7fffffff,lowest=0x7fffffff;
    for(int i=0;i<(int)(sizeof(me_patterns)/sizeof(me_patterns[0]));i++) {
        constrain_me(base,me_patterns[i]);
        decode_one(raw,m,base,&timing,answer->word,NULL,112);
        int cost=trial_cost(base,timing);
        if(cost<lowest) {runner=lowest;lowest=cost;winner=i;}
        else if(cost<runner) runner=cost;
    }
    if(lowest>best_cost+best_cost/5 || runner<lowest+lowest/20) return 0;
    constrain_me(base,me_patterns[winner]);
    w->list.count=0;
    decode_one(raw,m,base,&timing,answer->word,&w->list,112);
    answer->family=winner;
    memset(answer->weak,0,sizeof(answer->weak));
    bit_confidence(w);
    mark_weak(answer,w->confidence,5,88,index==0?16:24);
    mark_weak(answer,w->confidence,88,112,index==0?2:4);
    return 1;
}

static int frame_distance(const uint8_t *a,const uint8_t *b) {
    int distance=0;
    for(int i=0;i<14;i++) {
        unsigned different=a[i]^b[i];
        while(different) {different&=different-1;distance++;}
    }
    return distance;
}

static int combine_responses(const sx_soft_word *a,const sx_soft_word *b,uint8_t *frame) {
    if(a->family!=b->family) return 0;
    uint8_t fixed[14],unknown[16],count=0;
    memcpy(fixed,a->word,14);
    uint32_t columns[112],v=0xfff409;
    for(int bit=111;bit>=0;bit--) {
        columns[bit]=v;
        v=((v<<1)^((v&0x800000)?0xfff409:0))&0xffffff;
    }
    uint32_t basis[24]={0};
    uint16_t representation[24]={0};
    for(int bit=5;bit<88;bit++) {
        if(a->weak[bit] && b->weak[bit]) {
            if(count>=16) return 0;
            unknown[count]=bit;
            uint16_t choices=(uint16_t)(1u<<count++);
            uint32_t s=columns[bit];
            for(int k=23;k>=0;k--) if(s&(1u<<k)) {
                if(!basis[k]) {basis[k]=s;representation[k]=choices;break;}
                s^=basis[k];choices^=representation[k];
            }
            if(!s) return 0; /* The RF erasures must have a unique solution. */
        } else if(frame_bit(a->word,bit)!=frame_bit(b->word,bit)) {
            if(a->weak[bit]) flip_frame_bit(fixed,bit);
            else if(!b->weak[bit]) return 0;
        }
    }
    int result=0;
    for(int parity=87;parity<112;parity++) {
        if(parity>=88 && !a->weak[parity]) continue;
        uint8_t candidate[14];memcpy(candidate,fixed,14);
        if(parity>=88) flip_frame_bit(candidate,parity);
        int ok=1;
        for(int bit=88;bit<112;bit++)
            if(!b->weak[bit] && frame_bit(candidate,bit)!=frame_bit(b->word,bit)) ok=0;
        if(!ok) continue;
        uint32_t s=sx_crc(candidate,14);uint16_t choices=0;
        for(int k=23;k>=0;k--) if(s&(1u<<k)) {
            if(!basis[k]) break;
            s^=basis[k];choices^=representation[k];
        }
        if(s) continue;
        for(int k=0;k<count;k++) if(choices&(1u<<k)) flip_frame_bit(candidate,unknown[k]);
        int distance=frame_distance(candidate,a->word);
        if(distance>8 || frame_distance(candidate,b->word)>10 || !check_frame(candidate,112)) continue;
        if(result) return 0;
        memcpy(frame,candidate,14);result=1+distance;
    }
    return result;
}

/* Produces a candidate for independent short-address confirmation in the app. */
int sx_decode_family(const uint8_t *raw,size_t n,int anchor,const void *primary,size_t primary_len,
                     const void *second,size_t second_len,void *work,size_t bytes,uint8_t *frame) {
    if(bytes<sizeof(sx_family_workspace) || ((uintptr_t)work&3u)) return -1;
    if(anchor<12 || (size_t)anchor+173>n) return 0;
    int before=0,after=0,body=0;
    for(int j=0;j<8;j++) {before+=raw[anchor-12+j];after+=raw[anchor+165+j];}
    for(int j=16;j<144;j++) body+=raw[anchor+j];
    if(16*(before<after?before:after)-body<12*128) return 0; /* 6 dB sustained rise */
    sx_family_workspace *w=work;
    int status=fit_response(raw,n,anchor,primary,primary_len,w,0);
    if(status<=0) return status;
    status=fit_response(raw,n,anchor,second,second_len,w,1);
    if(status<=0) return status;
    return combine_responses(&w->response[0],&w->response[1],frame);
}

/* Select timing and calculate bit reliability without a header, an aircraft
 * address or a parity prior. Used by both the field header tracker and the
 * held-out waveform diagnostics. */
static int sx_unconstrained_fit(const uint8_t *raw,size_t n,int anchor,const sx_model *model,
                                size_t model_len,int df,sx_family_workspace *work,uint8_t *word) {
    sx_workspace *base = &work->list.base;
    int count = decode_window_header(raw, n, anchor, model, model_len, base,
                                     sizeof(*base), NULL, NULL, 112, 1, NULL, df, 0, 0);
    if (count <= 0) return count;
    int best = 0, best_cost = 0;
    for (int i = 0; i < count; i++) {
        decode_one(raw, model, base, &base->chosen[i], word, NULL, 112);
        int cost = trial_cost(base, base->chosen[i]);
        if (i == 0 || cost < best_cost) {
            best = i;
            best_cost = cost;
        }
    }
    work->list.count = 0;
    decode_one(raw, model, base, &base->chosen[best], word, &work->list, 112);
    bit_confidence(work);
    return best + 1;
}
