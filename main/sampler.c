/* The little CPU inside the SX1262 can read its RSSI register much faster
 * than the ESP32 can ask for it over SPI. Semtech's spectral-scan program
 * already starts this CPU and reports completion. Replace its scan loop. */
#include "receiver.h"
#define SX126X_STANDALONE
#define RADIOLIB_NONVOLATILE
#include "vendor/sx126x_patch_scan.h"

enum { ENTRY = 299, RETURN = 358, WARM = ENTRY+9, ROLL = ENTRY+16, POST = ENTRY+40 };
#define MOV(reg,value) (0x0ea000u | ((reg)<<8) | ((value)^255))
#define JUMP(target,cond) (0x300000u | ((cond)<<16) | ((0x8000+(target))^0xffff))
#define READ 0x18ab00     /* R3 = [I0]. A smaller byte means more RF power. */
#define STORE 0x035b01    /* [I1++] = R3 */
#define ADD_LOW 0x0ccbee
#define ADD_CARRY 0x0d5ddd
#define AHEAD (0x0ec200 | (AVERAGE^255))
#define ADVANCE (0x0e3200 | ((AVERAGE-1)^255))
#define PUSH 0x02db00
#define NOP 0x3fffff

static const uint32_t loop[] = {
    MOV(0,0xbe), MOV(1,8),             /* I0 reads RSSI at 0x08be. */
    MOV(2,0), MOV(3,4),                /* I1 writes the 256-byte page at 0x0400. */
    MOV(4,0xc1), MOV(5,6),             /* I2 reads the ESP32's threshold. */
    MOV(14,0), MOV(13,0), MOV(12,AVERAGE),

    /* Fill the moving sum once. From then on subtract one old byte and add
     * one new byte. Only the low address byte wraps, like an 8-bit cursor. */
    READ, STORE, ADD_LOW, ADD_CARRY, 0x0dbccc, JUMP(WARM,2), MOV(2,0),
    READ, 0x1a3e00, 0x0dfddd, AHEAD, PUSH, ADVANCE, ADD_LOW, ADD_CARRY,
    0x0cafdc, 0x1c1e00, 0x1c5c01, JUMP(ROLL,0),

    /* Latch the crossing position. Keep exactly 12 instructions between
     * every pair of reads, including across the branch into the tail. */
    READ, AHEAD, 0x0caf2e, 0x02ee02, PUSH, ADVANCE, MOV(12,SAMPLES-HISTORY-1),
    NOP, NOP, NOP, NOP, NOP,
    READ, AHEAD, PUSH, ADVANCE, NOP, NOP, NOP, NOP, NOP, NOP, 0x0dbccc, JUMP(POST,2),

    /* The vendor's completion routine uses page byte zero. Save it first. */
    MOV(2,0), 0x1aab00, 0x02eb03, JUMP(RETURN,3)
};

_Static_assert(ENTRY+sizeof(loop)/sizeof(*loop) <= RETURN,"sampler overwrites return");
_Static_assert(sizeof(sx126x_patch_scan) == 1544,"unexpected vendor scan image");

uint32_t sampler_word(unsigned index, bool test) {
    if (index < ENTRY || index >= ENTRY+sizeof(loop)/sizeof(*loop))
        return sx126x_patch_scan[index];
    /* During self-test, read a writable register instead of live RF. */
    if (test && index == ENTRY) return MOV(0,0xc0);
    if (test && index == ENTRY+1) return MOV(1,6);
    return loop[index-ENTRY];
}
