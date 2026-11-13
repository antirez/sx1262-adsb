/*
 * Receive-only decision-directed adjustment to the measured pulse response.
 * Fit 21 small corrections to the best tentative RF word. Leave one sample
 * in seven out of the fit and always read parity from the same RF waveform.
 * The ranked paths never feed back into the response estimate.
 */
#ifndef SX_BITS
#include "sequence.c"
#endif

#define AD_DIM 21
#define AD_SEEDS 3
#define AD_ITERATIONS 3

typedef struct {
    sx_trial timing;
    int quality;
    uint8_t word[14];
} adaptive_seed;

typedef struct {
    float normal[AD_DIM][AD_DIM], coeff[AD_DIM];
    float correction[SX_PHASES][128], weight[SX_PHASES][5];
    float observed[SX_MAX_FETCHES];
    adaptive_seed seed[AD_SEEDS];
} adaptive_scratch;

size_t sx_adaptive_size(void) { return sizeof(adaptive_scratch); }

/* Every denominator is a positive normal float. The Xtensa native-module ABI
 * has multiply/add but no libgcc float divide. Four Newton steps from the
 * reciprocal exponent approximation reach single-precision accuracy. */
static float adaptive_reciprocal(float x) {
    union { float f; uint32_t bits; } value={.f=x};
    value.bits=0x7ef311c3u-value.bits;
    float y=value.f;
    for(int i=0;i<4;i++) y*=2.0f-x*y;
    return y;
}

static int adaptive_state(const uint8_t *word,int n) {
    int state=0;
    for(int i=n-3;i<=n+3;i++) {
        state<<=1;
        if(i>=0 && i<112) state|=frame_bit(word,i);
    }
    return state;
}

static int adaptive_edge(int n) {return n<3?n:(n>=109?n-105:3);}

static float adaptive_sign(int state,int term) {
    return (state & (1<<(5-term))) ? 1.0f : -1.0f;
}

static void adaptive_phase(int phase,float values[3]) {
    float x=((float)phase-9.5f)*.1f;
    values[0]=1.0f;
    values[1]=x;
    values[2]=1.5f*x*x-.5f;
}

static void adaptive_features(int state,int phase,int n,float nominal,float *z) {
    float p[3];
    adaptive_phase(phase,p);
    for(int i=0;i<3;i++) {
        for(int j=0;j<5;j++) z[5*i+j]=p[i]*adaptive_sign(state,j);
        z[15+i]=p[i];
    }
    z[18]=(nominal-64.0f)*.05f;
    z[20]=((float)n-55.5f)*.018018018018f;
    z[19]=z[18]*z[20];
}

static void adaptive_solve(adaptive_scratch *a) {
    /* Positive-definite system with the nominal response as a ridge prior. */
    for(int k=0;k<AD_DIM;k++) {
        float inv=adaptive_reciprocal(a->normal[k][k]);
        for(int i=k+1;i<AD_DIM;i++) {
            float scale=a->normal[i][k]*inv;
            for(int j=k+1;j<AD_DIM;j++) a->normal[i][j]-=scale*a->normal[k][j];
            a->coeff[i]-=scale*a->coeff[k];
        }
    }
    for(int i=AD_DIM-1;i>=0;i--) {
        for(int j=i+1;j<AD_DIM;j++) a->coeff[i]-=a->normal[i][j]*a->coeff[j];
        a->coeff[i]*=adaptive_reciprocal(a->normal[i][i]);
    }
}

static void adaptive_fit(const sx_model *m,const int16_t *ticks,int start,int end,
                         const uint8_t *word,adaptive_scratch *a) {
    memset(a->normal,0,sizeof(a->normal));
    memset(a->coeff,0,sizeof(a->coeff));
    for(int i=0;i<AD_DIM;i++) a->normal[i][i]=16.0f;
    for(int k=start;k<end;k++) {
        if((k&15)==0) SX_SERVICE_RX();
        int n=ticks[k]/SX_PHASES-8;
        if(n<3 || n>=109 || (k-start)%7==3) continue;
        int phase=ticks[k]%SX_PHASES,state=adaptive_state(word,n);
        float nominal=m->expected[phase][3][state];
        float err=a->observed[k]-nominal, magnitude=err<0?-err:err;
        float weight=magnitude>20.0f?20.0f*adaptive_reciprocal(magnitude):1.0f;
        float z[AD_DIM];
        adaptive_features(state,phase,n,nominal,z);
        for(int i=0;i<AD_DIM;i++) {
            float weighted=weight*z[i];
            a->coeff[i]+=weighted*err;
            for(int j=0;j<=i;j++) a->normal[i][j]+=weighted*z[j];
        }
    }
    for(int i=0;i<AD_DIM;i++) for(int j=0;j<i;j++) a->normal[j][i]=a->normal[i][j];
    adaptive_solve(a);
    for(int ph=0;ph<SX_PHASES;ph++) {
        float p[3];
        adaptive_phase(ph,p);
        float offset=a->coeff[15]+p[1]*a->coeff[16]+p[2]*a->coeff[17];
        for(int j=0;j<5;j++)
            a->weight[ph][j]=a->coeff[j]+p[1]*a->coeff[5+j]+p[2]*a->coeff[10+j];
        for(int s=0;s<128;s++) {
            float v=offset;
            for(int j=0;j<5;j++) v+=a->weight[ph][j]*adaptive_sign(s,j);
            a->correction[ph][s]=v;
        }
    }
}

