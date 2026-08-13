/* Smart scale firmware
   Reworked for simple Wi-Fi provisioning, mDNS, and a live weight page.
*/

#include "config.h"
#include "wifi.h"
#include "led.h"
#include "HX711.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG = "SCALE_APP";

static volatile bool s_is_calibrating = false;

typedef struct {
    const char* key;
    config_data_type_t type;
} config_key_info_t;

const config_key_info_t known_config_keys[] = {
    {"bri_def_scale", CONFIG_TYPE_INT32},
    {"bri_def_global", CONFIG_TYPE_INT32},
    {"col_scale_r", CONFIG_TYPE_INT32},
    {"col_scale_g", CONFIG_TYPE_INT32},
    {"col_scale_b", CONFIG_TYPE_INT32},
    {"scale_led_gpio", CONFIG_TYPE_INT32},
    {"scale_led_count", CONFIG_TYPE_INT32},
    {"scale_factor", CONFIG_TYPE_FLOAT},
    {"weight_thres", CONFIG_TYPE_INT32},
    {"cal_weight", CONFIG_TYPE_INT32},
    {"scale_stab_t", CONFIG_TYPE_INT32},
    {"cur_col_scale", CONFIG_TYPE_STRING}
};

void set_calibrating(bool is_calibrating)
{
    s_is_calibrating = is_calibrating;
    if (is_calibrating) {
        set_color_scale("blue");
    } else {
        set_color_scale("default");
    }
}

