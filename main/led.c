#include "config.h"
#include "utils.h"

// global
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <rom/ets_sys.h>

static const char *TAG = "LED";

// LED strip handles
static led_strip_handle_t led_strip_scale;

static volatile bool s_led_refreshing = false;
bool led_is_refreshing(void) { return s_led_refreshing; }

// WS2812 frame transmission time: ~50µs per LED + 100µs overhead for RMT to finish/queue interrupts
#define LED_FRAME_TIME_US(led_count) (((led_count) * 50) + 100)

static int32_t get_nvs_int32_default(const char *key, int32_t default_value) {
    int32_t value = default_value;
    if (get_value_from_nvs(key, CONFIG_TYPE_INT32, &value) != ESP_OK) {
        value = default_value;
    }
    return value;
}

void init_ws2812() {
    int32_t scale_led_gpio = get_nvs_int32_default("scale_led_gpio", SCALE_LED_GPIO);
    int32_t scale_led_count = get_nvs_int32_default("scale_led_count", SCALE_LED_COUNT);

    // Scale LED strip configuration
    led_strip_config_t strip_config_scale = {
        .strip_gpio_num = (gpio_num_t)scale_led_gpio,
        .max_leds = scale_led_count,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false
    };

    // RMT configuration (shared between strips)
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = false,
    };

    // Initialize scale LED strip if enabled
    led_strip_scale = NULL;
    if (strip_config_scale.max_leds > 0) {
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config_scale, &rmt_config, &led_strip_scale));
    }
}

// Helper function to get RGB values from a color name, also gets brightness from NVS
static int32_t get_global_brightness(void) {
    int32_t global_brightness;
    if (get_value_from_nvs("bri_def_global", CONFIG_TYPE_INT32, &global_brightness) != ESP_OK) {
        global_brightness = BRIGHTNESS_DEFAULT_GLOBAL;
    }
    return global_brightness;
}

static void get_rgb_from_color(const char *color, uint8_t *red, uint8_t *green, uint8_t *blue, const char *brightness_key, uint8_t default_brightness) {
    int32_t brightness;
    if (get_value_from_nvs(brightness_key, CONFIG_TYPE_INT32, &brightness) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get brightness from NVS (%s), using default: %d", brightness_key, default_brightness);
        brightness = default_brightness;
    }

    if (strcmp(color, "red") == 0) {
        *red = 255; *green = 0; *blue = 0;
    } else if (strcmp(color, "green") == 0) {
        *red = 0; *green = 255; *blue = 0;
    } else if (strcmp(color, "blue") == 0) {
        *red = 0; *green = 0; *blue = 255;
    } else if (strcmp(color, "cyan") == 0 || strcmp(color, "gblue") == 0) {
        *red = 0; *green = 255; *blue = 255;
    } else if (strcmp(color, "yellow") == 0) {
        *red = 255; *green = 255; *blue = 0;
    } else if (strcmp(color, "orange") == 0) {
        *red = 255; *green = 130; *blue = 0;
    } else if (strcmp(color, "amber") == 0) {
        *red = 255; *green = 191; *blue = 0;
    } else if (strcmp(color, "white") == 0) {
        *red = 255; *green = 255; *blue = 255;
    } else if (strcmp(color, "default_scale") == 0 || strcmp(color, "default") == 0) {
        int32_t r, g, b;
        if (get_value_from_nvs("col_scale_r", CONFIG_TYPE_INT32, &r) == ESP_OK) *red = (uint8_t)r; else *red = DEFAULT_COLOR_SCALE_R;
        if (get_value_from_nvs("col_scale_g", CONFIG_TYPE_INT32, &g) == ESP_OK) *green = (uint8_t)g; else *green = DEFAULT_COLOR_SCALE_G;
        if (get_value_from_nvs("col_scale_b", CONFIG_TYPE_INT32, &b) == ESP_OK) *blue = (uint8_t)b; else *blue = DEFAULT_COLOR_SCALE_B;
    } else if (strcmp(color, "off") == 0) {
        *red = 0; *green = 0; *blue = 0;
    } else {
        // Default color (white) if the color is not recognized
        *red = 255; *green = 255; *blue = 255;
    }

    // Apply brightness scaling
    *red = (*red * brightness) / 255;
    *green = (*green * brightness) / 255;
    *blue = (*blue * brightness) / 255;

    int32_t global_brightness = get_global_brightness();
    *red = (*red * global_brightness) / 255;
    *green = (*green * global_brightness) / 255;
    *blue = (*blue * global_brightness) / 255;
}

