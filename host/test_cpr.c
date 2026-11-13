#include "cpr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void feed(CprTrack *t, const char *hex, int64_t ms) {
    uint8_t w[14];
    for (unsigned i=0;i<14;i++) sscanf(hex+2*i,"%2hhx",w+i);
    cpr_update(t,w,ms);
}

int main(void) {
    const char *odd="8d4baa79580db4913fe0a3be306b", *even="8d4baa79580db0fbd3f6267aeda9";
    CprTrack t={0};
    feed(&t,odd,0);
    feed(&t,odd,1000);
    CHECK(!t.fixed);
    feed(&t,even,2120);
    CHECK(t.fixed && t.fix_ms==2120);
    CHECK(fabs(t.lat-37.47550964)<.000001 && fabs(t.lon-15.17176851)<.000001);
    feed(&t,even,20000); /* Single report continues a recent global fix. */
    CHECK(t.fix_ms==20000);
    feed(&t,even,81000);
    CHECK(t.fix_ms==20000);
    t=(CprTrack){0};
    feed(&t,odd,100);
    feed(&t,even,10101);
    CHECK(!t.fixed);
    /* Published pair from 40621D, tested with each possible latest grid. */
    t=(CprTrack){0};
    feed(&t,"8d40621d58c386435cc412692ad6",0);
    feed(&t,"8d40621d58c382d690c8ac2863a7",10000);
    CHECK(fabs(t.lat-52.25720215)<.000001 && fabs(t.lon-3.91937256)<.000001);
    feed(&t,"8d40621d58c386435cc412692ad6",11000);
    CHECK(fabs(t.lat-52.26578017)<.000001 && fabs(t.lon-3.93891253)<.000001);
    /* Surface reports repeat every 90 degrees. A receiver that starts on
     * the ground has no way to choose the right quadrant from these alone. */
    t=(CprTrack){0};
    feed(&t,"8c4841753aab238733c8cd4020b1",0);
    CHECK(!t.fixed);
    feed(&t,"8c4841753a8a35323faebdac702d",2000);
    CHECK(!t.fixed);
    /* The book's local surface example, after a fix near the runway. */
    t=(CprTrack){.fixed=true,.lat=52.320607,.lon=4.734735};
    feed(&t,"8c4841753a9a153237aef0f275be",2000);
    CHECK(t.fix_ms==2000 && fabs(t.lat-52.320561)<.000001 && fabs(t.lon-4.735735)<.000001);
    t=(CprTrack){.fixed=true,.lat=52.32,.lon=4.5};
    feed(&t,"8c4841753a9a153237aef0f275be",2000);
    CHECK(t.fix_ms==0); /* The implied ground movement is impossible. */
    return 0;
}
