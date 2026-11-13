/* Receive-only SX1262 driver. The MCU moves completed windows over SPI.
 * The SX1262 clocks every RSSI sample itself. */
#include "radio_bus.h"
#include "tuning.h"
#include <string.h>

enum { STATUS=0x07cd, RAM=0x0400, TEST_INPUT=0x06c0, LIMIT=0x06c1, CURSOR=0x06c3 };
static _Alignas(4) uint8_t tx[RADIO_SPI_BYTES], rx[RADIO_SPI_BYTES];
#define TRY(call) do { int err=(call); if (err) return err; } while (0)

static int exchange(size_t n) { return radio_exchange(tx,rx,n); }

static int command(const uint8_t *bytes, size_t n) {
    memcpy(tx,bytes,n);
    return exchange(n);
}
#define CMD(...) command((const uint8_t[]){__VA_ARGS__},sizeof((uint8_t[]){__VA_ARGS__}))

static int write_reg(unsigned reg, const uint8_t *bytes, size_t n) {
    tx[0] = 0x0d;
    tx[1] = reg>>8;
    tx[2] = reg;
    memcpy(tx+3,bytes,n);
    return exchange(3+n);
}
#define WRITE(reg,...) write_reg(reg,(const uint8_t[]){__VA_ARGS__},sizeof((uint8_t[]){__VA_ARGS__}))

static int read_reg(unsigned reg, uint8_t *out, size_t n) {
    tx[0] = 0x1d;
    tx[1] = reg>>8;
    tx[2] = reg;
    memset(tx+3,0,n+1);
    TRY(exchange(4+n));
    memcpy(out,rx+4,n);
    return 0;
}

static int load_sampler(bool test) {
    uint8_t before[15], after[15];
    TRY(CMD(0x80,0));
    TRY(read_reg(0x0320,before,sizeof(before)));
    TRY(WRITE(0x0610,0x10));
    for (unsigned i = 0; i < SCAN_WORDS; i++) {
        uint32_t w = sampler_word(i,test);
        TRY(WRITE(0x8000+i*4,w>>24,w>>16,w>>8,w));
    }
    TRY(WRITE(0x0610,0));
    TRY(CMD(0xd9));
    radio_delay_us(5000);
    TRY(read_reg(0x0320,after,sizeof(after)));
    return memcmp(before,after,sizeof(after)) ? 0 : RADIO_BAD_RESPONSE;
}

static int tune(uint32_t hz) {
    uint32_t pll = (((uint64_t)hz<<25)+16000000)/32000000;
    TRY(CMD(0x80,1));
    TRY(CMD(0x86,pll>>24,pll>>16,pll>>8,pll));
    TRY(CMD(0x82,0xff,0xff,0xff));  /* Continuous RX. */
    radio_delay_us(800);
    TRY(CMD(0xc0,0));
    if (((rx[1]>>4)&7)!=5) return RADIO_BAD_STATE;
    TRY(CMD(0x17,0,0,0));
    return (rx[2]||rx[3]) ? RADIO_BAD_RESPONSE : 0;
}

int radio_start(bool test) {
    uint8_t value;
    radio_reset_pin(false);
    radio_delay_us(500);
    radio_reset_pin(true);
    radio_delay_us(1000);
    TRY(CMD(0x80,0));                  /* Standby on RC oscillator. */
    TRY(CMD(0x97,radio_tcxo(),0,1,0x40));         /* Enable the board's temperature-compensated oscillator. */
    TRY(CMD(0x9d,1));                  /* DIO2 drives the antenna switch. */
    TRY(CMD(0x07,0,0));
    TRY(CMD(0x89,0x7f));               /* Calibrate analog blocks and PLL. */
    TRY(CMD(0x98,0xe1,0xe9));
    TRY(CMD(0x8a,0));                  /* GFSK supplies the analog receive path. */
    TRY(CMD(0x8b,0,0x10,0,0,9,0,0xcc,0xcd));
    TRY(load_sampler(test));
    TRY(CMD(0x80,1));
    TRY(CMD(0x8b,0,8,0,0,8,0,0xcc,0xcd));
    TRY(read_reg(0x0805,&value,1));
    TRY(WRITE(0x0805,value|7));
    TRY(read_reg(0x0806,&value,1));
    TRY(WRITE(0x0806,value|0x32));
    TRY(WRITE(0x0818,1));
    TRY(WRITE(0x088f,0xfa,0xaa,0xac));
    TRY(CMD(0x8c,0,8,0,1,0,0,8,1,0));
    TRY(CMD(0x8b,0,8,0,0,0,0,0xcc,0xcd));
    TRY(CMD(0x8f,0xf8,0xf8));
    TRY(WRITE(TEST_INPUT,0x2d,0xd4,0xa7,0x1b,0x59,0x90,0xe3,0x6c));
    TRY(CMD(0x8c,0,64,4,64,0,0,8,1,0));
    TRY(tune(1090000000));
    TRY(CMD(0x11,0,0));
    if (rx[2]!=0) return RADIO_BAD_STATE;
    TRY(CMD(0x80,1));
    TRY(WRITE(0x08ac,0x96));
    return radio_tune(DEFAULT_HZ);
}

