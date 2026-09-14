#ifndef LOG_SHIPPER_H
#define LOG_SHIPPER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

esp_err_t log_shipper_init(void);
void log_shipper_set_paused(bool paused);
bool log_shipper_is_paused(void);
void log_shipper_wifi_up(void);
void log_shipper_wifi_down(void);
void log_shipper_flush(void);
void log_shipper_logstat(void);
uint32_t log_shipper_crash_count(void);
bool log_shipper_crash_loop(void);
void log_shipper_mark_healthy(void);

int log_shipper_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int log_shipper_vprintf_locked(const char *fmt, va_list args);

#endif
