#ifndef LILYGO_TUNING_H
#define LILYGO_TUNING_H
#include "history.h"

enum { DEFAULT_HZ=1089800000, MIN_HZ=150000000, MAX_HZ=1150000000 };
enum { SCOPE_WIDTH=128, COLUMN_MS=20 };
typedef enum { KEY_NONE, KEY_TAP, KEY_DOUBLE, KEY_HOLD, KEY_REPEAT } Key;
typedef struct {
    bool sample, down, held, waiting;
    int64_t changed, pressed, released, repeat;
} Button;

typedef struct {
    uint32_t tick, sum;
    uint16_t count, triggers;
    uint8_t peak, burst_peak;
} Column;
typedef struct {
    uint32_t hz, block;
    int64_t hold_until_ms;
    Column column[SCOPE_WIDTH];
    uint8_t wave[SCOPE_WIDTH], wave_peak;
} Scope;
typedef struct {
    uint8_t mean[SCOPE_WIDTH], peak[SCOPE_WIDTH], burst_peak[SCOPE_WIDTH];
    uint8_t wave[SCOPE_WIDTH], wave_peak;
    uint32_t block;
    bool valid[SCOPE_WIDTH];
    bool zoom;
    unsigned samples, columns, triggers;
    int level, strongest;
} ScopeView;
typedef enum { TUNE_UP, TUNE_DOWN, STEP_SIZE, DEFAULT_FREQ, DISCARD, CLEAR_HISTORY } Action;
typedef struct { uint32_t hz; unsigned step; Action action; bool open; } Menu;

Key button_update(Button *b, bool pressed, int64_t now);
void scope_sample(Scope *scope, uint32_t hz, int64_t now, uint8_t rssi, const Capture *burst);
void scope_view(const Scope *scope, uint32_t hz, int64_t now, ScopeView *view);
void menu_open(Menu *menu, uint32_t hz);
void menu_next(Menu *menu);
void menu_adjust(Menu *menu);
void menu_screen(const Menu *menu, const ScopeView *view, const char *message, Screen rows);
#endif
