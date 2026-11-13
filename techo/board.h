#ifndef TECHO_BOARD_H
#define TECHO_BOARD_H
#ifdef __cplusplus
extern "C" {
#endif
#include "tuning.h"
void techo_start(void);
int64_t uptime_ms(void);
uint32_t board_random(void);
int board_free_heap(void);
bool board_button(void);
void board_begin(void);
void board_bootloader(void);
/* On battery, level is the number of filled bars. */
enum { BATTERY_USB=-2, BATTERY_UNKNOWN=-1, BATTERY_BARS=4 };
typedef struct { unsigned mv; int level; } Battery;
void board_read_battery(Battery *battery, int64_t now);
bool board_paint(Screen s, CardView card, const ScopeView *scope, int battery, bool clean);
void board_paper_status(void);
int board_getchar(void);
void board_print(const char *text);
bool journal_begin(uint32_t *dial);
bool journal_dial(uint32_t hz);
bool journal_save(const Result *r, uint32_t boot, unsigned tests);
bool journal_clear(void);
bool journal_dump(void);
#ifdef __cplusplus
}
#endif
#endif
