#ifndef AD_DIM
#include "adaptive.c"
#endif

#define REPEAT_PAST 24
#define REPEAT_NEAR 6
#define REPEAT_GROUP 5
#define REPEAT_MS 12000u

/* Save only the timed power readings and one signed likelihood per bit.
 * The model tables remain shared by every occurrence. */
typedef struct {
    float weight;
    uint16_t schedule;
    int16_t obs[SX_MAX_FETCHES];
    int8_t soft[112];
    uint32_t block, when;
} repeat_observation;


static int make_observation(const uint8_t *raw,int len,int at,const sx_model *model,int model_len,
                     void *scratch,size_t bytes,repeat_observation *out) {
    if (bytes < sizeof(sx_family_workspace)) return -1;
    sx_family_workspace *fit = scratch;
    uint8_t word[14];
    int rank = sx_unconstrained_fit(raw,len,at,model,model_len,17,fit,word);
    if (rank <= 0) return rank;
    sx_workspace *w = &fit->list.base;
    int best = w->costs[0][0];
    for (int s=1; s<64; s++) if (w->costs[0][s]<best) best=w->costs[0][s];
    out->weight=112.f*adaptive_reciprocal(best+448.f);
    out->schedule=w->chosen[rank-1].model;
    memcpy(out->obs,w->obs,model->fetches*sizeof(int16_t));
    int32_t order[107];
    for (int bit=5;bit<112;bit++) {
        int i=bit-5;
        int v=fit->confidence[bit];
        while(i && order[i-1]>v) {order[i]=order[i-1];i--;}
        order[i]=v;
    }
    float scale=30.f*adaptive_reciprocal(order[53] ? order[53] : 1);
    memset(out->soft,0,5);
    for (int bit=5;bit<112;bit++) {
        float weight=fit->confidence[bit]*scale;
        int rounded=weight>120?120:(int)(weight+.5f);
        out->soft[bit]=(word[bit>>3]&(128>>(bit&7)))?rounded:-rounded;
    }
    return rank;
}

/* Work in excess noise units per occurrence. An individual capture has
 * its own sample phase, gain and baseline. The word is the shared unknown. */
static int combine(const repeat_observation *const *captures,int count,const sx_model *model,
            size_t model_len,void *scratch,size_t size,uint8_t *frame) {
    if (size<sizeof(sx_rank_workspace) || count<1 || count>5 ||
        model_len<sizeof(*model) || model->fetches>SX_MAX_FETCHES ||
        model_len!=sizeof(*model)+2u*SX_TEMPLATES*model->fetches) return -1;
    sx_rank_workspace *ranked=scratch;
    sx_workspace *w=&ranked->list.base;
    int cursor[5]={0};
    const int16_t *ticks[5];
    for (int i=0;i<count;i++) {
        if(captures[i]->schedule>=SX_TEMPLATES || !(captures[i]->weight>0)) return -1;
        ticks[i]=model->ticks+captures[i]->schedule*model->fetches;
        while(cursor[i]<model->fetches && ticks[i][cursor[i]]<8*SX_PHASES) cursor[i]++;
    }
    w->df=17;
    memset(w->costs[0],0,sizeof(w->costs[0]));
    for(int n=0;n<112;n++){
        if((n&7)==0) SX_SERVICE_RX();
        float branch[128]={0};
        int edge=n<3?n:n<109?3:n-105, start=(8+n)*SX_PHASES;
        for(int i=0;i<count;i++){
            while(cursor[i]<model->fetches && ticks[i][cursor[i]]<start+SX_PHASES){
                int k=cursor[i]++,phase=ticks[i][k]-start;
                if(phase<0 || phase>=SX_PHASES) return -1;
                const int16_t *expected=model->expected[phase][edge];
                for(int s=0;s<128;s++){
                    int error=captures[i]->obs[k]-expected[s];
                    branch[s]+=error*error*captures[i]->weight;
                }
            }
        }
        int mask=0,value=0;
        for(int k=0;n<8 && k<7;k++){
            int b=n-3+k;
            if(b>=0 && b<5){mask|=1<<(6-k);value|=((17>>(4-b))&1)<<(6-k);}
        }
        for(int s=0;s<64;s++){
            int a=w->costs[n&1][s>>1],b=w->costs[n&1][32+(s>>1)];
            a=a>=SX_INF || (s&mask)!=value ? SX_INF : a+(int)(64*branch[s]+.5f);
            b=b>=SX_INF || ((s+64)&mask)!=value ? SX_INF : b+(int)(64*branch[s+64]+.5f);
            w->costs[(n+1)&1][s]=a<b?a:b;
            w->decisions[n][s]=b<a;
            ranked->list.delta[n][s]=a>b?a-b:b-a;
        }
    }
    int state=0;
    for(int s=1;s<64;s++)if(w->costs[0][s]<w->costs[0][state])state=s;
    traceback(w,state,-1,frame,112);
    if(check_frame(frame,112))return 1;
    return ranked_timing(ranked,frame);
}

