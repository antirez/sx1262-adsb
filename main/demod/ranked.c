/* Bounded serial list Viterbi for 112-bit Mode S. The path order depends
 * exclusively on measurement cost. CRC is consulted after each traceback.
 * Included by sequence.c so host replay and MicroPython share all metrics. */
#define SX_RANK_LIMIT 128
#define SX_RANK_TIMINGS 6
typedef struct {
    uint32_t flips[4];
    int cost, parent_cost;
    uint8_t end, branch, low, high;
} sx_path;
typedef struct {
    sx_list_workspace list;
    sx_path heap[64+3*SX_RANK_LIMIT+4];
    int used;
} sx_rank_workspace;

size_t sx_rank_workspace_size(void){return sizeof(sx_rank_workspace);}

static int path_bit(const sx_path *p,int i){return (p->flips[i>>5]>>(i&31))&1;}
static void toggle_path(sx_path *p,int i){p->flips[i>>5]^=1u<<(i&31);}

static void push_path(sx_rank_workspace *w,sx_path x){
    int i=w->used++;
    while(i){
        int parent=(i-1)/2;
        if(w->heap[parent].cost<=x.cost)break;
        w->heap[i]=w->heap[parent];i=parent;
    }
    w->heap[i]=x;
}

static sx_path pop_path(sx_rank_workspace *w){
    sx_path first=w->heap[0],last=w->heap[--w->used];
    int i=0;
    while(2*i+1<w->used){
        int child=2*i+1;
        if(child+1<w->used && w->heap[child+1].cost<w->heap[child].cost)child++;
        if(last.cost<=w->heap[child].cost)break;
        w->heap[i]=w->heap[child];i=child;
    }
    w->heap[i]=last;
    return first;
}

/* The three child ranges reuse the survivor states from the word traceback.
 * Undoing the current deviation only changes states below its bit. All
 * siblings above that bit share the saved suffix, including in cost ties. */
static void enqueue_deviation(sx_rank_workspace *w,sx_path parent,int low,int high,
                              const uint8_t states[112]){
    int best=SX_INF,where=-1;
    for(int n=high-1;n>=low;n--){
        int delta=w->list.delta[n][states[n]];
        if(delta<best){best=delta;where=n;}
    }
    if(where<0 || best>=SX_INF-parent.cost)return;
    toggle_path(&parent,where);
    parent.parent_cost=parent.cost;parent.cost+=best;
    parent.branch=where;parent.low=low;parent.high=high;
    push_path(w,parent);
}

static int ranked_timing(sx_rank_workspace *w,uint8_t *frame){
    w->used=0;
    /* Three nuisance bits outside each end of the actual transmission have
     * identical RF costs. Use one representation per real bit sequence. */
    for(int s=0;s<64;s+=8)if(w->list.base.costs[0][s]<SX_INF){
        sx_path first={{0,0,0,0},w->list.base.costs[0][s],0,(uint8_t)s,112,0,112};
        push_path(w,first);
    }
    for(int k=1;k<=SX_RANK_LIMIT && w->used;k++){
        if ((k&7)==1) SX_SERVICE_RX();
        sx_path x=pop_path(w);int state=x.end;
        uint8_t states[112];
        for(int j=0;j<14;j++)frame[j]=0;
        for(int n=111;n>=0;n--){
            states[n]=(uint8_t)state;
            int bit=n+3;
            if(bit<112)frame[bit>>3]|=(state&1)<<(7-(bit&7));
            state=(state>>1)+32*(w->list.base.decisions[n][state]^path_bit(&x,n));
        }
        frame[0]|=(state&7)<<5;
        if(check_selected_format(frame,112,w->list.base.df))return k;
        enqueue_deviation(w,x,3,x.branch,states);
        if(x.branch<112){
            toggle_path(&x,x.branch);x.cost=x.parent_cost;
            state=states[x.branch];
            for(int n=x.branch;n>x.low;n--){
                state=(state>>1)+32*(w->list.base.decisions[n][state]^path_bit(&x,n));
                states[n-1]=(uint8_t)state;
            }
            enqueue_deviation(w,x,x.low,x.branch,states);
            enqueue_deviation(w,x,x.branch+1,x.high,states);
        }
    }
    return 0;
}

