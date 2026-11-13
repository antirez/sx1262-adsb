/* Embedded DF17 decoding of timed unmodified-SX1262 power samples.
 * Rank time hypotheses against the 13 us known preamble/DF prefix, decode
 * 112 bits with a 64-state trellis, then check the unmodified Mode S CRC.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef SX_SERVICE_RX
#define SX_SERVICE_RX() ((void)0)
#endif

#define SX_BITS 112
#define SX_PHASES 20
#define SX_TEMPLATES 72
#define SX_LEGACY_FETCHES 155
#define SX_MAX_FETCHES 216
#define SX_TOP 24
#define SX_INF 500000000

typedef struct {
    uint16_t score;
    uint8_t at, model, floor;
    int gain;
} sx_trial;

typedef struct {
    int32_t costs[2][64];
    uint8_t decisions[SX_BITS][64];
    uint8_t df, known_n, residual_pair, known_mask[91], known_value[91];
    int16_t obs[SX_MAX_FETCHES];
    sx_trial chosen[SX_TOP];
} sx_workspace;

typedef struct {
    char magic[8];
    uint16_t templates,fetches,phases,prelen;
    uint16_t ratio[256];
    int16_t expected[SX_PHASES][7][128];
    int16_t pre[13*SX_PHASES];
    int16_t ticks[];
} sx_model;

size_t sx_workspace_size(void) { return sizeof(sx_workspace); }
size_t sx_model_size(void) { return sizeof(sx_model)+2*SX_TEMPLATES*SX_LEGACY_FETCHES; }

static int ratio_at(const sx_model *m, int baseline, int code) {
    int k=128+baseline-code;
    if (k<0) k=0;
    if (k>255) k=255;
    return m->ratio[k];
}

static int quiet(const uint8_t *p) {
    uint8_t v[13];
    for (int n=0;n<13;n++) {
        int j=n;
        while (j && v[j-1]>p[n]) {v[j]=v[j-1];j--;}
        v[j]=p[n];
    }
    return v[6];
}

static void insert(sx_trial *q,int *len,sx_trial trial) {
    if (*len==SX_TOP && q[*len-1].score<=trial.score) return;
    int k=*len<SX_TOP ? (*len)++ : SX_TOP-1;
    while (k && q[k-1].score>trial.score) {q[k]=q[k-1];k--;}
    q[k]=trial;
}

static uint32_t sx_crc(const uint8_t *frame,int length) {
    uint32_t s=0;
    for (int j=0;j<length;j++) {
        s^=(uint32_t)frame[j]<<16;
        for (int k=0;k<8;k++) s=((s<<1)^((s&0x800000)?0xfff409:0))&0xffffff;
    }
    return s;
}

static int check_frame(const uint8_t *frame,int bits) {
    if (bits==112) return (frame[0]>>3)==17 && sx_crc(frame,14)==0;
    /* DF11 replies carry an interrogator identifier in the low parity bits.
     * The host records the actual overlay and confirms an isolated hit by RF
     * comparison or repetition before attributing it to an aircraft. */
    uint32_t parity=((uint32_t)frame[4]<<16)|((uint32_t)frame[5]<<8)|frame[6];
    return (frame[0]>>3)==11 && (sx_crc(frame,4)^parity)<80;
}

static int check_selected_format(const uint8_t *frame,int bits,int df) {
    if(df==2)
        return bits==112 && (frame[0]>>3)==2 && frame[1]=='T' && frame[2]=='E' &&
               frame[3]=='S' && frame[4]=='T' && sx_crc(frame,14)==0;
    return check_frame(frame,bits);
}


#define SX_ALTERNATIVES 8
typedef struct {
    int32_t cost;
    uint8_t frame[14];
} sx_alternative;
typedef struct {
    sx_workspace base;
    int32_t delta[SX_BITS][64];
    sx_alternative selected[SX_ALTERNATIVES];
    int count;
} sx_list_workspace;

size_t sx_list_workspace_size(void) { return sizeof(sx_list_workspace); }

static void traceback(const sx_workspace *w,int state,int change,uint8_t *frame,int bits) {
    for(int k=0;k<14;k++)frame[k]=0;
    for(int n=bits-1;n>=0;n--){
        int bit=n+3;
        if(bit<bits) frame[bit>>3]|=(state&1)<<(7-(bit&7));
        state=(state>>1)+32*(w->decisions[n][state]^(n==change));
    }
    frame[0]|=(state&7)<<5;
}

