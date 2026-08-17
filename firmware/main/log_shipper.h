#ifndef LOG_SHIPPER_H
#define LOG_SHIPPER_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t log_shipper_init(void);
void log_shipper_set_paused(bool paused);
bool log_shipper_is_paused(void);

#endif
