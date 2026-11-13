/* The original T-Echo has independent SPI buses for its radio and e-paper.
 * Pin numbers use the PCA10056 mapping, P1.n = 32+n, as in FreakWAN. */
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_TinyUSB.h>
#include "board.h"
extern "C" {
#include "radio_bus.h"
}

enum { POWER=12, BUTTON=42, VBAT=4, RF_CS=24, RF_RESET=25, RF_BUSY=17,
       LIGHT=43 };
static SPIClass rf(NRF_SPIM3,23,19,22);

void board_begin(void) {
    pinMode(POWER,OUTPUT); digitalWrite(POWER,HIGH);
    pinMode(BUTTON,INPUT_PULLUP);
    pinMode(LIGHT,OUTPUT); digitalWrite(LIGHT,LOW);
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    analogSampleTime(40); /* Give the battery divider time to charge the ADC input. */
    Serial.begin(115200);
}

void board_read_battery(Battery *battery, int64_t now) {
    static int64_t next;
    bool usb=(NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk)!=0;
    if (now<next && usb==(battery->level==BATTERY_USB)) return;
    next=now+30000;

    /* P0.04 sees half the cell voltage. Average four 12-bit readings
     * with a 3 V full scale. While USB powers the charger, this voltage
     * no longer tells us how much charge is left. VBUS has its own sensor. */
    unsigned sum=0;
    for (int i=0;i<4;i++) sum+=analogRead(VBAT);
    battery->mv=(sum*6000+8192)/16384;
    if (usb) { battery->level=BATTERY_USB; return; }
    if (battery->mv<2500 || battery->mv>4350) { battery->level=BATTERY_UNKNOWN; return; }

    /* A bar disappears 30 mV below the voltage that first lit it.
     * Small changes in load should not keep repainting the paper. */
    static const unsigned threshold[BATTERY_BARS]={3500,3700,3850,4050};
    int bars=0;
    while (bars<BATTERY_BARS) {
        unsigned mv=threshold[bars];
        if (battery->level>bars) mv-=30;
        if (battery->mv<mv) break;
        bars++;
    }
    battery->level=bars;
}

void board_bootloader(void) { enterUf2Dfu(); }
bool board_button(void) { return digitalRead(BUTTON)==LOW; }
int board_getchar(void) { return Serial.available() ? Serial.read() : -1; }
void board_print(const char *text) { if (Serial) Serial.print(text); }

int64_t uptime_ms(void) {
    TimeOut_t t;
    vTaskSetTimeOutState(&t);
    uint64_t ticks=((uint64_t)(uint32_t)t.xOverflowCount<<32)|t.xTimeOnEntering;
    return ticks*1000/configTICK_RATE_HZ;
}
int board_free_heap(void) { return dbgHeapFree(); }
uint32_t board_random(void) {
    uint32_t value=0;
    NRF_RNG->TASKS_START=1;
    for (int i=0;i<4;i++) {
        NRF_RNG->EVENTS_VALRDY=0;
        while (!NRF_RNG->EVENTS_VALRDY) {}
        value=(value<<8)|NRF_RNG->VALUE;
    }
    NRF_RNG->TASKS_STOP=1;
    return value;
}
int radio_bus_init(void) {
    pinMode(RF_CS,OUTPUT); digitalWrite(RF_CS,HIGH);
    pinMode(RF_RESET,OUTPUT); digitalWrite(RF_RESET,HIGH);
    pinMode(RF_BUSY,INPUT); rf.begin();
    return 0;
}
int radio_exchange(const uint8_t *tx,uint8_t *rx,size_t n) {
    delayMicroseconds(2);
    uint32_t start=micros();
    while (digitalRead(RF_BUSY)) if ((uint32_t)(micros()-start)>=100000) return RADIO_TIMEOUT;
    rf.beginTransaction(SPISettings(8000000,MSBFIRST,SPI_MODE0));
    digitalWrite(RF_CS,LOW); rf.transfer(tx,rx,n); digitalWrite(RF_CS,HIGH);
    rf.endTransaction();
    return 0;
}
void radio_reset_pin(bool high) { digitalWrite(RF_RESET,high); }
void radio_delay_us(unsigned us) { delayMicroseconds(us); }
uint32_t radio_clock_us(void) { return micros(); }
unsigned radio_tcxo(void) { return 0; } /* 1.6 V on DIO3, as in FreakWAN. */
