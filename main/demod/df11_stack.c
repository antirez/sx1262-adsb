/* Incremental alignment of repeated 64-us power envelopes. No bit or address is
 * supplied during alignment. A stack collects measurements from different replies. */
#include <stdint.h>
#include <stddef.h>

#define STACK_POINTS 680
#define ALIGN_POINTS 872
#define ALIGN_SHIFTS 97
typedef struct {
    float amplitude[STACK_POINTS];
    float weight;
    unsigned count;
} stack_data;

typedef struct {
    float x[ALIGN_POINTS], area[ALIGN_POINTS+1], energy[ALIGN_POINTS+1];
    float probability[ALIGN_SHIFTS], samples[392];
    uint16_t histogram[256];
} stack_work;

size_t sx_stack_size(void) {return sizeof(stack_data);}
size_t sx_stack_work_size(void) {return sizeof(stack_work);}

static int is_quiet(int index,int anchor) {
    int quarter_us=(index-anchor)*3;
    return (-220<quarter_us && quarter_us < -60) || (328<quarter_us && quarter_us<456);
}

static int quantile(const uint16_t *hist,int target) {
    int cumulative=0;
    for (int i=0;i<256;i++) {
        cumulative+=hist[i];
        if (cumulative>target) return i;
    }
    return 255;
}

/* The MicroPython native ABI has no floating division helper on Xtensa.
 * All denominators below are positive, finite normal numbers. An exponent
 * estimate and three Newton steps give single-precision reciprocals. */
static float positive_inverse(float value) {
    union {float f;uint32_t u;} bits={.f=value};
    bits.u=0x7ef311c3u-bits.u;
    float x=bits.f;
    for(int i=0;i<3;i++) x*=2.f-value*x;
    return x;
}

static float approximate_sqrt(float v) {
    /* Quiet amplitudes span a small positive range. Clamp the noise estimate
     * below 0.07, the minimum used by the original fit. */
    if (v<=.0049f) return .07f;
    float x = v>1.f ? v:1.f;
    for(int i=0;i<9;i++) x=.5f*(x+v*positive_inverse(x));
    return x;
}

static float probability(float delta) {
    if(delta < -20.f) return 0;
    /* Fourth-order Taylor on [-.079,0], followed by eight squarings. */
    float t=delta/256.f;
    float x=1.f+t*(1.f+t*(.5f+t*(0.16666667f+t*0.041666667f)));
    for(int i=0;i<8;i++) x*=x;
    return x;
}

int sx_stack_add(const uint8_t *raw,size_t n,int anchor,const void *model,size_t model_size,
                 void *acc,size_t acc_size,void *scratch,size_t scratch_size) {
    if(n>392 || n<80 || anchor<0 || (size_t)anchor>=n ||
       acc_size<sizeof(stack_data) || scratch_size<sizeof(stack_work) ||
       (((uintptr_t)acc|(uintptr_t)scratch)&3u) || ((uintptr_t)model&1u)) return -1;
    const sx_model *m=model;
    if(model_size<sizeof(*m) || m->magic[4]!='1' || m->magic[5]!='1' ||
       m->prelen!=260 || m->phases!=20) return -1;
    stack_data *s=acc;
    stack_work *w=scratch;
    int quiet=0,body=0;
    for(int i=0;i<256;i++) w->histogram[i]=0;
    for(int i=0;i<(int)n;i++) if(is_quiet(i,anchor)){w->histogram[raw[i]]++;quiet++;}
    if(quiet<12) return 0;
    int flo=quantile(w->histogram,(quiet-1)/2),fhi=quantile(w->histogram,quiet/2);
    float sum=0,squares=0,power=0;
    for(int i=0;i<(int)n;i++){
        float amp=(ratio_at(m,flo,raw[i])+ratio_at(m,fhi,raw[i]))/512.f-1.f;
        w->samples[i]=amp;
        if(is_quiet(i,anchor)){sum+=amp;squares+=amp*amp;}
        int t=(i-anchor)*3;
        if(48<t && t<212){power+=amp;body++;}
    }
    if(!body) return 0;
    float inv_quiet=positive_inverse((float)quiet);
    float mean=sum*inv_quiet;
    float noise=approximate_sqrt(squares*inv_quiet-mean*mean);
    float inv_noise=positive_inverse(noise);
    float gain=power*inv_noise*positive_inverse((float)body);
    if(gain>4.5f) gain=4.5f;
    if(gain<.1f) gain=.1f;
    w->area[0]=w->energy[0]=0;
    for(int i=0;i<ALIGN_POINTS;i++){
        int sixths=6*anchor+i-176;
        if(sixths<0)sixths=0;
        if(sixths>6*((int)n-1))sixths=6*((int)n-1);
        int q=sixths/6,j=q+1<(int)n ? q+1:q;
        float x=(w->samples[q]+(w->samples[j]-w->samples[q])*(sixths%6)*0.16666667f)*inv_noise;
        w->x[i]=x;w->area[i+1]=w->area[i]+x;w->energy[i+1]=w->energy[i]+x*x;
    }
    float peak=-1e30f,total=0;
    for(int i=0;i<ALIGN_SHIFTS;i++){
        if((i&15)==0) SX_SERVICE_RX();
        int k=2*i;float dot=0;
        for(int j=0;j<104;j++){
            int h=5*j/2;
            float v=(j&1)?(m->pre[h]+m->pre[h+1])/128.f:m->pre[h]/64.f;
            dot+=v*w->x[k+80+j];
        }
        dot+=.06f*(w->area[k+592]-w->area[k+184]);
        const float *e=w->energy+k;
        float sq=.06f*(e[680]-e[0])+.94f*(e[184]-e[80])+
                 .02f*(e[80]-e[0]+e[680]-e[601]);
        float delay=(i-48)*.05f;
        float ll=(gain*dot-.5f*sq)*.125f-.5f*delay*delay;
        w->probability[i]=ll;
        if(ll>peak)peak=ll;
    }
    for(int i=0;i<ALIGN_SHIFTS;i++){
        w->probability[i]=probability(w->probability[i]-peak);
        total+=w->probability[i];
    }
    float inv_total=positive_inverse(total);
    for(int i=0;i<ALIGN_SHIFTS;i++){
        if((i&15)==0) SX_SERVICE_RX();
        float p=w->probability[i]*inv_total;
        if(p<.00001f)continue;
        float weight=p*gain;
        for(int j=0;j<STACK_POINTS;j++)s->amplitude[j]+=weight*w->x[2*i+j];
    }
    s->weight+=gain*gain;
    s->count++;
    return (int)s->count;
}