/* Retain at most eight distinct paths across every frame timing. No CRC
 * enters selection or ranking. A deviation follows the second predecessor
 * at one trellis state and then its maximum-likelihood predecessor chain. */
static void retain(sx_list_workspace *w,int final,int change,int cost) {
    if(cost>=SX_INF || (w->count==SX_ALTERNATIVES && cost>=w->selected[w->count-1].cost)) return;
    uint8_t frame[14];
    traceback(&w->base,final,change,frame,112);
    int at=0;
    while(at<w->count){
        int same=1;
        for(int k=0;k<14;k++)if(frame[k]!=w->selected[at].frame[k]){same=0;break;}
        if(same)break;
        at++;
    }
    if(at<w->count && w->selected[at].cost<=cost)return;
    if(at==w->count){
        if(w->count<SX_ALTERNATIVES)w->count++;
        else at--;
    }
    while(at>0 && w->selected[at-1].cost>cost){
        w->selected[at].cost=w->selected[at-1].cost;
        for(int k=0;k<14;k++)w->selected[at].frame[k]=w->selected[at-1].frame[k];
        at--;
    }
    w->selected[at].cost=cost;
    for(int k=0;k<14;k++)w->selected[at].frame[k]=frame[k];
}

static void alternate_paths(sx_list_workspace *w,int final) {
    int state=final,best=w->base.costs[0][final];
    for(int n=111;n>=0;n--){
        int delta=w->delta[n][state];
        retain(w,final,n,best+delta);
        state=(state>>1)+32*w->base.decisions[n][state];
    }
    for(int k=0;k<64;k++)
        if(k!=final)retain(w,k,-1,w->base.costs[0][k]);
}

/* The ESP32-S3 field decoder spends most of its time in the trellis.
 * Keep the bus and register timing loops outside this optimization scope. */
#if defined(__XTENSA__) && defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("O3")
#endif

/* Alternate likelihood for the uniformly spaced power readings. For
 * consecutive samples include one quarter of the previous prediction error.
 * Across a bit boundary the previous seven-bit context has one unknown
 * oldest bit. Average its two predictions before computing the residual. */
static void paired_residual_costs(const sx_model *model, const sx_workspace *work,
                                  const int16_t *ticks, int bit, int bits, int edge,
                                  int begin, int end, int32_t *cost) {
    const int start = (8+bit)*SX_PHASES;
    const int prior_edge = bit-1 < 3 ? bit-1 : bit-1 < bits-3 ? 3 : bit-1-bits+7;
    memset(cost, 0, 128*sizeof(*cost));
    for (int k=begin; k<end; k++) {
        const int16_t *current = model->expected[ticks[k]-start][edge];
        int before = ticks[k-1]-start;
        int same = before >= 0;
        const int16_t *prior = same ? model->expected[before][edge]
                                   : model->expected[before+SX_PHASES][prior_edge];
        for (int state=0; state<128; state++) {
            int prev = same ? prior[state] : (prior[state>>1]+prior[(state>>1)|64])/2;
            int delta = work->obs[k]-current[state] + (work->obs[k-1]-prev)/4;
            cost[state] += delta*delta;
        }
    }
}

