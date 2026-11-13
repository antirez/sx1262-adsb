/* A card answers "what did I know about this aircraft at this instant?"
 * Replay its packets up to the selected time. A newer speed must never
 * appear on an older card, and a callsign can outlive a speed reading. */
#include "history.h"
#include "cpr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char name[9], altitude[32], speed[32], angle[17], rate[17], selected[17], label[17];
    bool motion, height, vertical;
} Fields;

typedef struct {
    uint32_t boot, address;
    int64_t seen, height_ms, motion_ms, rate_ms, selected_ms;
    Fields fields;
    CprTrack cpr;
} Flight;

typedef struct {
    Card cards[CARDS];
    unsigned count;
    /* State immediately before the oldest card. A callsign or the other
     * half of a position pair may have scrolled off the packet list. */
    Flight prior[CARDS];
    unsigned prior_count, replace;
} History;
static History live, frozen;
static bool browsing;
static unsigned back;
static uint32_t current_boot;
static bool dirty=true, restart_pages=true;
static void remember_evicted(History *h, const Card *c);

void history_set_boot(uint32_t boot) { current_boot=boot; }

bool card_valid(const Card *c) {
    int df = c->word[0]>>3;
    return ((df==11 && c->length==7) || (df==17 && c->length==14) ||
            (df==2 && c->length==14 && !memcmp(c->word+1,"TEST",4))) &&
           modes_crc(c->word,c->length)==0;
}

void history_add(Card c) {
    if (!card_valid(&c)) return;
    for (unsigned j=0; j<live.count; j++) {
        const Card *old=&live.cards[j];
        if (old->boot==c.boot && old->ms==c.ms && old->length==c.length &&
            !memcmp(old->word,c.word,c.length)) return;
    }
    unsigned i = live.count;
    if (c.boot!=current_boot) {
        /* Imported cards can arrive while the current boot is already live. */
        while (i && live.cards[i-1].boot==current_boot) i--;
        for (unsigned j=0; j<i; j++) {
            if (live.cards[j].boot!=c.boot) continue;
            while (j<i && live.cards[j].boot==c.boot) j++;
            i=j;
            break;
        }
    }
    while (i && live.cards[i-1].boot==c.boot && live.cards[i-1].ms>c.ms) i--;
    if (live.count == CARDS) {
        if (!i) {
            /* A deferred decode can predate every card still on screen. */
            remember_evicted(&live,&c);
            if (!browsing) dirty=true;
            return;
        }
        remember_evicted(&live,&live.cards[0]);
        memmove(live.cards,live.cards+1,(--i)*sizeof(c));
    } else {
        memmove(live.cards+i+1,live.cards+i,(live.count-i)*sizeof(c));
        live.count++;
    }
    live.cards[i] = c;
    if (!browsing) dirty=true;
}

void history_older(void) {
    if (!live.count) return;
    if (browsing) back = (back+1)%frozen.count;
    else {
        frozen = live;
        browsing = true;
        back = frozen.count>1;
    }
    dirty=restart_pages=true;
}

void history_live(void) { browsing=false; back=0; dirty=restart_pages=true; }

void history_clear(void) {
    memset(&live,0,sizeof(live));
    memset(&frozen,0,sizeof(frozen));
    history_live();
}

static int gray(unsigned code, const int *positions, unsigned n) {
    int number = 0, bit = 0;
    for (unsigned i = 0; i < n; i++) {
        bit ^= (code>>positions[i])&1;
        number = number*2+bit;
    }
    return number;
}

static bool altitude(unsigned code, int *feet) {
    if (!code) return false;
    if (code&16) { *feet = 25*((code>>5)*16+(code&15))-1000; return true; }
    /* Older transponders use Gillham altitude, two interleaved Gray codes. */
    int fine = gray(code,(int[]){11,9,7},3);
    if (fine==0 || fine==5 || fine==6) return false;
    if (fine==7) fine=5;
    int coarse = gray(code,(int[]){2,0,10,8,6,5,3,1},8);
    *feet = 500*coarse+100*((coarse&1)?6-fine:fine)-1300;
    return true;
}

static int degrees(double angle) { return ((int)lrint(angle)+360)%360; }