typedef struct {
    repeat_observation rows[REPEAT_PAST+1];
    uint8_t next, count, used;
    struct {
        uint32_t when;
        uint8_t word[14];
    } reports[REPEAT_PAST+1];
} repeat_bank;

size_t sx_repeat_size(void) { return sizeof(repeat_bank); }

static int same_repeat_word(const uint8_t *a,const uint8_t *b) {
    for(int i=0;i<14;i++)if(a[i]!=b[i])return 0;
    return 1;
}

static int magnitude(int x) { return x<0 ? -x : x; }

/* Compare all free bits and give an equal vote to address/capability plus
 * parity. The same address alone is insufficient to identify a repeat. */
static float repeat_distance(const int8_t *a,const int8_t *b) {
    unsigned weight=0, changed=0, ends=0, end_changes=0;
    for(int k=5;k<112;k++){
        int x=magnitude(a[k]),y=magnitude(b[k]),w=x<y?x:y;
        int diff=(a[k]>0)!=(b[k]>0);
        weight+=w;
        if(diff)changed+=w;
        if(k<32 || k>=88){ends+=w;if(diff)end_changes+=w;}
    }
    return .5f*(changed*adaptive_reciprocal(weight?weight:1)+
                end_changes*adaptive_reciprocal(ends?ends:1));
}

static int supports_word(const uint8_t *word,const int8_t *soft) {
    int errors=0,disagree=0,total=0,strongest_error=0;
    for(int bit=5;bit<112;bit++){
        int w=magnitude(soft[bit]);
        int different=frame_bit(word,bit)!=(soft[bit]>0);
        /* Every contributing waveform must support all of its confident bits.
         * A magnitude of 60 is twice its median free-bit confidence. */
        if(different && w>60)return 0;
        if(bit<88)continue;
        total+=w;
        if(different){errors++;disagree+=w;if(w>strongest_error)strongest_error=w;}
    }
    /* Demand eight received parity bits with stronger evidence than any
     * parity disagreement. An uncertain or tied bit cannot count toward eight. */
    int supported=0;
    for(int bit=88;bit<112;bit++)if(magnitude(soft[bit])>strongest_error)supported++;
    return errors<=8 && disagree*10<=3*total && supported>=8;
}

typedef struct {float mean; unsigned mask;} repeat_group;

