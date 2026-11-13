#include "tuning.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr); exit(1); } } while (0)

int main(void) {
    Button b={0};
    /* Include contact bounce and a second press just inside the double-click
     * interval. Neither press may deliver an early single tap. */
    CHECK(button_update(&b,true,0)==KEY_NONE);
    CHECK(button_update(&b,false,4)==KEY_NONE);
    CHECK(button_update(&b,true,8)==KEY_NONE);
    CHECK(button_update(&b,true,40)==KEY_NONE);
    CHECK(button_update(&b,false,80)==KEY_NONE);
    CHECK(button_update(&b,false,110)==KEY_NONE);
    CHECK(button_update(&b,true,370)==KEY_NONE);
    CHECK(button_update(&b,true,405)==KEY_NONE);
    CHECK(button_update(&b,false,450)==KEY_NONE);
    CHECK(button_update(&b,false,480)==KEY_DOUBLE);
    CHECK(button_update(&b,false,900)==KEY_NONE);
    button_update(&b,true,1000);
    button_update(&b,true,1030);
    button_update(&b,false,1100);
    button_update(&b,false,1130);
    CHECK(button_update(&b,false,1400)==KEY_TAP);
    button_update(&b,true,2000);
    button_update(&b,true,2030);
    CHECK(button_update(&b,true,2800)==KEY_HOLD);
    CHECK(button_update(&b,true,3250)==KEY_REPEAT);
    button_update(&b,false,3300);
    CHECK(button_update(&b,false,3350)==KEY_NONE);
    CHECK(button_update(&b,false,4000)==KEY_NONE);

    Menu m;
    menu_open(&m,DEFAULT_HZ);
    menu_adjust(&m);
    CHECK(m.hz==DEFAULT_HZ+100000);
    menu_next(&m);
    menu_adjust(&m);
    CHECK(m.hz==DEFAULT_HZ);
    m.hz=MIN_HZ+1000;
    menu_adjust(&m);
    CHECK(m.hz==MIN_HZ);
    m.action=TUNE_UP;
    m.hz=MAX_HZ-1000;
    menu_adjust(&m);
    CHECK(m.hz==MAX_HZ);
    m.action=DEFAULT_FREQ;
    menu_adjust(&m);
    CHECK(m.hz==DEFAULT_HZ);

    Scope scope={0};
    ScopeView v;
    Screen s;
    Capture burst={.block=42,.floor=185};
    memset(burst.raw,185,sizeof(burst.raw));
    burst.raw[23]=140;
    scope_sample(&scope,m.hz,100,180,NULL);
    scope_sample(&scope,m.hz,101,184,&burst);
    scope_view(&scope,m.hz,110,&v);
    CHECK(v.samples==2 && v.columns==1 && v.level==182 && v.strongest==140);
    CHECK(v.valid[127] && v.mean[127]==182 && v.peak[127]==140);
    CHECK(v.triggers==1 && v.burst_peak[127]==140 && !v.zoom && !v.block);

    /* A constant carrier, a rising step and a falling step have no complete
     * envelope. All three still register as triggers on the scrolling graph. */
    memset(burst.raw,150,SAMPLES);
    scope_sample(&scope,m.hz,121,185,&burst);
    memset(burst.raw,185,60);
    scope_sample(&scope,m.hz,122,185,&burst);
    memset(burst.raw,150,60);
    memset(burst.raw+140,185,SAMPLES-140);
    scope_sample(&scope,m.hz,123,185,&burst);
    scope_view(&scope,m.hz,125,&v);
    CHECK(!v.zoom && v.triggers==4);

    burst.block=43;
    memset(burst.raw,185,SAMPLES);
    memset(burst.raw+60,174,48); /* Half a dB below the six-dB threshold. */
    scope_sample(&scope,m.hz,150,185,&burst);
    scope_view(&scope,m.hz,151,&v);
    CHECK(!v.zoom);
    memset(burst.raw+60,173,48);
    scope_sample(&scope,m.hz,200,185,&burst);
    scope_view(&scope,m.hz,210,&v);
    CHECK(v.zoom && v.block==43 && v.wave_peak==173 && v.wave[30]==173);
    menu_screen(&m,&v,NULL,s);
    CHECK(!strcmp(s[1],"Hold -87 dBm"));

    /* A stronger envelope during the hold cannot change a pixel or move
     * the deadline. Ambient readings keep filling the live history. */
    burst.block=44;
    memset(burst.raw+60,140,80);
    scope_sample(&scope,m.hz,3050,190,NULL);
    scope_sample(&scope,m.hz,3199,194,&burst);
    scope_view(&scope,m.hz,3199,&v);
    CHECK(v.zoom && v.block==43 && v.wave[30]==173 && v.wave_peak==173);
    scope_view(&scope,m.hz,3200,&v); /* Expires even without a new sample. */
    CHECK(!v.zoom && !v.block && v.samples==2 && v.level==192);
    scope_sample(&scope,m.hz,4199,185,&burst);
    scope_view(&scope,m.hz,4199,&v);
    CHECK(!v.zoom);
    scope_sample(&scope,m.hz,4200,185,&burst);
    scope_view(&scope,m.hz,4200,&v);
    CHECK(v.zoom && v.block==44 && v.wave[30]==140);

    /* Retuning cancels the old hold and lets the new channel latch at once. */
    scope_view(&scope,m.hz+100000,5000,&v);
    CHECK(!v.zoom && !v.samples);
    scope_sample(&scope,m.hz+100000,5100,200,NULL);
    scope_view(&scope,m.hz,5100,&v);
    CHECK(!v.samples && !v.zoom);
    scope_view(&scope,m.hz+100000,5100,&v);
    CHECK(v.level==200 && v.strongest==200 && v.samples==1);
    CHECK(!v.zoom && !v.triggers && v.burst_peak[127]==255);
    menu_screen(&m,&v,NULL,s);
    CHECK(!strcmp(s[0],"RX 1089.800 MHz") && !strcmp(s[1],"A-100 P-100 dBm"));
    scope_sample(&scope,m.hz+100000,5101,200,&burst);
    scope_view(&scope,m.hz+100000,5101,&v);
    CHECK(v.zoom && v.block==44);
    m.action=DISCARD;
    menu_next(&m);
    CHECK(m.action==CLEAR_HISTORY);
    menu_screen(&m,&v,NULL,s);
    CHECK(!strcmp(s[6],"Clear history") && !strcmp(s[7],"Hold to erase"));
    menu_adjust(&m);
    CHECK(m.hz==DEFAULT_HZ);
    menu_next(&m);
    CHECK(m.action==TUNE_UP);
    return 0;
}