static void surface(Fields *f, const uint8_t *w) {
    int movement = ((w[4]&7)<<4)|(w[5]>>4);
    static const struct { int first; double base,step; } ranges[] = {
        {109,100,5},{94,70,2},{39,15,1},{13,2,.5},{9,1,.25},{2,.125,.125},{1,0,0}};
    if (movement==124) strcpy(f->speed,"GS >=175 kt");
    for (unsigned i = 0; movement>0 && movement<124 && i<7; i++) {
        if (movement < ranges[i].first) continue;
        double knots = ranges[i].base+(movement-ranges[i].first)*ranges[i].step;
        snprintf(f->speed,sizeof(f->speed),"GS %g kt",knots);
        break;
    }
    if (w[5]&8) snprintf(f->angle,sizeof(f->angle),"Track %03d deg",
            degrees((((w[5]&7)<<4)|(w[6]>>4))*360.0/128));
    strcpy(f->altitude,"On ground");
    f->motion = f->height = f->vertical = true;
}

static void velocity(Fields *f, const uint8_t *w) {
    int sub = w[4]&7, scale = sub==2 || sub==4 ? 4 : 1;
    int ew = ((w[5]&3)<<8)|w[6], ns = ((w[7]&127)<<3)|(w[8]>>5);
    if (sub<1 || sub>4) return;
    f->motion = f->vertical = true;
    if (sub<=2 && ew && ns) {
        int east = (ew-1)*scale*((w[5]&4)?-1:1);
        int north = (ns-1)*scale*((w[7]&128)?-1:1);
        snprintf(f->speed,sizeof(f->speed),"GS %ld kt",lrint(hypot(east,north)));
        if (east || north) snprintf(f->angle,sizeof(f->angle),"Track %03d deg",
            degrees(atan2(east,north)*180/3.141592653589793));
    } else if (sub>=3) {
        if (ns) snprintf(f->speed,sizeof(f->speed),"%s %d kt",(w[7]&128)?"TAS":"IAS",(ns-1)*scale);
        if (w[5]&4) snprintf(f->angle,sizeof(f->angle),"Hdg %03d deg",degrees(ew*360.0/1024));
    }
    int climb = ((w[8]&7)<<6)|(w[9]>>2);
    if (climb) snprintf(f->rate,sizeof(f->rate),"V/S %+d fpm",(climb-1)*64*((w[8]&8)?-1:1));
}

static Fields fields(const Card *c) {
    Fields f = {0};
    const uint8_t *w = c->word;
    const char *label = "Message";
    if (c->length==7) { strcpy(f.label,"DF11 All-call"); return f; }
    int tc = w[4]>>3;
    if (tc>=1 && tc<=4) {
        label = "Callsign";
        uint64_t bits = 0;
        for (int i = 5; i <= 10; i++) bits = bits*256+w[i];
        for (int i = 0; i < 8; i++) {
            int ch = (bits>>(42-6*i))&63;
            if (ch==32) f.name[i]=' ';
            else if (ch>=1 && ch<=26) f.name[i]='A'+ch-1;
            else if (ch>=48 && ch<=57) f.name[i]=ch;
            else { f.name[0]=0; break; }
        }
        for (int i = 7; i>=0 && f.name[i]==' '; i--) f.name[i]=0;
    } else if (tc>=5 && tc<=8) { label="Surface"; surface(&f,w); }
    else if ((tc>=9 && tc<=18) || (tc>=20 && tc<=22)) {
        int code = w[5]*16+(w[6]>>4), feet;
        label = "Position";
        f.height = true;
        if (tc<=18 && altitude(code,&feet)) snprintf(f.altitude,sizeof(f.altitude),"Alt %d ft",feet);
        if (tc>=20 && code) snprintf(f.altitude,sizeof(f.altitude),"GNSS %d m",code);
    } else if (tc==19) { label="Velocity"; velocity(&f,w); }
    else if (tc==29) {
        label = "Target";
        int selected = ((w[5]&127)<<4)|(w[6]>>4);
        if (((w[4]>>1)&3)==1 && selected) {
            snprintf(f.selected,sizeof(f.selected),"Sel %d ft",(selected-1)*32);
        }
    } else if (tc==28) label="Status";
    else if (tc==31) label="Op status";
    else if (!tc) label="No position";
    snprintf(f.label,sizeof(f.label),"TC%d %s",tc,label);
    return f;
}

static uint32_t address(const Card *c) {
    return (c->word[1]<<16)|(c->word[2]<<8)|c->word[3];
}

/* Apply only the fields actually carried by this packet. The selected
 * altitude is a target, so it can coexist with a measured vertical rate. */