// Write all default values to NVS on first boot so every module finds its keys
static void nvs_init_defaults(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing defaults: %s", esp_err_to_name(err));
        return;
    }

    bool committed = false;

    // LED brightness defaults
    int32_t bri_scale;
    if (nvs_get_i32(handle, "bri_def_scale", &bri_scale) != ESP_OK) {
        bri_scale = BRIGHTNESS_DEFAULT_SCALE;
        nvs_set_i32(handle, "bri_def_scale", bri_scale);
        ESP_LOGI(TAG, "NVS default: bri_def_scale = %" PRId32, bri_scale);
        committed = true;
    }

    int32_t bri_global;
    if (nvs_get_i32(handle, "bri_def_global", &bri_global) != ESP_OK) {
        bri_global = BRIGHTNESS_DEFAULT_GLOBAL;
        nvs_set_i32(handle, "bri_def_global", bri_global);
        ESP_LOGI(TAG, "NVS default: bri_def_global = %" PRId32, bri_global);
        committed = true;
    }

    // Scale LED color defaults
    int32_t col_r;
    if (nvs_get_i32(handle, "col_scale_r", &col_r) != ESP_OK) {
        col_r = DEFAULT_COLOR_SCALE_R;
        nvs_set_i32(handle, "col_scale_r", col_r);
        ESP_LOGI(TAG, "NVS default: col_scale_r = %" PRId32, col_r);
        committed = true;
    }

    int32_t col_g;
    if (nvs_get_i32(handle, "col_scale_g", &col_g) != ESP_OK) {
        col_g = DEFAULT_COLOR_SCALE_G;
        nvs_set_i32(handle, "col_scale_g", col_g);
        ESP_LOGI(TAG, "NVS default: col_scale_g = %" PRId32, col_g);
        committed = true;
    }

    int32_t col_b;
    if (nvs_get_i32(handle, "col_scale_b", &col_b) != ESP_OK) {
        col_b = DEFAULT_COLOR_SCALE_B;
        nvs_set_i32(handle, "col_scale_b", col_b);
        ESP_LOGI(TAG, "NVS default: col_scale_b = %" PRId32, col_b);
        committed = true;
    }

    int32_t scale_led_gpio;
    if (nvs_get_i32(handle, "scale_led_gpio", &scale_led_gpio) != ESP_OK) {
        scale_led_gpio = (int32_t)SCALE_LED_GPIO;
        nvs_set_i32(handle, "scale_led_gpio", scale_led_gpio);
        ESP_LOGI(TAG, "NVS default: scale_led_gpio = %" PRId32, scale_led_gpio);
        committed = true;
    }

    int32_t scale_led_count;
    if (nvs_get_i32(handle, "scale_led_count", &scale_led_count) != ESP_OK) {
        scale_led_count = SCALE_LED_COUNT;
        nvs_set_i32(handle, "scale_led_count", scale_led_count);
        ESP_LOGI(TAG, "NVS default: scale_led_count = %" PRId32, scale_led_count);
        committed = true;
    }

    // HX711 defaults
    float scale_factor_val;
    size_t float_size = sizeof(float);
    if (nvs_get_blob(handle, "scale_factor", &scale_factor_val, &float_size) != ESP_OK) {
        scale_factor_val = (float)SCALE_FACTOR;
        nvs_set_blob(handle, "scale_factor", &scale_factor_val, sizeof(float));
        ESP_LOGI(TAG, "NVS default: scale_factor = %.2f", (double)scale_factor_val);
        committed = true;
    }

    int32_t weight_thres;
    if (nvs_get_i32(handle, "weight_thres", &weight_thres) != ESP_OK) {
        weight_thres = WEIGHT_THRESHOLD;
        nvs_set_i32(handle, "weight_thres", weight_thres);
        ESP_LOGI(TAG, "NVS default: weight_thres = %" PRId32, weight_thres);
        committed = true;
    }

    int32_t cal_weight;
    if (nvs_get_i32(handle, "cal_weight", &cal_weight) != ESP_OK) {
        cal_weight = CALIBRATION_WEIGHT;
        nvs_set_i32(handle, "cal_weight", cal_weight);
        ESP_LOGI(TAG, "NVS default: cal_weight = %" PRId32, cal_weight);
        committed = true;
    }

    int32_t scale_stab_t;
    if (nvs_get_i32(handle, "scale_stab_t", &scale_stab_t) != ESP_OK) {
        scale_stab_t = SCALE_STABILITY_TIME;
        nvs_set_i32(handle, "scale_stab_t", scale_stab_t);
        ESP_LOGI(TAG, "NVS default: scale_stab_t = %" PRId32, scale_stab_t);
        committed = true;
    }

    // Current LED colors (default to "default" so animation/wave plays)
    size_t cur_len = 0;
    if (nvs_get_str(handle, "cur_col_scale", NULL, &cur_len) != ESP_OK) {
        nvs_set_str(handle, "cur_col_scale", "default");
        ESP_LOGI(TAG, "NVS default: cur_col_scale = default");
        committed = true;
    }

    if (committed) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "NVS defaults written successfully");
    } else {
        ESP_LOGI(TAG, "NVS defaults already present, nothing to write");
    }

    nvs_close(handle);
}

config_data_type_t get_key_type(const char* key) {
    for (size_t i = 0; i < sizeof(known_config_keys) / sizeof(known_config_keys[0]); i++) {
        if (strcmp(known_config_keys[i].key, key) == 0) {
            return known_config_keys[i].type;
        }
    }
    return CONFIG_TYPE_STRING; // fallback for unknowns
}

