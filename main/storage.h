#ifndef LILYGO_STORAGE_H
#define LILYGO_STORAGE_H
#include "history.h"
#include "esp_err.h"
esp_err_t storage_init(void);
typedef enum { ERASE_FAILED, ERASE_PENDING, ERASE_DONE } EraseResult;
EraseResult storage_clear_packets(void);
void storage_restore(void);
bool storage_save(const char *line, bool packet);
void storage_dump(void);
bool storage_import(const char *json);
#endif
