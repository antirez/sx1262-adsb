#ifndef LILYGO_DISPLAY_H
#define LILYGO_DISPLAY_H
#include "history.h"
#include "tuning.h"
#include "esp_err.h"
esp_err_t display_init(void);
esp_err_t display_draw(Screen rows, CardView card, const uint8_t *trace, const ScopeView *scope);
void display_dump(void);
#endif