int sx_stack_phase(const void *acc,size_t acc_size,const void *model,size_t model_size,
                   int phase,uint8_t *raw,size_t n) {
    if(phase<0 || phase>=6 || n<160 || acc_size<sizeof(stack_data) ||
       ((uintptr_t)acc&3u) || ((uintptr_t)model&1u) || model_size<sizeof(sx_model))return -1;
    const stack_data *s=acc;const sx_model *m=model;
    if(s->count<5 || s->weight<=0) return 0;
    float inv_weight=positive_inverse(s->weight);
    for(int i=0;i<160;i++){
        int k=(i-30)*6+phase+80;
        float amp=256.f;
        if(k>=0 && k<STACK_POINTS)amp+=1024.f*s->amplitude[k]*inv_weight;
        if(amp<12.8f)amp=12.8f;
        int l=0,h=255;
        while(l+1<h){
            int mid=(l+h)/2;
            if(m->ratio[mid]<amp)l=mid;else h=mid;
        }
        int q=amp*amp<(float)m->ratio[l]*m->ratio[h]?l:h;
        int code=172+128-q;
        raw[i]=(uint8_t)(code>255?255:code<0?0:code);
    }
    return 160;
}

/* Thermal-noise rejection for an independently triggered short reply. */
int sx_short_gate(const uint8_t *raw,int n,int period_ps,int first,int last,int *score) {
    if(n<24 || n>512 || period_ps<500000 || period_ps>850000)return -2;
    const int nominal[10]={-30,-6,19,92,119,143,10,27,91,104};
    int edge[10],sum[513];sum[0]=0;
    for(int j=0;j<n;j++)sum[j+1]=sum[j]+raw[j];
    for(int j=0;j<10;j++){
        int d=nominal[j]*592550;
        edge[j]=(d+(d>=0?period_ps/2:-period_ps/2))/period_ps;
    }
    if(first < 2-edge[0]) first=2-edge[0];
    if(last > n-edge[5]-1) last=n-edge[5]-1;
    float inv_width[5];
    for(int j=0;j<5;j++)inv_width[j]=positive_inverse((float)(edge[2*j+1]-edge[2*j]));
    int best=-1;float strength=0;
    for(int at=first;at<last;at++){
        float v[5];
        for(int j=0;j<5;j++){
            int a=edge[2*j],b=edge[2*j+1];
            v[j]=(sum[at+b]-sum[at+a])*inv_width[j];
        }
        float quieter=v[0]<v[2]?v[0]:v[2];
        float rise=(quieter-v[1])*.5f;
        if(rise>=6.f && rise>strength && v[0]-v[3]>=3.6f && v[2]-v[4]>=3.6f){
            strength=rise;best=at;
        }
    }
    *score=(int)(100*strength+.5f);
    return best;
}