void config_nvs_dump_all(void)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", "config", NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == NULL) {
        ESP_LOGI(TAG, "No entries found in NVS namespace 'config'.");
        return;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open("config", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle for namespace 'config'.");
        return;
    }
    while (it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        err = nvs_entry_next(&it);

        config_data_type_t key_type = get_key_type(info.key);

        if (info.type == NVS_TYPE_U8 && key_type == CONFIG_TYPE_U8) {
            uint8_t val;
            if (nvs_get_u8(handle, info.key, &val) == ESP_OK) {
                //ESP_LOGI(TAG, "%s: u8=%u", info.key, val);
                ESP_LOGI(TAG, "%s: %u", info.key, val);
            }
        } else if (info.type == NVS_TYPE_I32 && key_type == CONFIG_TYPE_INT32) {
            int32_t val;
            if (nvs_get_i32(handle, info.key, &val) == ESP_OK) {
                //ESP_LOGI(TAG, "%s: int32=%" PRId32, info.key, val);
                ESP_LOGI(TAG, "%s: %" PRId32, info.key, val);
            }
        } else if (info.type == NVS_TYPE_STR && key_type == CONFIG_TYPE_STRING) {
            char str[64];
            size_t required_size = sizeof(str);
            if (nvs_get_str(handle, info.key, str, &required_size) == ESP_OK) {
                //ESP_LOGI(TAG, "%s: string=%s", info.key, str);
                ESP_LOGI(TAG, "%s: %s", info.key, str);
            }
        } else if (info.type == NVS_TYPE_BLOB) {
            uint8_t buf[64];
            size_t required_size = sizeof(buf);
            if (nvs_get_blob(handle, info.key, buf, &required_size) == ESP_OK) {
                switch (key_type) {
                    case CONFIG_TYPE_FLOAT: {
                        float val = *(float*)buf;
                        //ESP_LOGI(TAG, "%s: float=%.3f", info.key, val);
                        ESP_LOGI(TAG, "%s: %.3f", info.key, val);
                        break;
                    }
                    case CONFIG_TYPE_INT32: {
                        int32_t val = *(int32_t*)buf;
                        //ESP_LOGI(TAG, "%s: int32 (as blob)=%" PRId32, info.key, val);
                        ESP_LOGI(TAG, "%s: %" PRId32, info.key, val);
                        break;
                    }
                    case CONFIG_TYPE_BLOB:
                    default: {
                        ESP_LOGI(TAG, "%s: Blob (len=%zu)", info.key, required_size);
                        char hexbuf[3 * 64 + 1] = {0}; // hex dump for up to 64 bytes
                        for (size_t i = 0; i < required_size && i < 64; i++) {
                            sprintf(hexbuf + i * 3, "%02X ", buf[i]);
                        }
                        ESP_LOGI(TAG, "%s: %s", info.key, hexbuf);
                        break;
                    }
                }
            } else {
                ESP_LOGW(TAG, "%s: Unsupported or unknown type (key_type=%d, nvs_type=%d)", info.key, key_type, info.type);
            }
        }
    }
    nvs_close(handle);
}

static void button_task(void *arg)
{
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLDOWN_ONLY);

    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (gpio_get_level(BUTTON_GPIO) != 1) {
                continue;
            }

            TickType_t press_start = xTaskGetTickCount();
            bool reset_pending = false;

            while (gpio_get_level(BUTTON_GPIO) == 1) {
                TickType_t elapsed = xTaskGetTickCount() - press_start;
                if (!reset_pending && elapsed >= pdMS_TO_TICKS(RESET_WIFI_HOLD_TIME)) {
                    ESP_LOGW(TAG, "Wi-Fi reset button held for %d ms", RESET_WIFI_HOLD_TIME);
                    set_color_scale("red");
                    reset_pending = true;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            TickType_t held_time = xTaskGetTickCount() - press_start;
            if (reset_pending) {
                ESP_LOGW(TAG, "Button released after Wi-Fi reset hold: resetting credentials");
                soft_reset();
            } else if (held_time < pdMS_TO_TICKS(TARE_HOLD_TIME)) {
                ESP_LOGI(TAG, "Button tap detected: performing tare");
                set_color_scale("blue");
                HX711_tare();
            } else {
                ESP_LOGI(TAG, "Button release after %u ms: no action", (unsigned int)(held_time * portTICK_PERIOD_MS));
            }

            set_color_scale("default");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Write all default config values to NVS on first boot
    nvs_init_defaults();
    config_nvs_dump_all();

    ESP_LOGI(TAG, "Initializing LEDs");
    init_led();
    set_color_scale("blue");

    ESP_LOGI(TAG, "Initializing HX711");
    HX711_init(HX711_DT_GPIO, HX711_SCK_GPIO, eGAIN_128);
    vTaskDelay(pdMS_TO_TICKS(200));
    HX711_tare();
    HX711_start_sampling_task();

    set_color_scale("default");

    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initializing Wi-Fi");
    init_wifi();

    ESP_LOGI(TAG, "Starting web server");
    start_webserver();

    ESP_LOGI(TAG, "Scale ready at http://scale.local/");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
