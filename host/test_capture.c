/* At high input rates a brief spike can otherwise take a queue slot from
 * a 64 us all-call or a 120 us extended squitter. Test queue admission,
 * independently of bit recovery. */
#include "receiver.h"
#include "selftest.h"
#include <stdio.h>
#include <string.h>

static void ignore(const Result *r) { (void)r; }

static bool check(Capture *c, ReplyShape expected, const char *name) {
    if (decoder_candidate(c)==expected) return true;
    fprintf(stderr,"Wrong queue admission for %s\n",name);
    return false;
}

/* The two interferers came back through a T-Echo on the coax bench. The
 * roof recording contains a repeated DF11 all-call. Its original 0.593 us
 * samples were resampled to the current scanner's 0.75 us period. */
static bool check_recordings(const char *path) {
    FILE *f=fopen(path,"r");
    if (!f) return false;
    char name[64], hex[2*SAMPLES+1];
    unsigned expected;
    Capture c={0};
    int n, records=0;
    while ((n=fscanf(f,"%u %63s %512[0123456789abcdef]",&expected,name,hex))==3) {
        if (expected>LONG_REPLY || strlen(hex)!=2*SAMPLES) break;
        for (int i=0; i<SAMPLES; i++) {
            unsigned byte;
            sscanf(hex+2*i,"%2x",&byte); /* The scanset above admits only hex. */
            c.raw[i]=byte;
        }
        if (!check(&c,expected,name)) break;
        records++;
    }
    bool ok=records>0 && n==EOF && !ferror(f);
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    if (!decoder_init(ignore)) return 1;
    Capture c={0};
    memcpy(c.raw,garden_raw,sizeof(c.raw));
    if (!check(&c,LONG_REPLY,"recorded DF17")) return 1;
    memset(c.raw,190,sizeof(c.raw));
    if (!check(&c,NO_REPLY,"flat floor")) return 1;
    memset(c.raw+64,150,24);
    if (!check(&c,NO_REPLY,"18 us spike")) return 1;
    memset(c.raw+64,150,85);
    if (!check(&c,NO_REPLY,"steady carrier")) return 1;
    for (int i=64; i<149; i++) c.raw[i]=150+((i%3)==0 ? 7 : 0);
    if (!check(&c,SHORT_REPLY,"modulated short reply envelope")) return 1;
    return argc==2 && check_recordings(argv[1]) ? 0 : 1;
}