static int decode_one(const uint8_t *raw,const sx_model *m,sx_workspace *w,
                      const sx_trial *q,uint8_t *frame,sx_list_workspace *alternatives,int bits) {
    const int16_t *ticks=m->ticks+q->model*m->fetches;
    int j=0;
    for (int k=0;k<m->fetches;k++) {
        int x=64*(ratio_at(m,q->floor,raw[q->at+k])-256)/q->gain;
        if (x>900) x=900;
        if (x < -220) x=-220;
        w->obs[k]=(int16_t)x;
    }
    for (int s=0;s<64;s++) w->costs[0][s]=0;
    while (j<m->fetches && ticks[j]<8*SX_PHASES) j++;
    for (int n=0;n<bits;n++) {
        if ((n&15)==0) SX_SERVICE_RX();
        int edge=n<3?n:n<bits-3?3:n-bits+7;
        int start=(8+n)*SX_PHASES;
        int begin=j;
        while (j<m->fetches && ticks[j]<start+SX_PHASES) j++;
        const int32_t *old=w->costs[n&1];
        int32_t *next=w->costs[(n+1)&1];
        int32_t branch[128];
        if (w->residual_pair && n>=3 && begin<j && begin>0 &&
            ticks[begin-1]>=start-SX_PHASES && ticks[begin-1]>=8*SX_PHASES) {
            paired_residual_costs(m,w,ticks,n,bits,edge,begin,j,branch);
        } else if(begin==j) {
            memset(branch,0,sizeof(branch));
        } else {
            const int16_t *expected=m->expected[ticks[begin]-start][edge];
            for(int s=0;s<128;s++) {
                int error=w->obs[begin]-expected[s];
                branch[s]=error*error;
            }
            for(int k=begin+1;k<j;k++) {
                expected=m->expected[ticks[k]-start][edge];
                for(int s=0;s<128;s++) {
                    int error=w->obs[k]-expected[s];
                    branch[s]+=error*error;
                }
            }
        }
        for (int state=0;state<64;state++) {
            int a=old[state>>1]+branch[state],b=old[32+(state>>1)]+branch[state+64];
            if(n<w->known_n){
                int mask=w->known_mask[n],value=w->known_value[n];
                if((state&mask)!=value)a=SX_INF;
                if(((state+64)&mask)!=value)b=SX_INF;
            }
            if(alternatives)alternatives->delta[n][state]=a>b?a-b:b-a;
            w->decisions[n][state]=(uint8_t)(b<a);
            next[state]=b<a?b:a;
        }
    }
    int state=0;
    for (int s=1;s<64;s++) if(w->costs[0][s]<w->costs[0][state]) state=s;
    traceback(w,state,-1,frame,bits);
    if(check_selected_format(frame,bits,w->df))return 1;
    if(alternatives)alternate_paths(alternatives,state);
    return 0;
}

static int same_ticks(const int16_t *left, const int16_t *right, int n) {
    for(int k=0;k<n;k++) if(left[k]!=right[k]) return 0;
    return 1;
}

static int tried_timing(const sx_model *m, const sx_trial *trials, int index) {
    const sx_trial *q = &trials[index];
    const int16_t *ticks = m->ticks+q->model*m->fetches;
    for(int i=index-1;i>=0 && trials[i].score==q->score;i--) {
        if(trials[i].at==q->at && same_ticks(ticks,m->ticks+trials[i].model*m->fetches,m->fetches)) return 1;
    }
    return 0;
}

/* `raw` includes a complete frame and at least 13 post-frame readings.
 * A positive return value is the one-based rank of the CRC-valid hypothesis. */
