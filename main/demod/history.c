/* Receive-only acquisition of a shared DF17 header. Votes are derived from
 * earlier measured envelopes, with the current burst used only to rank them. */
#define HEADER_BITS 27
#define HEADER_NEAR 8
#define HEADER_RESULTS 2
#define HEADER_ROW 7
#define STATUS_BITS 40

static int measure_profile(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                      void *memory,size_t available,uint8_t *output,int status_bits) {
    if(available<sizeof(sx_family_workspace) || ((uintptr_t)memory&3u)) return -1;
    if(at<12 || (size_t)at+173>n) return 0;
    sx_family_workspace *work=memory;
    uint8_t word[14];
    int rank=sx_unconstrained_fit(raw,n,at,m,len,17,work,word);
    if(rank<=0) return rank;
    int32_t sorted[107];
    for(int bit=5;bit<112;bit++) {
        int32_t value=work->confidence[bit];
        int i=bit-5;
        while(i>0 && sorted[i-1]>value) {sorted[i]=sorted[i-1];i--;}
        sorted[i]=value;
    }
    int median=sorted[53],shift=0;
    if(median<1) median=1;
    while(median>1000000) {median>>=1;shift++;}
    int out=0;
    for(int bit=5;bit<(status_bits?88:32);bit++) {
        if(bit>=40 && bit<56) continue;  /* Always leave the status and identity code free. */
        int val=work->confidence[bit]>>shift;
        int magnitude=val>=3*median?96:(32*val+median/2)/median;
        output[out++]=(uint8_t)(128+(frame_bit(word,bit)?magnitude:-magnitude));
    }
    return 1;
}

int sx_header_profile(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                      void *memory,size_t available,uint8_t *output) {
    return measure_profile(raw,n,at,m,len,memory,available,output,0);
}

int sx_status_profile(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                      void *memory,size_t available,uint8_t *output) {
    return measure_profile(raw,n,at,m,len,memory,available,output,1);
}

static int header_distance(const uint8_t *a,const uint8_t *b) {
    int disagreements=0,weight=0;
    for(int k=0;k<HEADER_BITS;k++) {
        int u=(int)a[k]-128,v=(int)b[k]-128;
        int x=u<0?-u:u,y=v<0?-v:v,w=x<y?x:y;
        weight+=w;
        if((u<0)!=(v<0)) disagreements+=w;
    }
    return weight<160?1024:1024*disagreements/weight;
}

static int same_header(const uint8_t *a,const uint8_t *b) {
    for(int i=0;i<4;i++) if(a[i]!=b[i]) return 0;
    return 1;
}

static void remember_header(uint8_t *answers,int *cost,int *count,
                            const uint8_t *word,const uint8_t *sources,int score) {
    int same=0;
    while(same<*count && !same_header(answers+HEADER_ROW*same,word)) same++;
    if(same<*count && cost[same]<=score) return;
    if(same==*count && *count==HEADER_RESULTS && cost[*count-1]<=score) return;
    if(same==*count) {
        if(*count<HEADER_RESULTS) (*count)++;
        else same--;
    }
    while(same && score<cost[same-1]) {
        memcpy(answers+HEADER_ROW*same,answers+HEADER_ROW*(same-1),HEADER_ROW);
        cost[same]=cost[same-1];same--;
    }
    memcpy(answers+HEADER_ROW*same,word,4);
    memcpy(answers+HEADER_ROW*same+4,sources,3);
    cost[same]=score;
}

int sx_header_guesses(const uint8_t *bank,int entries,const uint8_t *now,uint8_t *answers) {
    if(entries<0 || entries>40) return -1;
    uint8_t indices[HEADER_NEAR];int distances[HEADER_NEAR],used=0;
    for(int i=0;i<entries;i++) {
        int d=header_distance(bank+HEADER_BITS*i,now),j=0;
        if(d>327) continue;
        while(j<used && distances[j]<=d) j++;
        if(j==HEADER_NEAR) continue;
        if(used<HEADER_NEAR) used++;
        for(int k=used-1;k>j;k--) {distances[k]=distances[k-1];indices[k]=indices[k-1];}
        distances[j]=d;indices[j]=i;
    }
    uint8_t compatible[HEADER_NEAR][HEADER_NEAR]={{0}};
    for(int i=0;i<used;i++) for(int j=i+1;j<used;j++)
        compatible[i][j]=header_distance(bank+HEADER_BITS*indices[i],bank+HEADER_BITS*indices[j])<=266;
    int count=0,cost[HEADER_RESULTS];
    for(int a=0;a<used;a++) for(int b=a+1;b<used;b++) for(int c=b+1;c<used;c++) {
        if(!compatible[a][b] || !compatible[a][c] || !compatible[b][c]) continue;
        uint8_t src[3]={indices[a],indices[b],indices[c]};
        uint8_t header[4]={0x88,0,0,0};
        int uncertain=0,contested=0,weight=0,disagreements=0;
        for(int j=0;j<HEADER_BITS;j++){
            int total=0,absolute=0;
            for(int z=0;z<3;z++) {
                int v=(int)bank[HEADER_BITS*src[z]+j]-128;
                total+=v;absolute+=v<0?-v:v;
            }
            int amplitude=total<0?-total:total;
            uncertain+=amplitude<8;
            contested+=100*amplitude<28*absolute;
            if(total>0) header[(j+5)/8]|=(uint8_t)(128u>>((j+5)%8));
            int observed=((int)now[j]-128)*3;
            int magnitude=observed<0?-observed:observed;
            int w=amplitude<magnitude?amplitude:magnitude;
            weight+=w;
            if((total<0)!=(observed<0)) disagreements+=w;
        }
        if(uncertain>3 || contested>4 || weight<480) continue;
        int score=3*1024*disagreements/weight+distances[a]+distances[b]+distances[c];
        remember_header(answers,cost,&count,header,src,score);
    }
    return count;
}

int sx_decode_header(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                     void *work,size_t size,const uint8_t *header,size_t hlen,uint8_t *frame) {
    if(hlen!=4 || header[0]>>3!=17) return -1;
    int rank=ranked_header(raw,n,at,m,len,work,size,frame,header,17,0,0);
    if(rank>0 && !same_header(frame,header)) return 0;
    return rank;
}

/* The 16 status/Mode A bits and all 24 parity bits remain ordinary RF
 * decisions. An empty prefix also leaves CA and the aircraft address free. */
int sx_decode_status(const uint8_t *raw,size_t n,int at,const sx_model *m,size_t len,
                     void *work,size_t size,const uint8_t *header,size_t hlen,uint8_t *frame) {
    if(hlen && (hlen!=4 || header[0]>>3!=17)) return -1;
    int rank=ranked_header(raw,n,at,m,len,work,size,frame,hlen?header:NULL,17,1,0);
    if(rank>0 && (frame[4]!=0xe1 || frame[7] || frame[8] || frame[9] || frame[10]
                  || (hlen && !same_header(frame,header)))) return 0;
    return rank;
}
