/* Compact Position Reporting sends fractions of a latitude/longitude cell.
 * Even and odd reports use slightly different grids. The pair tells us which
 * cell the aircraft is in. Once we know that, a single fresh report can put
 * the aircraft in the cell nearest its previous position. */
#include "cpr.h"
#include <math.h>
#include <string.h>

static const double pi=3.14159265358979323846, scale=131072.0;

static int zones(double lat) {
    lat=fabs(lat);
    if (lat>87) return 1;
    if (lat==87) return 2;
    double c=cos(lat*pi/180);
    return (int)fmin(59, floor(2*pi/acos(1-(1-cos(pi/30))/(c*c))));
}

static int positive_mod(int n, int m) {
    int r=n%m;
    return r<0 ? r+m : r;
}

static bool airborne_pair(const CprSample s[2], int odd, double *lat, double *lon) {
    int j=(int)floor((59.0*s[0].lat-60.0*s[1].lat)/scale+.5);
    double y[2];
    for (int i=0; i<2; i++) {
        y[i]=(positive_mod(j,60-i)+s[i].lat/scale)*360/(60-i);
        if (y[i]>=270) y[i]-=360;
        if (fabs(y[i])>90) return false;
    }
    int nl=zones(y[odd]);
    if (nl!=zones(y[1-odd])) return false;
    int m=(int)floor((s[0].lon*(nl-1.0)-s[1].lon*(double)nl)/scale+.5);
    int ni=nl-odd;
    if (ni<1) ni=1;
    *lat=y[odd];
    *lon=(positive_mod(m,ni)+s[odd].lon/scale)*360/ni;
    return true;
}

static double nearest_cell(double reference, double step, unsigned code) {
    double fraction=code/scale;
    return step*(floor(reference/step-fraction+.5)+fraction);
}

void cpr_update(CprTrack *t, const uint8_t w[14], int64_t ms) {
    int tc=w[4]>>3, odd=(w[6]>>2)&1;
    bool surface=tc>=5 && tc<=8;
    if (w[0]>>3!=17 || !(surface || (tc>=9 && tc<=18) || (tc>=20 && tc<=22))) return;
    if (surface!=t->surface) {
        memset(t->sample,0,sizeof(t->sample));
        t->surface=surface;
    }
    CprSample *s=&t->sample[odd], *other=&t->sample[1-odd];
    *s=(CprSample){.lat=((w[6]&3)<<15)|(w[7]<<7)|(w[8]>>1),
                   .lon=((w[8]&1)<<16)|(w[9]<<8)|w[10],.ms=ms,.valid=true};
    double lat,lon;
    bool paired=!surface && other->valid && ms>=other->ms && ms-other->ms<=10000 &&
                airborne_pair(t->sample,odd,&lat,&lon);
    if (!paired) {
        /* A surface grid spans only 90 degrees. It too needs an established
         * reference. Never substitute a presumed location for this radio. */
        if (!t->fixed || ms<t->fix_ms || ms-t->fix_ms>60000) return;
        double span=surface?90.0:360.0;
        lat=nearest_cell(t->lat,span/(60-odd),s->lat);
        if (fabs(lat)>90) return;
        int ni=zones(lat)-odd;
        lon=nearest_cell(t->lon,span/(ni>0?ni:1),s->lon);
        /* Check a local continuation against elapsed time. A 1 NM margin
         * covers rounding and nearby reports. 1200 knots allows 1 NM/3 s. */
        double dy=(lat-t->lat)*60;
        double dx=remainder(lon-t->lon,360)*60*cos((lat+t->lat)*pi/360);
        if (hypot(dx,dy)>1+(ms-t->fix_ms)/3000.0) return;
    }
    t->lat=lat;
    t->lon=lon-360*floor((lon+180)/360);
    t->fix_ms=ms;
    t->fixed=true;
}
