#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

bool is_demo_mode();
bool have_endpoint_id();
void soft_reset();
void hard_reset();
void get_device_service_name(char *service_name, size_t max);
void generate_custom_service_uuid(uint8_t *uuid_out);
esp_err_t hash_secret(const char *secret, uint8_t *out_hash);
esp_err_t get_value_from_nvs(const char *key, config_data_type_t type, void *out_value);
esp_err_t set_value_in_nvs(const char *key, config_data_type_t type, const void *value);
esp_err_t sync_time_with_ntp();
void ntp_sync_task(void *pvParameter);
bool is_ntp_synced();
void set_capture_enabled(bool enabled);
bool is_capture_enabled();
void set_ota_mode(bool enabled);
bool is_ota_mode();

#endif
