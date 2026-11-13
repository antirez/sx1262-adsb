/* The SSD1306 has eight pages of 128 bytes. Each byte is a vertical column
 * of eight pixels, exactly the layout of the font, so text needs no canvas
 * library. Send a new screen only when something on it actually changed. */
#include "display.h"
#include "font8x8.h"
#include "driver/i2c.h"
#include <stdio.h>
#include <string.h>

static const i2c_port_t bus = I2C_NUM_0;
static uint8_t image[1+128*8], previous[128*8];
static bool painted;

static esp_err_t send(const uint8_t *bytes, size_t count) {
    return i2c_master_write_to_device(bus,0x3c,bytes,count,pdMS_TO_TICKS(100));
}

esp_err_t display_init(void) {
    i2c_config_t cfg = {.mode=I2C_MODE_MASTER,.sda_io_num=18,.scl_io_num=17,
        .sda_pullup_en=true,.scl_pullup_en=true,.master.clk_speed=400000};
    esp_err_t err = i2c_param_config(bus,&cfg);
    if (err==ESP_OK) err = i2c_driver_install(bus,cfg.mode,0,0,0);
    if (err!=ESP_OK) return err;
    const uint8_t init[] = {0,0xae,0x20,0,0x40,0xa1,0xc8,0xa8,63,0xd3,0,
        0xda,0x12,0xd5,0x80,0xd9,0xf1,0xdb,0x30,0x81,0xcf,0xa4,0xa6,0x8d,0x14,0xaf};
    return send(init,sizeof(init));
}

static void draw_trace(const uint8_t *raw) {
    int lo=255, hi=0;
    for (int i=0; i<SAMPLES; i++) {
        if (raw[i]<lo) lo=raw[i];
        if (raw[i]>hi) hi=raw[i];
    }
    int previous_y=-1;
    for (int x=0; x<128; x++) {
        int code = raw[2*x]<raw[2*x+1]?raw[2*x]:raw[2*x+1];
        int y = 63-(hi==lo ? 0 : 14*(hi-code)/(hi-lo));
        int a = previous_y<0 || previous_y>y ? y : previous_y;
        int b = previous_y>y ? previous_y : y;
        for (int row=a; row<=b; row++) image[1+128*(row/8)+x] |= 1u<<(row%8);
        previous_y=y;
    }
}

static int scope_y(unsigned code) {
    /* Fixed scale: -40 dBm at the top, -120 dBm at the bottom. */
    if (code<80) code=80;
    if (code>240) code=240;
    return 16+(code-80)*31/160;
}

static void pixel(unsigned x, unsigned y) {
    image[1+128*(y/8)+x] |= 1u<<(y%8);
}

static void vertical(unsigned x, int from, int to) {
    for (int y=(from<to?from:to); y<=(from>to?from:to); y++) pixel(x,y);
}

static void draw_scope(const ScopeView *v) {
    for (unsigned x=0; x<SCOPE_WIDTH; x++) {
        if (x%8==0) {
            for (int db=60; db<=100; db+=20) pixel(x,scope_y(2*db));
        }
        if (v->zoom) {
            int y=scope_y(v->wave[x]), last=x?scope_y(v->wave[x-1]):y;
            vertical(x,last,y);
            continue;
        }
        if (!v->valid[x]) continue;
        int y=scope_y(v->mean[x]), last=x && v->valid[x-1]?scope_y(v->mean[x-1]):y;
        vertical(x,last,y);
        pixel(x,scope_y(v->peak[x]));
        if (v->burst_peak[x]!=255)
            vertical(x,scope_y(v->burst_peak[x]),y);
    }
}

/* A callsign is at most eight characters. Doubling each pixel gives it
 * exactly the width of the OLED. Longer readings use all 128 columns and
 * keep the same 16-pixel height. No letter or digit is clipped. */
static void tall_line(unsigned page, const char *text) {
    unsigned columns=8*strlen(text), width=columns>64 ? 128 : columns*2;
    uint8_t *top=image+1+128*page;
    for (unsigned x=0; x<width; x++) {
        unsigned src=x*columns/width, ch=(unsigned char)text[src/8];
        if (ch<32 || ch>126) ch='?';
        uint8_t glyph=font_petme128_8x8[(ch-32)*8+src%8];
        unsigned bits=0;
        for (unsigned i=0; i<8; i++) if (glyph&(1u<<i)) bits|=3u<<(2*i);
        top[x]=bits&255;
        top[128+x]=bits>>8;
    }
}

esp_err_t display_draw(Screen rows, CardView card, const uint8_t *trace, const ScopeView *scope) {
    image[0]=0x40;  /* The remaining bytes are display RAM, not commands. */
    memset(image+1,0,sizeof(previous));
    bool aircraft=card==AIRCRAFT_CARD || card==POSITION_CARD;
    /* Page zero carries status and packet age. The callsign has the full
     * width of the next two pages, even with an eight-character name. */
    memset(image+1,0xff,128);
    for (int y=0; y<LINES; y++) {
        if ((aircraft && (y==1 || y==3)) || (card==POSITION_CARD && y==5)) {
            tall_line(y,rows[y]);
            y++;  /* This line owns two SSD1306 pages. */
            continue;
        }
        for (int x=0; x<COLUMNS && rows[y][x]; x++) {
            unsigned ch=(unsigned char)rows[y][x];
            if (ch<32 || ch>126) ch='?';
            for (int k=0; k<8; k++) {
                uint8_t glyph=font_petme128_8x8[8*(ch-32)+k];
                image[1+128*y+8*x+k]=y==0 ? (uint8_t)~glyph : glyph;
            }
        }
    }
    if (trace) draw_trace(trace);
    if (scope) draw_scope(scope);
    if (painted && !memcmp(previous,image+1,sizeof(previous))) return ESP_OK;
    const uint8_t address[] = {0,0x21,0,127,0x22,0,7};
    esp_err_t err=send(address,sizeof(address));
    if (err==ESP_OK) err=send(image,sizeof(image));
    if (err==ESP_OK) { memcpy(previous,image+1,sizeof(previous)); painted=true; }
    return err;
}

/* The USB console can read exactly what was last sent to the OLED. */
void display_dump(void) {
    if (painted) {
        static const char hex[]="0123456789abcdef";
        for (unsigned y=0; y<8; y++) {
            char row[257];
            for (unsigned x=0; x<128; x++) {
                uint8_t v=previous[128*y+x];
                row[2*x]=hex[v>>4];
                row[2*x+1]=hex[v&15];
            }
            row[256]=0;
            printf("SXPIX %u %s\n",y,row);
        }
    }
    puts("SXSCREEN END");
}
