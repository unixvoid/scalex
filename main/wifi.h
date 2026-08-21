#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_http_server.h"

// Calibration state for async UI feedback
typedef enum {
    CAL_IDLE = 0,
    CAL_PLACE_WEIGHT,
    CAL_REMOVE_WEIGHT,
    CAL_COMPLETE,
    CAL_FAILED
} calibration_state_t;

void init_wifi();
bool is_wifi_connected();
httpd_handle_t start_webserver(void);

// Calibration status API (called from HX711.c during calibration)
void set_calibration_status(calibration_state_t state, const char *msg);
calibration_state_t get_calibration_state(void);
const char *get_calibration_message(void);

#endif