static void update_flight(Flight *f, const Card *c) {
    Fields p=fields(c);
    strcpy(f->fields.label,p.label);
    /* Keep a name through a continuous pass. After a long silence the
     * same airframe may have returned under a different flight number. */
    if (c->ms-f->seen>600000) f->fields.name[0]=0;
    if (*p.name) strcpy(f->fields.name,p.name);
    if (p.height) { strcpy(f->fields.altitude,p.altitude); f->height_ms=c->ms; }
    if (p.motion) {
        strcpy(f->fields.speed,p.speed);
        strcpy(f->fields.angle,p.angle);
        f->motion_ms=c->ms;
    }
    if (p.vertical) { strcpy(f->fields.rate,p.rate); f->rate_ms=c->ms; }
    if (*p.selected) { strcpy(f->fields.selected,p.selected); f->selected_ms=c->ms; }
    if (c->length==14) cpr_update(&f->cpr,c->word,c->ms);
    f->seen=c->ms;
}

static Flight *prior_flight(History *h, const Card *c) {
    uint32_t addr=address(c);
    for (unsigned i=0; i<h->prior_count; i++)
        if (h->prior[i].boot==c->boot && h->prior[i].address==addr) return &h->prior[i];
    return NULL;
}

static void remember_evicted(History *h, const Card *c) {
    if (c->word[0]>>3==2) return;
    Flight *f=prior_flight(h,c);
    if (!f) {
        unsigned slot=h->prior_count<CARDS ? h->prior_count++ : h->replace++%CARDS;
        f=&h->prior[slot];
        *f=(Flight){.boot=c->boot,.address=address(c)};
    }
    if (c->ms>=f->seen) update_flight(f,c);
}

/* Rebuild at most 64 packets when the card changes. Even if a packet was
 * decoded late, a browsed card sees exactly the reports known at its time. */
static void flight_at(Flight *f, History *h, unsigned index) {
    const Card *c=&h->cards[index];
    Flight *prior=prior_flight(h,c);
    *f=(Flight){.boot=c->boot,.address=address(c)};
    if (prior && prior->seen<=c->ms) *f=*prior;
    for (unsigned i=0; i<=index; i++) {
        const Card *p=&h->cards[i];
        if (p->boot==c->boot && p->word[0]>>3!=2 && address(p)==f->address && p->ms>=f->seen)
            update_flight(f,p);
    }
}

static unsigned aircraft_count(const History *h) {
    unsigned planes=0;
    for (unsigned i=0; i<h->count; i++) {
        const Card *c=&h->cards[i];
        if (c->word[0]>>3==2) continue;
        unsigned j=0;
        while (j<i && (h->cards[j].word[0]>>3==2 || address(&h->cards[j])!=address(c))) j++;
        if (j==i) planes++;
    }
    return planes;
}

static void status_line(char *row, const char *left, const char *right) {
    size_t n=strlen(right);
    memset(row,' ',COLUMNS);
    memcpy(row,left,strnlen(left,COLUMNS-n-1));
    memcpy(row+COLUMNS-n,right,n);
    row[COLUMNS]=0;
}

static void message_age(char age[12], const Card *c, uint32_t boot, int64_t now) {
    if (boot!=c->boot) { strcpy(age,"saved"); return; }
    unsigned sec=now>c->ms ? (unsigned)((now-c->ms)/1000) : 0;
    if (sec<60) snprintf(age,12,"%us",sec);
    else if (sec<3600) snprintf(age,12,"%um",sec/60);
    else if (sec<86400) snprintf(age,12,"%uh",sec/3600);
    else snprintf(age,12,"%ud",sec/86400);
}

/* The lower 40 pixels fit one large reading and three small lines, or a
 * pair of large coordinates and a caption. Keep the pair on one page. */
enum { DETAIL_ROWS=4 };
typedef struct {
    char line[8][COLUMNS+1], lat[17], lon[17];
    unsigned count;
} Details;

static void detail(Details *d, const char *text) {
    if (*text) snprintf(d->line[d->count++],COLUMNS+1,"%.16s",text);
}