// Function to set color for the scale LED strip
void set_color_scale(const char *color) {
    int32_t scale_led_count = get_nvs_int32_default("scale_led_count", SCALE_LED_COUNT);
    if (scale_led_count <= 0 || led_strip_scale == NULL) {
        return;
    }

    uint8_t red, green, blue;
    
    // Use default scale brightness for all colors
    if (strcmp(color, "default") == 0) {
        get_rgb_from_color("default_scale", &red, &green, &blue, "bri_def_scale", BRIGHTNESS_DEFAULT_SCALE);
    } else {
        get_rgb_from_color(color, &red, &green, &blue, "bri_def_scale", BRIGHTNESS_DEFAULT_SCALE);
    }

    if (is_demo_mode()) {
        // Set first and last LEDs to cyan to mark demo mode
        uint8_t cyan_red = 0;
        int32_t demo_brightness;
        if (get_value_from_nvs("bri_def_scale", CONFIG_TYPE_INT32, &demo_brightness) != ESP_OK) {
            demo_brightness = BRIGHTNESS_DEFAULT_SCALE;
        }
        int32_t global_brightness = get_global_brightness();
        uint8_t cyan_green = (demo_brightness * global_brightness) / 255;
        uint8_t cyan_blue = (demo_brightness * global_brightness) / 255;
        int last_index_scale = scale_led_count - 1;

        if (strcmp(color, "off") == 0) {
            led_strip_set_pixel(led_strip_scale, 0, 0, 0, 0);
            led_strip_set_pixel(led_strip_scale, last_index_scale, 0, 0, 0);
        } else {
            led_strip_set_pixel(led_strip_scale, 0, cyan_red, cyan_green, cyan_blue);
            led_strip_set_pixel(led_strip_scale, last_index_scale, cyan_red, cyan_green, cyan_blue);
            for (int i = 1; i < last_index_scale; i++) {
                led_strip_set_pixel(led_strip_scale, i, red, green, blue);
            }
        }
    } else {
        // Normal mode - set all LEDs to the same color
        for (int i = 0; i < scale_led_count; i++) {
            led_strip_set_pixel(led_strip_scale, i, red, green, blue);
        }
    }

    s_led_refreshing = true;
    led_strip_refresh(led_strip_scale);
    ets_delay_us(LED_FRAME_TIME_US(scale_led_count));
    s_led_refreshing = false;

    // Update NVS with the current color
    esp_err_t err = set_value_in_nvs("cur_col_scale", CONFIG_TYPE_STRING, (void*)color);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save current scale LED color to NVS");
    }
}

// Deinitialize LED strips for power management (hibernation)
void deinit_led(void) {
    ESP_LOGI(TAG, "Deinitializing LED strips for hibernation");

    // Deinitialize scale LED strip
    if (led_strip_scale != NULL) {
        led_strip_del(led_strip_scale);
        led_strip_scale = NULL;
    }

    ESP_LOGI(TAG, "LED strips deinitialized");
}

void init_led() {
    // Initialize LED strips
    init_ws2812();

    // set IO2 high to turn on bulk cap
    gpio_set_direction(CAP_CONTROL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CAP_CONTROL_GPIO, 1);
    
    // Initialize current color NVS entry if it doesn't exist
    char default_color[] = "default";
    size_t dummy_len = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        if (nvs_get_str(handle, "cur_col_scale", NULL, &dummy_len) != ESP_OK) {
            nvs_set_str(handle, "cur_col_scale", default_color);
            ESP_LOGI(TAG, "Initialized cur_col_scale in NVS to default");
        }
        nvs_commit(handle);
        nvs_close(handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for initializing current LED colors");
    }
}

// Function to refresh the LED colors based on NVS configuration
void refresh_led_colors() {
    char scale_color_str[20];

    // Get current color string from NVS and set the scale LED color
    if (get_value_from_nvs("cur_col_scale", CONFIG_TYPE_STRING, scale_color_str) == ESP_OK) {
        set_color_scale(scale_color_str);
    } else {
        ESP_LOGW(TAG, "Failed to get current scale LED color from NVS, using default.");
        set_color_scale("default");
    }
}