/* 1..32 retains the ordinary meaning on distinct timing hypotheses.
 * Larger values are 1000 * one-based timing rank + one-based path rank. */
static int ranked_header(const uint8_t *raw,size_t n,int anchor,const void *model,size_t len,
                     void *workspace,size_t size,uint8_t *frame,const uint8_t *header,int df,int status_layout,int residual_pair){
    if(size<sizeof(sx_rank_workspace) || ((uintptr_t)workspace&3u))return -1;
    sx_rank_workspace *w=workspace;
    for(int i=0;i<SX_TOP;i++)w->list.base.chosen[i].gain=0;
    int prior=decode_window_header(raw,n,anchor,model,len,&w->list,sizeof(w->list),frame,&w->list,112,1,header,df,status_layout,residual_pair);
    if(prior)return prior;
    for(int t=0;t<SX_RANK_TIMINGS && w->list.base.chosen[t].gain>0;t++){
        w->list.count=0;
        decode_one(raw,model,&w->list.base,&w->list.base.chosen[t],frame,&w->list,112);
        int rank=ranked_timing(w,frame);
        if(rank)return 1000*(t+1)+rank;
    }
    return 0;
}

int sx_decode_ranked(const uint8_t *raw,size_t n,int anchor,const void *model,size_t len,
                     void *workspace,size_t size,uint8_t *frame){
    return ranked_header(raw,n,anchor,model,len,workspace,size,frame,NULL,17,0,0);
}

int sx_decode_test(const uint8_t *raw,size_t n,int anchor,const void *model,size_t len,
                   void *workspace,size_t size,uint8_t *frame){
    return ranked_header(raw,n,anchor,model,len,workspace,size,frame,NULL,2,0,0);
}

/* An independently decoded short reply supplies the CA and ICAO bits only.
 * The 56 message and 24 parity bits are always received from the waveform. */
int sx_decode_tracked(const uint8_t *raw,size_t n,int anchor,const void *model,size_t len,
                      void *workspace,size_t size,uint8_t *frame,const uint8_t *df11,size_t prefixlen){
    if(prefixlen!=7 || df11[0]>>3!=11 || sx_crc(df11,7)!=0)return -1;
    uint8_t header[4]={(17<<3)|(df11[0]&7),df11[1],df11[2],df11[3]};
    int rank=ranked_header(raw,n,anchor,model,len,workspace,size,frame,header,17,0,0);
    if(rank>0)for(int i=0;i<4;i++)if(frame[i]!=header[i])return 0;
    return rank;
}

/* A full-length power rise pays for the deeper list on battery. Inputs
 * here are register updates at 0.75 us, with an 8 us preamble and a
 * 112 us body. The near and far quiet periods must both support the rise. */
int sx_decode_latch(const uint8_t *raw,size_t n,int anchor,const void *model,size_t len,
                    void *workspace,size_t size,uint8_t *frame){
    if(anchor>=12 && (size_t)anchor+173<=n){
        int before=0,after=0,body=0;
        for(int i=0;i<8;i++){
            before+=raw[anchor-12+i];
            after+=raw[anchor+165+i];
        }
        for(int i=16;i<144;i++)body+=raw[anchor+i];
        int quiet=before<after?before:after;
        if(16*quiet-body>=14*128)
            return sx_decode_ranked(raw,n,anchor,model,len,workspace,size,frame);
    }
    return sx_decode_list(raw,n,anchor,model,len,workspace,size,frame);
}


/* The field app schedules this final retry after its ordinary fits.
 * It uses 0.75 us register updates and retains the same DF17 parity gate. */
int sx_decode_residual(const uint8_t *raw,size_t n,int at,const void *model,size_t len,
                       void *workspace,size_t size,uint8_t *frame) {
    return ranked_header(raw,n,at,model,len,workspace,size,frame,NULL,17,0,1);
}