static Details flight_details(const Flight *f) {
    Details d={0};
    const Fields *p=&f->fields;
    if (f->seen-f->height_ms<=60000) detail(&d,p->altitude);
    if (f->seen-f->motion_ms<=60000) { detail(&d,p->speed); detail(&d,p->angle); }
    if (f->seen-f->rate_ms<=60000) detail(&d,p->rate);
    if (f->seen-f->selected_ms<=60000) detail(&d,p->selected);
    if (f->cpr.fixed && f->seen-f->cpr.fix_ms<=60000) {
        snprintf(d.lat,sizeof(d.lat),"%.5f %c",fabs(f->cpr.lat),f->cpr.lat<0?'S':'N');
        snprintf(d.lon,sizeof(d.lon),"%.5f %c",fabs(f->cpr.lon),f->cpr.lon<0?'W':'E');
    }
    if (*p->name) snprintf(d.line[d.count++],COLUMNS+1,"ICAO %06X",(unsigned)f->address);
    detail(&d,p->label);
    return d;
}

static unsigned detail_pages(const Details *d) {
    return (d->count+DETAIL_ROWS-1)/DETAIL_ROWS+(d->lat[0]!=0);
}

CardView history_screen(Screen s, uint32_t boot, int64_t now) {
    static Flight flight;
    static Details d;
    static unsigned planes;
    static int64_t page_start;
    memset(s,0,sizeof(Screen));
    History *h=browsing ? &frozen : &live;
    if (dirty) planes=aircraft_count(h);
    char left[17]="LIVE", right[16];
    if (browsing) snprintf(left,sizeof(left),"H%u/%u",back+1,h->count);
    snprintf(right,sizeof(right),"Air %u",planes);
    if (!h->count) {
        status_line(s[0],left,right);
        strcpy(s[1],"1090 MHz");
        strcpy(s[2],"Listening");
        return NO_CARD;
    }
    unsigned index=h->count-1-back;
    Card *c=&h->cards[index];
    /* Two aircraft can report a fraction of a second apart. Give the eye
     * one turn through the current flight's pages. Its own reports still
     * update the readings. A tap or hold always takes effect immediately. */
    if (!browsing && !restart_pages && now>=page_start &&
        now-page_start<3000*detail_pages(&d) && c->boot==flight.boot &&
        c->word[0]>>3!=2 && address(c)!=flight.address) {
        for (unsigned i=index; i>0; i--) {
            Card *p=&h->cards[i-1];
            if (p->boot==flight.boot && p->word[0]>>3!=2 && address(p)==flight.address) {
                index=i-1; c=p;
                break;
            }
        }
    }
    char age[12];
    message_age(age,c,boot,now);
    if (c->word[0]>>3==2) {
        status_line(s[0],left,age);
        snprintf(s[1],17,"TEST CRC OK %u",c->test_count);
        strcpy(s[2],"DF2   112 bits");
        snprintf(s[3],17,"Seq %u",c->word[5]*256+c->word[6]);
        for (int i=0; i<14; i++) snprintf(s[4+i/7]+2*(i%7),3,"%02X",c->word[i]);
        status_line(s[7],browsing?"Hold live":"Tap older",right);
        dirty=false;
        restart_pages=true;
        return TEST_CARD;
    }

    if (flight.boot!=c->boot || flight.address!=address(c)) dirty=restart_pages=true;
    if (dirty) {
        flight_at(&flight,h,index);
        d=flight_details(&flight);
        dirty=false;
    }
    if (restart_pages || now<page_start) { page_start=now; restart_pages=false; }
    if (*flight.fields.name) strcpy(s[1],flight.fields.name);
    else snprintf(s[1],17,"%06X",(unsigned)flight.address);

    bool position=d.lat[0]!=0;
    unsigned pages=detail_pages(&d);
    unsigned page=(unsigned)(((now-page_start)/3000)%pages);
    if (pages>1) {
        size_t n=strlen(left);
        snprintf(left+n,sizeof(left)-n," %u/%u",page+1,pages);
    }
    status_line(s[0],left,age);
    if (position && page==1) {
        strcpy(s[3],d.lat);
        strcpy(s[5],d.lon);
        strcpy(s[7],"Flight position");
        return POSITION_CARD;
    }
    unsigned start=(page-(position && page>1))*DETAIL_ROWS;
    const char *reading=d.line[start];
    if (!strncmp(reading,"Alt ",4)) reading+=4;
    else if (!strncmp(reading,"GS ",3)) reading+=3;
    strcpy(s[3],reading);
    for (unsigned i=1; i<DETAIL_ROWS && start+i<d.count; i++)
        strcpy(s[4+i],d.line[start+i]);
    if (!*s[7]) status_line(s[7],browsing?"Hold live":"Tap older",right);
    return AIRCRAFT_CARD;
}