static int best_groups(const repeat_observation *now,const repeat_bank *bank,
                       const repeat_observation **near,repeat_group jobs[4][2]) {
    float distances[REPEAT_NEAR+1],pair[REPEAT_NEAR][REPEAT_NEAR];
    int count=0;
    for(int k=0;k<bank->count;k++){
        unsigned idx=(bank->next+REPEAT_PAST-k)%(REPEAT_PAST+1);
        const repeat_observation *prior=&bank->rows[idx];
        uint32_t age=now->when-prior->when;
        if(age<40 || age>REPEAT_MS || prior->block==now->block)continue;
        float dist=repeat_distance(now->soft,prior->soft);
        if(dist>.36f)continue;
        int i=count;
        while(i>0 && distances[i-1]>dist){
            if(i<REPEAT_NEAR){distances[i]=distances[i-1];near[i]=near[i-1];}
            i--;
        }
        if(i<REPEAT_NEAR){near[i]=prior;distances[i]=dist;if(count<REPEAT_NEAR)count++;}
    }
    for(int i=0;i<count;i++)for(int j=0;j<i;j++)
        pair[i][j]=repeat_distance(near[i]->soft,near[j]->soft);
    for(unsigned mask=1;mask<(1u<<count);mask++){
        int members=0,valid=1;
        float sum=0;
        for(int i=0;i<count && valid;i++)if(mask&(1u<<i)){
            members++;sum+=distances[i];
            for(int j=0;j<i;j++)if(mask&(1u<<j)){
                uint32_t age=near[i]->when-near[j]->when;
                if(pair[i][j]>.36f || age<40u || (0u-age)<40u){valid=0;break;}
                sum+=pair[i][j];
            }
        }
        if(!valid || members>REPEAT_GROUP-1)continue;
        float mean=sum*adaptive_reciprocal((members*(members+1))/2);
        repeat_group *best=jobs[members-1];
        for(int j=0;j<2;j++)if(!best[j].mask || mean<best[j].mean){
            if(!j)best[1]=best[0];
            best[j].mask=mask;best[j].mean=mean;break;
        }
    }
    return count;
}

/* All slots in the bank belong to this boot. A new reception reports at
 * most one unambiguous word, with the original block IDs of its supporters.
 * At most two RF-ranked groups of each length (2 through 5) are solved. */
int sx_repeat_add(const uint8_t *raw,int len,int at,const sx_model *model,size_t mlen,
                  void *work,size_t size,void *storage,size_t bank_size,uint32_t block,
                  uint32_t when,uint8_t *word,uint32_t *support) {
    if(bank_size!=sizeof(repeat_bank) || size<sizeof(sx_rank_workspace) ||
       ((uintptr_t)storage&3u) || ((uintptr_t)work&3u) || ((uintptr_t)model&1u))return -1;
    repeat_bank *bank=storage;
    if(bank->next>REPEAT_PAST || bank->count>REPEAT_PAST || bank->used>REPEAT_PAST)return -1;
    for(int i=0;i<bank->count;i++)
        if(bank->rows[(bank->next+REPEAT_PAST-i)%(REPEAT_PAST+1)].block==block)return 0;
    repeat_observation *now=&bank->rows[bank->next];
    int fit=make_observation(raw,len,at,model,(int)mlen,work,size,now);
    if(fit<=0)return fit;
    now->block=block;now->when=when;
    const repeat_observation *near[REPEAT_NEAR];
    repeat_group jobs[REPEAT_GROUP-1][2]={0};
    int n=best_groups(now,bank,near,jobs);
    bank->next=(bank->next+1)%(REPEAT_PAST+1);
    if(bank->count<REPEAT_PAST)bank->count++;
    int result=0,best_count=0;
    for(int size_index=0;size_index<REPEAT_GROUP-1;size_index++)
        for(int j=0;j<2;j++){
            unsigned mask=jobs[size_index][j].mask;
            if(!mask)continue;
            const repeat_observation *group[REPEAT_GROUP];
            int count=0;
            for(int k=0;k<n;k++)if(mask&(1u<<k))group[count++]=near[k];
            group[count++]=now;
            uint8_t candidate[14];
            int rank=combine(group,count,model,mlen,work,size,candidate);
            if(rank<0)return -1;
            if(!rank)continue;
            int agree=1;
            for(int k=0;k<count;k++)if(!supports_word(candidate,group[k]->soft))agree=0;
            if(!agree)continue;
            if(result && !same_repeat_word(candidate,word))return 0;
            if(!best_count || count<best_count){
                memcpy(word,candidate,14);
                support[0]=count;
                for(int k=0;k<count;k++)support[k+1]=group[k]->block;
                best_count=count;result=rank;
            }
        }
    if(result){
        for(unsigned k=0;k<REPEAT_PAST+1;k++){
            if(when-bank->reports[k].when<=REPEAT_MS && same_repeat_word(bank->reports[k].word,word))return 0;
        }
        memcpy(bank->reports[bank->used].word,word,14);
        bank->reports[bank->used].when=when;
        bank->used=(bank->used+1)%(REPEAT_PAST+1);
    }
    return result;
}