int radio_tune(uint32_t hz) {
    uint8_t value;
    TRY(CMD(0x80,1));
    TRY(CMD(0x07,0,0));   /* Clear any PLL error from the previous attempt. */
    TRY(CMD(0x8b,0,8,0,0,8,0,0xcc,0xcd));
    /* The measured passband lies above the programmed center. Start the
     * 1 MHz front end first, then select the fast RSSI update clock. */
    TRY(tune(hz));
    TRY(WRITE(0x0807,0));
    TRY(read_reg(0x0807,&value,1));
    if (value) return RADIO_BAD_RESPONSE;
    return WRITE(0x089b,0);
}

int radio_rssi(uint8_t *code) {
    return read_reg(0x08be,code,1);
}

int radio_state(uint8_t *state) {
    return read_reg(STATUS,state,1);
}

static int wait_state(uint8_t wanted, unsigned timeout_us) {
    uint32_t start=radio_clock_us();
    do {
        uint8_t state;
        TRY(radio_state(&state));
        if (state == wanted) return 0;
    } while ((uint32_t)(radio_clock_us()-start)<timeout_us);
    return RADIO_TIMEOUT;
}

static int threshold(uint8_t level) {
    unsigned total = AVERAGE*level;
    return WRITE(LIMIT,total,total>>8);
}

int radio_arm(uint8_t level) {
    TRY(threshold(0));
    TRY(WRITE(STATUS,0));
    TRY(wait_state(0,25000));
    TRY(CMD(0x9b,0,1,0));
    TRY(wait_state(SCAN_ARMED,25000));
    radio_delay_us(160);  /* Fill the pretrigger history with fresh readings. */
    return threshold(level);
}

int radio_read(uint8_t raw[SAMPLES]) {
    uint8_t ring[SAMPLES], info[2];
    TRY(read_reg(RAM,ring,sizeof(ring)));
    TRY(read_reg(CURSOR,info,sizeof(info)));
    ring[0] = info[1];
    unsigned first = (uint8_t)(info[0]-HISTORY);
    memcpy(raw,ring+first,SAMPLES-first);
    memcpy(raw+SAMPLES-first,ring,first);
    return 0;
}

int radio_finish(void) {
    TRY(threshold(255));
    return wait_state(SCAN_COMPLETE,200000);
}

int radio_calibrate(uint8_t *floor, uint8_t *level) {
    unsigned hist[256] = {0};
    uint8_t raw[SAMPLES];
    for (int i = 0; i < 3; i++) {
        TRY(radio_arm(0));
        TRY(radio_finish());
        TRY(radio_read(raw));
        for (int j = 0; j < SAMPLES; j++) hist[raw[j]]++;
    }
    int median = 0;
    unsigned total = hist[0];
    while (total <= 3*SAMPLES/2) total += hist[++median];
    *floor = median;
    *level = median > 5 ? median-5 : 1;  /* Half-dB codes: 2.5 dB above quiet. */
    return 0;
}

int radio_self_test(void) {
    TRY(radio_start(true));
    const uint8_t steps[][3] = {{200,165,188},{189,188,188},{172,163,167}};
    for (unsigned i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        uint8_t high = steps[i][0], low = steps[i][1], limit = steps[i][2];
        TRY(WRITE(TEST_INPUT,high));
        TRY(radio_arm(limit));
        radio_delay_us(1000);
        TRY(wait_state(SCAN_ARMED,1000));
        TRY(WRITE(TEST_INPUT,low));
        TRY(wait_state(SCAN_COMPLETE,200000));
        uint8_t raw[SAMPLES];
        TRY(radio_read(raw));
        int replaced = (AVERAGE*(high-limit)+high-low-1)/(high-low);
        for (int j = 0; j < SAMPLES; j++)
            if (raw[j] != (j < HISTORY-replaced ? high : low)) return RADIO_BAD_RESPONSE;
    }
    /* Already strong when armed. Even the oldest byte must come from the
     * new capture. A stale prefix can look remarkably like a preamble. */
    const uint8_t strong[] = {146,163};
    for (unsigned i = 0; i < sizeof(strong); i++) {
        uint8_t raw[SAMPLES];
        TRY(WRITE(TEST_INPUT,strong[i]));
        TRY(radio_arm(180));
        TRY(wait_state(SCAN_COMPLETE,200000));
        TRY(radio_read(raw));
        for (int j = 0; j < SAMPLES; j++)
            if (raw[j] != strong[i]) return RADIO_BAD_RESPONSE;
    }
    return 0;
}
