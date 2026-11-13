/* Check the cards a walker actually sees, using packets from the journal.
 * In particular, an old card must never borrow a later callsign or fix. */
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

static Card packet(unsigned boot, int ms, const char *hex) {
    Card c={.boot=boot,.ms=ms,.length=strlen(hex)/2};
    for (int i=0; i<c.length; i++) CHECK(sscanf(hex+2*i,"%2hhx",c.word+i)==1);
    CHECK(card_valid(&c));
    return c;
}

int main(void) {
    Screen s;
    history_set_boot(1);
    const Screen listening={"LIVE       Air 0","1090 MHz","Listening"};
    CHECK(history_screen(s,1,0)==NO_CARD && !memcmp(s,listening,sizeof(s)));
    Card lab=packet(1,1000,"8dabc123203010b1cb3820de9e79");
    history_add(lab);
    /* Ages are measured from reception. The last glyph occupies the
     * rightmost cell of the OLED's first row, including on a unit change. */
    const struct { int elapsed; const char *top; } ages[]={
        {23000,"LIVE         23s"}, {59999,"LIVE         59s"},
        {60000,"LIVE          1m"}, {240000,"LIVE          4m"},
        {3600000,"LIVE          1h"}, {86400000,"LIVE          1d"}
    };
    for (unsigned i=0; i<sizeof(ages)/sizeof(ages[0]); i++) {
        CHECK(history_screen(s,1,lab.ms+ages[i].elapsed)==AIRCRAFT_CARD);
        CHECK(!strcmp(s[0],ages[i].top) && !strcmp(s[1],"LAB123"));
    }
    history_add(packet(1,2000,"8d48c12b990ca5b098403ec9dd45"));
    history_add(packet(1,3000,"8d3200375807047a97d9af1c8831"));
    history_add(packet(1,2500,"8d320037ea01a848015e00441521"));
    CHECK(history_screen(s,1,3200)==AIRCRAFT_CARD);
    CHECK(!strcmp(s[1],"320037") && !strcmp(s[3],"200 ft") && !strcmp(s[5],"Sel 800 ft"));
    CHECK(!strcmp(s[0],"LIVE          0s") && strstr(s[7],"Air 3"));
    history_older();
    history_screen(s,1,3200);
    CHECK(!strcmp(s[3],"Sel 800 ft") && !strcmp(s[0],"H2/4          0s"));
    history_screen(s,1,25500);
    CHECK(!strcmp(s[0],"H2/4         23s"));
    for (int i=0; i<CARDS+1; i++) { lab.ms=4000+i; history_add(lab); }
    history_older();
    history_screen(s,1,5000);
    CHECK(!strcmp(s[1],"48C12B") && !strcmp(s[3],"420 kt") && !strcmp(s[5],"Track 203 deg"));
    history_live();
    history_screen(s,2,0);
    CHECK(!strcmp(s[1],"LAB123") && !strcmp(s[0],"LIVE       saved"));
    lab.word[13]^=1;
    CHECK(!card_valid(&lab));

    /* The second half of the airborne position was decoded before the
     * first. Once both arrive, the display can put them back in time order. */
    history_set_boot(2);
    Card target=packet(2,1010,"8d4baa79ea09d820013c08097f65");
    Card name=packet(2,3440,"8d4baa7923508674103820b6ad06");
    Card speed=packet(2,3685,"8d4baa79990c8d821830023908f2");
    Card odd=packet(2,4575,"8d4baa79580db4913fe0a3be306b");
    Card even=packet(2,5695,"8d4baa79580db0fbd3f6267aeda9");
    history_add(target);
    history_add(name);
    history_add(speed);
    history_add(even);
    history_add(odd);
    CHECK(history_screen(s,2,6000)==AIRCRAFT_CARD);
    CHECK(!strcmp(s[1],"THY4DC") && !strcmp(s[3],"1675 ft") && strstr(s[0],"1/3"));
    CHECK(strstr(s[5],"GS ") && strstr(s[6],"Track ") && strstr(s[7],"V/S "));
    /* More packets from this aircraft must not keep restarting page one. */
    target.ms=6500;
    history_add(target);
    CHECK(history_screen(s,2,8999)==AIRCRAFT_CARD && strstr(s[0],"1/3"));
    CHECK(history_screen(s,2,9000)==POSITION_CARD && !strcmp(s[0],"LIVE 2/3      2s"));
    CHECK(!strcmp(s[1],"THY4DC") && !strcmp(s[3],"37.47551 N") && !strcmp(s[5],"15.17177 E"));
    CHECK(history_screen(s,2,12000)==AIRCRAFT_CARD && strstr(s[0],"3/3"));
    CHECK(strstr(s[3],"Sel ") && !strcmp(s[5],"ICAO 4BAA79") && !strcmp(s[6],"TC29 Target"));
    history_older(); /* even */
    history_older(); /* odd: the even report is in the future */
    CHECK(history_screen(s,2,13000)==AIRCRAFT_CARD && strstr(s[0],"1/2"));
    CHECK(history_screen(s,2,16000)==AIRCRAFT_CARD && strstr(s[0],"2/2"));
    history_live();

    /* Put both halves and the identification beyond the 64-card window.
     * Later velocity reports still see the fix until its own time expires. */
    for (int i=0; i<CARDS+2; i++) { speed.ms=7000+i; history_add(speed); }
    CHECK(history_screen(s,2,20000)==AIRCRAFT_CARD && !strcmp(s[1],"THY4DC"));
    CHECK(history_screen(s,2,23000)==POSITION_CARD && !strcmp(s[3],"37.47551 N"));
    speed.ms=70000;
    history_add(speed);
    CHECK(history_screen(s,2,70001)==AIRCRAFT_CARD && !strcmp(s[1],"THY4DC"));
    CHECK(!strstr(s[0],"1/3") && !strstr(s[0],"2/3") && !strstr(s[0],"3/3"));
    speed.ms+=600001;
    history_add(speed);
    CHECK(history_screen(s,2,speed.ms)==AIRCRAFT_CARD && !strcmp(s[1],"4BAA79"));

    /* The same ICAO on a different boot cannot inherit its old fix. */
    history_set_boot(3);
    even.boot=3;
    history_add(even);
    CHECK(history_screen(s,3,6000)==AIRCRAFT_CARD && !strcmp(s[1],"4BAA79"));
    history_add(packet(3,6250,"8d48c12b990ca5b098403ec9dd45"));
    CHECK(history_screen(s,3,8999)==AIRCRAFT_CARD && !strcmp(s[3],"1675 ft"));
    CHECK(history_screen(s,3,9000)==AIRCRAFT_CARD && !strcmp(s[3],"420 kt"));

    /* Longest possible identifier, including an internal space. */
    const char *full="AB 12345";
    uint64_t chars=0;
    for (int i=0; i<8; i++) chars=chars*64+(full[i]&63);
    for (int i=10; i>=5; i--) { name.word[i]=chars&255; chars>>=8; }
    uint32_t parity=modes_crc(name.word,11);
    for (int i=13; i>=11; i--) { name.word[i]=parity&255; parity>>=8; }
    name.boot=3; name.ms=10500;
    history_add(name);
    history_live();
    CHECK(history_screen(s,3,11000)==AIRCRAFT_CARD && !strcmp(s[1],full));
    CHECK(!strcmp(s[0]+14,"0s"));

    history_add(packet(3,12000,"1654455354000669672a44bba1ad"));
    CHECK(history_screen(s,3,35000)==TEST_CARD && !strcmp(s[0],"LIVE         23s"));
    /* Clear from a frozen card. A later position from that same aircraft
     * must start with neither the old name nor the old odd/even partner. */
    history_older();
    history_clear();
    history_older();
    CHECK(history_screen(s,3,36000)==NO_CARD && !memcmp(s,listening,sizeof(s)));
    even.ms=37000;
    history_add(even);
    CHECK(history_screen(s,3,38000)==AIRCRAFT_CARD && !strcmp(s[1],"4BAA79"));
    CHECK(!strstr(s[0],"/") && strstr(s[7],"Air 1"));
    return 0;
}
