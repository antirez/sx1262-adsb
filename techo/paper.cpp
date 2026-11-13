/* T-Echo, DEPG0150BN, SSD1681. The chip has two RAM bits per pixel. We
 * use them to name four things the ink can do during an update. The MCU
 * keeps a copy of the page so it knows which pixels need to move. */
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <initializer_list>
#include "board.h"

enum { SIDE=200, STRIDE=SIDE/8, CS=30, DC=28, RESET_PIN=2, BUSY=3,
       PAPER_SPI_HZ=8000000, BLACK=0, WHITE=1 };
static SPIClass spi(NRF_SPIM2,38,31,29);
static GFXcanvas1 panel(SIDE,SIDE);        /* White is 1 in controller RAM. */
static uint8_t previous[SIDE*STRIDE];
static bool known;                         /* Shadow describes the glass. */
static bool was_scope;                     /* Last page actually on the glass. */
struct Window { int left, right, top, bottom; }; /* x in bytes, y in rows, inclusive */
static const Window whole={0,STRIDE-1,0,SIDE-1};

enum Refresh { FACTORY, REPAINT, GRAPH };
static Refresh loaded=FACTORY;
struct Stats { unsigned full, repaint, fast, skipped, errors, bytes, elapsed, drive; };
static Stats stats;

static void command(uint8_t op, std::initializer_list<uint8_t> args={}) {
    digitalWrite(DC,LOW); digitalWrite(CS,LOW);
    spi.transfer(op);
    digitalWrite(DC,HIGH);
    for (uint8_t arg : args) spi.transfer(arg);
    digitalWrite(CS,HIGH);
}

/* The address window restricts RAM writes. It does not by itself keep the
 * rest of the glass quiet: both bits must select an idle waveform there. */
static void window(Window w) {
    command(0x44,{(uint8_t)w.left,(uint8_t)w.right});
    command(0x45,{(uint8_t)w.top,0,(uint8_t)w.bottom,0});
}
/* A normal update starts and ends with zeros in both controller memories.
 * For the update itself, encode these four instructions, one per pixel:
 *
 *     00  stay put                 01  erase old ink
 *     10  draw new ink             11  clean nearby white paper
 *
 * At rest a black pixel gets no pulse. Setting 11 on the white pixels of
 * a changed byte washes the gaps between strokes. The other bytes of the
 * same line stay still. On entering or leaving the graph, wash all whites
 * once to remove old traces. The scrolling graph needs only 01 and 10. */
enum Plane { TARGET, BEFORE, AFTER, QUIET };
static uint8_t plane_byte(Plane bank, uint8_t old, uint8_t now, bool text, bool wash_all) {
    if (bank==TARGET) return now;      /* Factory LUT uses ordinary image bits. */
    if (bank==QUIET) return 0;
    uint8_t changed=old^now;
    uint8_t strokes=(bank==BEFORE ? old : now) & changed;
    uint8_t paper=(text && (changed || wash_all)) ? (old & now) : 0;
    return strokes | paper;
}

static void plane(uint8_t op, Plane bank, Window w, const uint8_t *image,
                  bool text=false, bool wash_all=false) {
    command(0x4e,{(uint8_t)w.left});
    command(0x4f,{(uint8_t)w.top,0});
    command(op);
    digitalWrite(CS,LOW);
    /* Give SPI several rows per DMA transfer. A full image is 200 rows. */
    uint8_t block[STRIDE*8];
    for (int row=w.top;row<=w.bottom;row+=8) {
        int count=0, end=min(row+7,w.bottom);
        for (int y=row;y<=end;y++) for (int x=w.left;x<=w.right;x++) {
            int i=y*STRIDE+x;
            block[count++]=plane_byte(bank,previous[i],image[i],text,wash_all);
        }
        spi.transfer(block,nullptr,count);
    }
    digitalWrite(CS,HIGH);
}

