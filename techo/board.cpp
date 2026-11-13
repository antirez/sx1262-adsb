/* The original T-Echo has independent SPI buses for its radio and e-paper.
 * Pin numbers use the PCA10056 mapping, P1.n = 32+n, as in FreakWAN. */
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_TinyUSB.h>
#include "board.h"
extern "C" {
#include "radio_bus.h"
}

enum { POWER=12, BUTTON=42, RF_CS=24, RF_RESET=25, RF_BUSY=17,
       LIGHT=43 };
static SPIClass rf(NRF_SPIM3,23,19,22);

void board_begin(void) {
    pinMode(POWER,OUTPUT); digitalWrite(POWER,HIGH);
    pinMode(BUTTON,INPUT_PULLUP);
    pinMode(LIGHT,OUTPUT); digitalWrite(LIGHT,LOW);
    Serial.begin(115200);
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
