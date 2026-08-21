#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

bool is_demo_mode();
void soft_reset();
esp_err_t get_value_from_nvs(const char *key, config_data_type_t type, void *out_value);
esp_err_t set_value_in_nvs(const char *key, config_data_type_t type, const void *value);

#endif