static bool wait_level(bool high, unsigned timeout_ms) {
    int64_t start=uptime_ms();
    while ((digitalRead(BUSY)!=0)!=high) {
        if (uptime_ms()-start>=timeout_ms) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool reset_panel(void) {
    spi.begin();
    pinMode(CS,OUTPUT); digitalWrite(CS,HIGH);
    pinMode(DC,OUTPUT); pinMode(BUSY,INPUT); pinMode(RESET_PIN,OUTPUT);
    digitalWrite(RESET_PIN,LOW); delay(10); digitalWrite(RESET_PIN,HIGH); delay(10);
    if (!wait_level(false,3000)) return false;
    spi.beginTransaction(SPISettings(PAPER_SPI_HZ,MSBFIRST,SPI_MODE0));
    command(0x12); /* Software reset. */
    delay(10);
    bool ok=wait_level(false,3000);
    if (ok) {
        command(0x01,{SIDE-1,0,0});
        command(0x11,{3});        /* Increment x, then y. */
        command(0x18,{0x80});     /* Internal temperature sensor for full refresh. */
    }
    spi.endTransaction();
    return ok;
}

/* There are twelve timing groups per LUT row. Only group zero is used.
 * Each pair of bits in its byte chooses a voltage for one phase, from
 * A (bits 7:6) to D (bits 1:0). 01 moves the ink toward black, 10 toward
 * white. Row 00, VCOM and the border stay quiet throughout. */
static void waveform(Refresh mode) {
    uint8_t lut[153]={};
    if (mode==GRAPH) {
        lut[1*12]=0x80;             /* 01: white */
        lut[2*12]=0x40;             /* 10: black */
        lut[60]=6;
    } else {
        lut[1*12]=0xa0;             /* 01: white throughout */
        lut[2*12]=0x50;             /* 10: black throughout */
        lut[3*12]=0x60;             /* 11: black, then white */
        lut[60]=lut[61]=4;
    }
    memset(lut+144,0x22,6);
    command(0x32);
    digitalWrite(CS,LOW); spi.transfer(lut,nullptr,sizeof(lut)); digitalWrite(CS,HIGH);
    command(0x3f,{0x02});
    command(0x03,{0x17});
    command(0x04,{0x41,0xb0,0x32});
    command(0x2c,{0x28});
    command(0x3c,{0x80});           /* Keep the border quiet. */
    /* Datasheet 0Ch: default drive strengths, 10 ms per booster stage. */
    command(0x0c,{0x8b,0x9c,0x96,0x00});
    loaded=mode;
}

static Window differences(const uint8_t *image) {
    Window w={STRIDE,-1,SIDE,-1};
    for (int y=0;y<SIDE;y++) for (int x=0;x<STRIDE;x++) {
        int i=y*STRIDE+x;
        if (image[i]==previous[i]) continue;
        if (x<w.left) w.left=x;
        if (x>w.right) w.right=x;
        if (y<w.top) w.top=y;
        w.bottom=y;
    }
    return w;
}

static int signal_y(unsigned code) {
    if (code<80) code=80;
    if (code>240) code=240;
    return 53+(int)(code-80)*80/160;
}
static void draw(Screen rows,CardView card,const ScopeView *v) {
    panel.fillScreen(WHITE);
    panel.fillRect(0,0,200,20,BLACK);
    bool plane=card==AIRCRAFT_CARD || card==POSITION_CARD;
    for (int y=0;y<LINES;y++) {
        if (!rows[y][0]) continue;
        bool big=(plane && (y==1 || y==3)) || (card==POSITION_CARD && y==5);
        int n=strlen(rows[y]), size=big ? min(4,200/(6*n)) : 2;
        panel.setTextSize(size); panel.setTextColor(y?BLACK:WHITE);
        panel.setCursor(3,3+y*25); panel.print(rows[y]);
    }
    if (v) {
        for (unsigned i=0;i<SCOPE_WIDTH;i++) {
            int x=2+i*195/(SCOPE_WIDTH-1);
            if (v->zoom || v->valid[i]) {
                int y=signal_y(v->zoom?v->wave[i]:v->mean[i]);
                if (i && (v->zoom || v->valid[i-1]))
                    panel.drawLine(2+(i-1)*195/(SCOPE_WIDTH-1),signal_y(v->zoom?v->wave[i-1]:v->mean[i-1]),x,y,BLACK);
                if (!v->zoom) {
                    panel.drawPixel(x,signal_y(v->peak[i]),BLACK);
                    if (v->burst_peak[i]!=255) panel.drawLine(x,signal_y(v->burst_peak[i]),x,y,BLACK);
                }
            }
        }
    }
}

bool board_paint(Screen rows, CardView card, const ScopeView *scope, bool clean) {
    bool graph=scope!=nullptr;
    Refresh mode=(!known || clean) ? FACTORY : (graph && was_scope ? GRAPH : REPAINT);
    int64_t begin=uptime_ms();
    if (!known) { panel.setRotation(3); panel.setTextWrap(false); }
    uint8_t *image=panel.getBuffer();
    if (!image) return false;
    draw(rows,card,scope);
    bool whole_page=mode==FACTORY || graph!=was_scope;
    Window w=whole_page ? whole : differences(image);
    if (w.right<0) {
        taskENTER_CRITICAL(); stats.skipped++; taskEXIT_CRITICAL();
        return true;
    }

    unsigned drive=0;
    bool ok=known || reset_panel();
    if (ok) {
        spi.beginTransaction(SPISettings(PAPER_SPI_HZ,MSBFIRST,SPI_MODE0));
        window(w);
        if (mode==FACTORY) {
            command(0x3c,{0x05});  /* Factory waveform also cleans the border. */
            plane(0x26,TARGET,w,image);
            plane(0x24,TARGET,w,image);
            command(0x0c,{0x8b,0x9c,0x96,0x0f}); /* Factory soft start. */
            loaded=FACTORY;
        } else {
            if (loaded!=mode) waveform(mode);
            plane(0x26,BEFORE,w,image,mode==REPAINT,whole_page);
            plane(0x24,AFTER,w,image,mode==REPAINT,whole_page);
        }
        command(0x22,{(uint8_t)(mode==FACTORY?0xf7:0xcf)});
        int64_t started=uptime_ms();
        command(0x20);            /* Each sequence turns off analog drive when done. */
        ok=wait_level(true,40) && wait_level(false,6000);
        drive=(unsigned)(uptime_ms()-started);
        if (ok) {
            /* Retire every nonzero instruction before the next refresh.
             * The next window can be smaller or somewhere else entirely. */
            window(w);
            plane(0x26,QUIET,w,image);
            plane(0x24,QUIET,w,image);
            memcpy(previous,image,sizeof(previous));
            was_scope=graph;
        }
        spi.endTransaction();
    }
    known=ok; /* After an interrupted update, recover with a full refresh. */
    unsigned elapsed=(unsigned)(uptime_ms()-begin);
    taskENTER_CRITICAL();
    if (!ok) stats.errors++;
    else if (mode==FACTORY) stats.full++;
    else if (mode==REPAINT) stats.repaint++;
    else stats.fast++;
    stats.bytes=(w.right-w.left+1)*(w.bottom-w.top+1);
    stats.drive=drive; stats.elapsed=elapsed;
    taskEXIT_CRITICAL();
    return ok;
}

void board_paper_status(void) {
    taskENTER_CRITICAL(); Stats s=stats; taskEXIT_CRITICAL();
    char msg[176];
    snprintf(msg,sizeof(msg),"SXEPD full=%u repaint=%u fast=%u skipped=%u errors=%u window_bytes=%u busy_ms=%u update_ms=%u\n",
             s.full,s.repaint,s.fast,s.skipped,s.errors,s.bytes,s.drive,s.elapsed);
    board_print(msg);
}
