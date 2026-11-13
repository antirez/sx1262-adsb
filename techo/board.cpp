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
       TOUCH=11, LIGHT=43, BATTERY=4 };
static SPIClass rf(NRF_SPIM3,23,19,22);
static bool touch_sample, touch_down, touch_held, light_on;
static int64_t touch_changed, touch_pressed;

void board_begin(void) {
    pinMode(POWER,OUTPUT); digitalWrite(POWER,HIGH);
    pinMode(BUTTON,INPUT_PULLUP);
    pinMode(LIGHT,OUTPUT); digitalWrite(LIGHT,LOW);
    pinMode(TOUCH,INPUT);
    /* USB traces on this T-Echo show HIGH idle and LOW during contact. */
    touch_sample=touch_down=digitalRead(TOUCH)==LOW;
    touch_held=touch_down; /* Ignore a finger already present at startup. */
    Serial.begin(115200);
}
void board_bootloader(void) { enterUf2Dfu(); }
bool board_button(void) { return digitalRead(BUTTON)==LOW; }
bool board_poll_touch(int64_t now) {
    /* Release after two seconds to toggle the light, or hold for five
     * seconds to request one battery visibility change. Short touches
     * do nothing; waiting for release keeps the two gestures separate. */
    bool touched=digitalRead(TOUCH)==LOW;
    if (touched!=touch_sample) {
        touch_sample=touched;
        touch_changed=now;
    }
    if (touch_down!=touch_sample && now-touch_changed>=30) {
        touch_down=touch_sample;
        if (touch_down) {
            touch_pressed=touch_changed;
            touch_held=false;
        } else if (!touch_held) {
            /* Recognize a hold even if the field task missed its deadline. */
            int64_t held=touch_changed-touch_pressed;
            if (held>=5000) return true;
            if (held>=2000) {
                light_on=!light_on;
                digitalWrite(LIGHT,light_on?HIGH:LOW);
            }
        }
    }
    if (touch_down && touch_sample && !touch_held && now-touch_pressed>=5000) {
        touch_held=true;
        return true;
    }
    return false;
}
unsigned board_battery_mv(void) {
    /* LilyGO's BAT_ADC is P0.04 through a 1:2 divider. Use the same
     * 3.0 V reference and 12-bit conversion as its readVBAT example. */
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    analogSampleTime(40);
    delay(1);
    (void)analogRead(BATTERY);
    unsigned sum=0;
    for (unsigned i=0;i<8;i++) sum+=analogRead(BATTERY);
    analogReference(AR_DEFAULT);
    analogReadResolution(10);
    analogSampleTime(3);
    return (sum*6000u+8*4096/2)/(8*4096);
}
int board_battery_percent(unsigned mv) {
    /* Voltage-based LiPo estimate, as in Adafruit's adc_vbat example:
     * 3.3 V = 0%, 3.6 V = 10%, 4.2 V = 100%. Not a fuel gauge. */
    if (mv<2500 || mv>4500) return -1;
    if (mv<=3300) return 0;
    if (mv>=4200) return 100;
    if (mv<3600) return (mv-3300)/30;
    return 10+(mv-3600)*15/100;
}
bool board_usb_powered(void) {
    /* Detect VBUS itself, including a wall charger without a USB host.
     * The TP4054 CHRG output only drives the red LED, not a GPIO. */
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk)!=0;
}
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