static void adaptive_metric(const sx_model *m,const adaptive_scratch *a,
                            int k,int phase,int n,float loss[128]) {
    float tm=((float)n-55.5f)*.018018018018f;
    float gain=(a->coeff[18]+tm*a->coeff[19])*.05f;
    float offset=tm*a->coeff[20]-64.0f*gain;
    const int16_t *table=m->expected[phase][adaptive_edge(n)];
    for(int s=0;s<128;s++) {
        float correction=a->correction[phase][s];
        if(n<2 || n>=110) {
            for(int j=0;j<5;j++) if(n-2+j<0 || n-2+j>=112)
                correction-=a->weight[phase][j]*adaptive_sign(s,j);
        }
        float err=a->observed[k]-(table[s]*(1.0f+gain)+offset+correction);
        float absolute=err<0.0f?-err:err;
        loss[s]+=absolute<=24.0f?err*err:24.0f*(2.0f*absolute-24.0f);
    }
}

static int adaptive_path(const sx_model *m,const int16_t *ticks,int start,int end,
                         adaptive_scratch *a,sx_rank_workspace *w,uint8_t *best,uint8_t *candidate) {
    sx_workspace *base=&w->list.base;
    for(int s=0;s<64;s++) base->costs[0][s]=0;
    int k=start;
    for(int n=0;n<112;n++) {
        if ((n&3)==0) SX_SERVICE_RX();
        float loss[128]={0};
        while(k<end && ticks[k]<(n+9)*SX_PHASES) {
            adaptive_metric(m,a,k,ticks[k]%SX_PHASES,n,loss);
            k++;
        }
        float low=loss[0];
        for(int s=1;s<128;s++) if(loss[s]<low) low=loss[s];
        for(int s=0;s<64;s++) {
            int x=base->costs[n&1][s>>1]+(int)(loss[s]-low+.5f);
            int y=base->costs[n&1][32+(s>>1)]+(int)(loss[s+64]-low+.5f);
            if(n<base->known_n) {
                int mask=base->known_mask[n],value=base->known_value[n];
                if((s&mask)!=value)x=SX_INF;
                if(((s+64)&mask)!=value)y=SX_INF;
            }
            w->list.delta[n][s]=x>y?x-y:y-x;
            base->decisions[n][s]=y<x;
            base->costs[(n+1)&1][s]=y<x?y:x;
        }
    }
    int state=0;
    for(int s=1;s<64;s++) if(base->costs[0][s]<base->costs[0][state]) state=s;
    traceback(base,state,-1,best,112);
    return ranked_timing(w,candidate);
}

static int adaptive_seeds(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                          sx_rank_workspace *w,adaptive_scratch *a) {
    sx_workspace *base=&w->list.base;
    int count=decode_window_header(raw,n,at,m,len,base,sizeof(*base),NULL,NULL,112,1,NULL,17,0,0);
    if(count<=0) return count;
    int used=0;
    for(int i=0;i<count;i++) {
        adaptive_seed seed;
        seed.timing=base->chosen[i];
        decode_one(raw,m,base,&seed.timing,seed.word,NULL,112);
        seed.quality=trial_cost(base,seed.timing);
        int pos=0;
        while(pos<used && a->seed[pos].quality<=seed.quality) pos++;
        if(pos>=AD_SEEDS) continue;
        if(used<AD_SEEDS) used++;
        for(int j=used-1;j>pos;j--) a->seed[j]=a->seed[j-1];
        a->seed[pos]=seed;
    }
    return used;
}

int sx_decode_adaptive(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                       void *memory,size_t size,void *scratch,size_t extra,uint8_t *frame) {
    if(size<sizeof(sx_rank_workspace) || extra<sizeof(adaptive_scratch) ||
       ((uintptr_t)memory&3u) || ((uintptr_t)scratch&3u)) return -1;
    if(at<12 || (size_t)at+173>n) return 0;
    int before=0,after=0,body=0;
    for(int i=0;i<8;i++) {before+=raw[at-12+i];after+=raw[at+165+i];}
    for(int i=16;i<144;i++) body+=raw[at+i];
    if(16*(before<after?before:after)-body<14*128) return 0;
    sx_rank_workspace *w=memory;
    adaptive_scratch *a=scratch;
    int count=adaptive_seeds(raw,n,at,m,len,w,a),answer=0;
    if(count<=0) return count;
    for(int j=0;j<count;j++) {
        const adaptive_seed *seed=&a->seed[j];
        const sx_trial *q=&seed->timing;
        const int16_t *ticks=m->ticks+q->model*m->fetches;
        int start=0,end=0;
        while(start<m->fetches && ticks[start]<8*SX_PHASES) start++;
        while(end<m->fetches && ticks[end]<120*SX_PHASES) end++;
        float scale=64.0f*adaptive_reciprocal((float)q->gain);
        for(int k=start;k<end;k++) {
            int gain=ratio_at(m,q->floor,raw[q->at+k])-256;
            a->observed[k]=scale*(float)gain;
        }
        uint8_t history[AD_ITERATIONS][14];
        memcpy(history[0],seed->word,14);
        for(int rep=0;rep<AD_ITERATIONS;rep++) {
            uint8_t best[14],candidate[14];
            adaptive_fit(m,ticks,start,end,history[rep],a);
            int rank=adaptive_path(m,ticks,start,end,a,w,best,candidate);
            if(rank) {
                if(answer && frame_distance(candidate,frame)) return 0;
                memcpy(frame,candidate,14);
                answer=rank;
                break;
            }
            int repeated=0;
            for(int h=0;h<=rep;h++) if(!frame_distance(best,history[h])) repeated=1;
            if(repeated || rep+1==AD_ITERATIONS) break;
            memcpy(history[rep+1],best,14);
        }
    }
    return answer;
}