static int decode_window_header(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_len,
              void *workspace,size_t work_len,uint8_t *frame,sx_list_workspace *alternatives,int bits,int distinct,
              const uint8_t *header, int df, int status_layout, int residual_pair) {
    const sx_model *m=model;
    sx_workspace *w=workspace;
    if (model_len<sizeof(*m)||work_len<sizeof(*w)||n<120||anchor<0||anchor>240||
        m->magic[0]!='S'||m->magic[1]!='X'||m->magic[6]!='C'||m->magic[7]!='1'||
        m->magic[2]!='D'||m->magic[3]!='F'||m->magic[4]!='1'||m->magic[5]!=(bits==112?'7':'1')||
        m->templates!=SX_TEMPLATES||m->fetches<(bits==112?110:69)||m->fetches>SX_MAX_FETCHES||
        model_len!=sizeof(*m)+2u*SX_TEMPLATES*m->fetches||m->phases!=SX_PHASES||
        m->prelen!=13*SX_PHASES) return -1;
    const int count_known=header?32:5;
    w->df=(uint8_t)df;
    w->residual_pair=(uint8_t)residual_pair;
    w->known_n=(status_layout?88:count_known)+3;
    for(int t=0;t<w->known_n;t++){
        int mask=0,value=0;
        for(int k=0;k<7;k++){
            int pos=t-3+k;
            int bit;
            if(pos>=0 && pos<count_known){
                bit=header?(header[pos/8]>>(7-pos%8))&1:(df>>(4-pos))&1;
            } else if(status_layout && pos>=32 && pos<40) {
                bit=(0xe1>>(39-pos))&1;  /* TC28, subtype 1 */
            } else if(status_layout && pos>=56 && pos<88) {
                bit=0;  /* Optional zero-filled reserved field in the recorded layout. */
            } else continue;
            mask|=1<<(6-k);
            value|=bit<<(6-k);
        }
        w->known_mask[t]=mask;
        w->known_value[t]=value;
    }
    uint32_t signatures[SX_TEMPLATES];
    int count=0;
    for (int ph=0;ph<SX_TEMPLATES;ph++) {
        if ((ph&3)==0) SX_SERVICE_RX();
        const int16_t *ticks=m->ticks+ph*m->fetches;
        int first=0,body=0,stop=0,pre_end=0;
        for (int k=1;k<m->fetches;k++) if(ticks[k]<ticks[k-1]) return -1;
        if(distinct){
            uint32_t hash=2166136261u;
            for(int k=0;k<m->fetches;k++)hash=(hash^(uint16_t)ticks[k])*16777619u;
            signatures[ph]=hash;
            int duplicate=0;
            for(int t=0;t<ph;t++)if(signatures[t]==hash){
                const int16_t *other=m->ticks+t*m->fetches;
                if(same_ticks(ticks,other,m->fetches)){duplicate=1;break;}
            }
            if(duplicate)continue;
        }
        while (first<m->fetches && ticks[first]<0) first++;
        /* The saved prefix includes five DF17 bits. A bench word shares
         * the actual eight-microsecond preamble only. */
        int prefix_ticks=(df==2?8:13)*SX_PHASES;
        while (pre_end<m->fetches && ticks[pre_end]<prefix_ticks) pre_end++;
        while (body<m->fetches && ticks[body]<8*SX_PHASES) body++;
        while (stop<m->fetches && ticks[stop]<(bits+8)*SX_PHASES) stop++;
        if (body>=stop || pre_end<=first) continue;
        int low=anchor>4?anchor-4:0;
        for(int at=low;at<=anchor+4;at++) {
            if((size_t)(at+stop+13)>n || (size_t)(at+m->fetches)>n) continue;
            int floor=quiet(raw+at+stop),sum=0;
            for (int k=body;k<stop;k++) sum+=ratio_at(m,floor,raw[at+k]);
            int gain=sum/(stop-body)-256;
            if(gain<=51) continue;
            int32_t error=0;
            for(int k=first;k<pre_end;k++) {
                int x=64*(ratio_at(m,floor,raw[at+k])-256)/gain-m->pre[ticks[k]];
                if (x>4096) x=4096;
                if (x < -4096) x=-4096;
                error+=x*x;
            }
            /* The maximum summed error fits in signed 32 bits. */
            int score=(int)error/(pre_end-first);
            if(score<65535) {
                sx_trial q={(uint16_t)score,(uint8_t)at,(uint8_t)ph,(uint8_t)floor,gain};
                insert(w->chosen,&count,q);
            }
        }
    }
    if(!frame)return count; /* Timing preparation for the two-response decoder. */
    if(alternatives)alternatives->count=0;
    for(int i=0;i<count;i++) {
        const sx_trial *q=&w->chosen[i];
        /* Preserve the timing rank, all alternatives and the final unchecked output. */
        if(!distinct && i+1<count && tried_timing(m,w->chosen,i)) continue;
        if(decode_one(raw,m,w,q,frame,alternatives,bits))return i+1;
    }
    if(alternatives)for(int i=0;i<alternatives->count;i++){
        if(check_selected_format(alternatives->selected[i].frame,112,df)){
            for(int k=0;k<14;k++)frame[k]=alternatives->selected[i].frame[k];
            return SX_TOP+i+1;
        }
    }
    return 0;
}

static int decode_window_mode(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_len,
              void *workspace,size_t work_len,uint8_t *frame,sx_list_workspace *alternatives,int bits,int distinct) {
    return decode_window_header(raw,n,anchor,model,model_len,workspace,work_len,
                                frame,alternatives,bits,distinct,NULL,bits==112?17:11,0,0);
}

int sx_decode(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_len,
              void *workspace,size_t work_len,uint8_t *frame) {
    return decode_window_mode(raw,n,anchor,model,model_len,workspace,work_len,frame,NULL,112,0);
}
int sx_decode_list(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_len,
                   void *workspace,size_t work_len,uint8_t *frame) {
    if(work_len<sizeof(sx_list_workspace))return -1;
    return decode_window_mode(raw,n,anchor,model,model_len,workspace,work_len,frame,workspace,112,0);
}


int sx_decode11(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_len,
                void *workspace,size_t work_len,uint8_t *frame) {
    return decode_window_mode(raw,n,anchor,model,model_len,workspace,work_len,frame,NULL,56,0);
}

#include "ranked.c"
#include "family.c"

#include "history.c"

#if defined(__XTENSA__) && defined(__GNUC__)
#pragma GCC pop_options
#endif
