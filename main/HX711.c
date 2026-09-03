#include "config.h"
#include "utils.h"
#include "led.h"
#include "wifi.h"
#include "HX711.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <rom/ets_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <math.h>

#define HIGH 1
#define LOW 0
#define CLOCK_DELAY_US 1

#define TAG "HX711"

static gpio_num_t GPIO_PD_SCK = GPIO_NUM_10;
static gpio_num_t GPIO_DOUT = GPIO_NUM_7;
static HX711_GAIN GAIN = eGAIN_128;
static unsigned long OFFSET = 0;
static float SCALE = 1.0f;
static float WEIGHT_THRESHOLD_VAL = 5.0f;
static int SCALE_STABILITY_TIME_MS = 100;

static TaskHandle_t s_hx711_task_handle = NULL;
static float s_cached_weight = 0.0f;
static portMUX_TYPE s_cached_weight_mux = portMUX_INITIALIZER_UNLOCKED;
static const TickType_t s_hx711_sample_delay = pdMS_TO_TICKS(200);

void HX711_init(gpio_num_t dout, gpio_num_t pd_sck, HX711_GAIN gain)
{
    ESP_LOGI(TAG, "Initializing HX711 with DOUT=GPIO%d, SCK=GPIO%d, GAIN=%d", dout, pd_sck, gain);
    
    GPIO_PD_SCK = pd_sck;
    GPIO_DOUT = dout;

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_PD_SCK);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << GPIO_DOUT);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    HX711_set_gain(gain);

    // Load values from NVS with correct types
    if (get_value_from_nvs("scale_factor", CONFIG_TYPE_FLOAT, &SCALE) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load scale_factor from NVS, using default: %f", (double)SCALE_FACTOR);
        SCALE = SCALE_FACTOR;
    } else {
        ESP_LOGI(TAG, "Loaded scale_factor from NVS: %f", (double)SCALE);
    }

    int32_t temp_weight_thres;
    if (get_value_from_nvs("weight_thres", CONFIG_TYPE_INT32, &temp_weight_thres) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load weight_thres from NVS, using default: %f", (double)WEIGHT_THRESHOLD);
        WEIGHT_THRESHOLD_VAL = WEIGHT_THRESHOLD;
    }
    else{
        WEIGHT_THRESHOLD_VAL = (float)temp_weight_thres;
        ESP_LOGI(TAG, "Loaded weight_thres from NVS: %f", (double)WEIGHT_THRESHOLD_VAL);
    }

    if (get_value_from_nvs("scale_stab_t", CONFIG_TYPE_INT32, &SCALE_STABILITY_TIME_MS) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load scale_stab_t from NVS, using default: %d", SCALE_STABILITY_TIME);
        SCALE_STABILITY_TIME_MS = SCALE_STABILITY_TIME;
    } else {
        ESP_LOGI(TAG, "Loaded scale_stab_t from NVS: %d ms", SCALE_STABILITY_TIME_MS);
    }
    
    ESP_LOGI(TAG, "HX711 initialization complete. OFFSET=%lu, SCALE=%.2f", OFFSET, (double)SCALE);
}

bool HX711_is_ready()
{
    return gpio_get_level(GPIO_DOUT);
}

void HX711_set_gain(HX711_GAIN gain)
{
    GAIN = gain;
    gpio_set_level(GPIO_PD_SCK, LOW);
    HX711_read();
}

uint8_t HX711_shiftIn()
{
    uint8_t value = 0;

    for (int i = 0; i < 8; ++i) {
        gpio_set_level(GPIO_PD_SCK, HIGH);
        ets_delay_us(CLOCK_DELAY_US);
        value |= gpio_get_level(GPIO_DOUT) << (7 - i);
        gpio_set_level(GPIO_PD_SCK, LOW);
        ets_delay_us(CLOCK_DELAY_US);
    }

    return value;
}

unsigned long HX711_read()
{
    gpio_set_level(GPIO_PD_SCK, LOW);
    
    // Wait for READY signal (with 500ms timeout to avoid hanging)
    int timeout_us = 500000; 
    while (HX711_is_ready()) {
        ets_delay_us(500); // Check every 500us
        timeout_us -= 500;
        if (timeout_us <= 0) {
            return 0; // Device not responding
        }
    }

    unsigned long value = 0;
    // Wait for any ongoing LED refresh to complete before disabling interrupts.
    // Without this, portDISABLE_INTERRUPTS starves the RMT interrupt handler
    // mid-frame, causing WS2812 LED stuttering — especially at high ADC rates.
    uint32_t led_wait_us = 2000; // 2ms max wait
    while (led_is_refreshing() && led_wait_us > 0) {
        ets_delay_us(10);
        led_wait_us -= 10;
    }
    portDISABLE_INTERRUPTS();
    for (int i = 0; i < 24; i++) {
        gpio_set_level(GPIO_PD_SCK, HIGH);
        ets_delay_us(CLOCK_DELAY_US);
        value = value << 1;
        gpio_set_level(GPIO_PD_SCK, LOW);
        ets_delay_us(CLOCK_DELAY_US);

        if (gpio_get_level(GPIO_DOUT))
            value++;
    }
    for (unsigned int i = 0; i < GAIN; i++) {
        gpio_set_level(GPIO_PD_SCK, HIGH);
        ets_delay_us(CLOCK_DELAY_US);
        gpio_set_level(GPIO_PD_SCK, LOW);
        ets_delay_us(CLOCK_DELAY_US);
    }
    portENABLE_INTERRUPTS();
    value = value ^ 0x800000;
    return (value);
}

unsigned long HX711_read_average(char times)
{
    unsigned long sum = 0;
    for (char i = 0; i < times; i++) {
        sum += HX711_read();
    }
    return sum / times;
}

unsigned long HX711_read_median(int times)
{
    unsigned long readings[times];
    for (int i = 0; i < times; i++) {
        readings[i] = HX711_read();
    }
    // Simple bubble sort for median
    for (int i = 0; i < times - 1; i++) {
        for (int j = 0; j < times - i - 1; j++) {
            if (readings[j] > readings[j + 1]) {
                unsigned long temp = readings[j];
                readings[j] = readings[j + 1];
                readings[j + 1] = temp;
            }
        }
    }
    return readings[times / 2];
}

unsigned long HX711_get_value(char times)
{
    unsigned long avg = HX711_read_average(times);
    if (avg > OFFSET)
        return avg - OFFSET;
    else
        return 0;
}

float HX711_get_units_median(int times)
{
    unsigned long avg = HX711_read_median(times);
    if (avg > OFFSET)
        return (avg - OFFSET) / SCALE;
    else
        return 0;
}

void HX711_tare()
{
    ESP_LOGI(TAG, "Taring scale...");
    
    // Give the scale a brief moment to physically settle
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Aggressive stability check - take multiple readings over time and check variance
    ESP_LOGI(TAG, "Checking stability before tare...");
    const int stability_readings = 10;           // Number of readings to collect
    const int reading_interval_ms = 150;         // Interval between readings
    const unsigned long max_variance = 800;      // Maximum allowed variance in raw units
    const int max_attempts = 10;                  // Number of stability check cycles
    bool stable = false;
    
    for (int attempt = 0; attempt < max_attempts && !stable; attempt++) {
        unsigned long readings[stability_readings];
        
        // Collect multiple readings
        for (int i = 0; i < stability_readings; i++) {
            readings[i] = HX711_read_average(2);
            if (i < stability_readings - 1) {
                vTaskDelay(pdMS_TO_TICKS(reading_interval_ms));
            }
        }
        
        // Calculate statistics
        unsigned long min_reading = readings[0];
        unsigned long max_reading = readings[0];
        unsigned long sum = readings[0];
        
        for (int i = 1; i < stability_readings; i++) {
            if (readings[i] < min_reading) min_reading = readings[i];
            if (readings[i] > max_reading) max_reading = readings[i];
            sum += readings[i];
        }
        
        unsigned long variance = (max_reading > min_reading) ? 
                                (max_reading - min_reading) : 
                                (min_reading - max_reading);
        
        unsigned long average = sum / stability_readings;
        
        // Check if stable (variance within threshold)
        if (variance <= max_variance) {
            ESP_LOGI(TAG, "Scale stable. Variance: %lu (avg: %lu, range: %lu-%lu)", 
                    variance, average, min_reading, max_reading);
            stable = true;
        } else {
            ESP_LOGW(TAG, "Scale not stable. Variance: %lu (exceeds %lu), attempt %d/%d. Range: %lu-%lu", 
                    variance, max_variance, attempt + 1, max_attempts, min_reading, max_reading);
            if (attempt < max_attempts - 1) {
                // Wait longer before retry to allow device to settle
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
    
    if (!stable) {
        ESP_LOGW(TAG, "Scale did not stabilize after %d checks. Make sure device is placed on a stable surface.", max_attempts);
        // Still tare, but log the warning
    }
    
    // Perform the actual tare with fresh readings
    unsigned long sum = HX711_read_average(6);
    HX711_set_offset(sum);
    ESP_LOGI(TAG, "Tare complete. Offset: %lu", sum);
}

void HX711_set_scale(float scale)
{
    SCALE = scale;
}

float HX711_get_scale()
{
    return SCALE;
}

void HX711_set_offset(unsigned long offset)
{
    OFFSET = offset;
}

unsigned long HX711_get_offset()
{
    return OFFSET;
}

void HX711_power_down()
{
    gpio_set_level(GPIO_PD_SCK, LOW);
    ets_delay_us(CLOCK_DELAY_US);
    gpio_set_level(GPIO_PD_SCK, HIGH);
    ets_delay_us(CLOCK_DELAY_US);
}

void HX711_power_up()
{
    gpio_set_level(GPIO_PD_SCK, LOW);
}

static void hx711_sample_task(void *arg)
{
    (void)arg;
    while (1) {
        float weight = HX711_get_units_median(3);

        portENTER_CRITICAL(&s_cached_weight_mux);
        s_cached_weight = weight;
        portEXIT_CRITICAL(&s_cached_weight_mux);

        vTaskDelay(s_hx711_sample_delay);
    }
}

void HX711_start_sampling_task(void)
{
    if (s_hx711_task_handle != NULL) {
        return;
    }

    s_cached_weight = 0.0f;
    xTaskCreate(hx711_sample_task, "hx711_sampler", 4096, NULL, 5, &s_hx711_task_handle);
}

float HX711_get_cached_weight(void)
{
    float weight;
    portENTER_CRITICAL(&s_cached_weight_mux);
    weight = s_cached_weight;
    portEXIT_CRITICAL(&s_cached_weight_mux);
    return weight;
}

float HX711_get_weight_threshold()
{
    return WEIGHT_THRESHOLD_VAL;
}

int HX711_get_stability_time_ms()
{
    return SCALE_STABILITY_TIME_MS;
}

void HX711_update_scale_factor() {
    float new_scale;
    if (get_value_from_nvs("scale_factor", CONFIG_TYPE_FLOAT, &new_scale) == ESP_OK) {
        SCALE = new_scale;
        ESP_LOGI(TAG, "Scale factor updated to: %f from NVS", (double)SCALE);
    } else {
        ESP_LOGE(TAG, "Failed to load scale_factor from NVS");
    }
}

void HX711_update_weight_threshold() {
    float new_threshold;
    int32_t temp_threshold;
     if (get_value_from_nvs("weight_thres", CONFIG_TYPE_INT32, &temp_threshold) == ESP_OK) {
        new_threshold = (float)temp_threshold;
        WEIGHT_THRESHOLD_VAL = new_threshold;
        ESP_LOGI(TAG, "Weight threshold updated to: %f from NVS", (double)WEIGHT_THRESHOLD_VAL);
    } else {
        ESP_LOGE(TAG, "Failed to load weight_thres from NVS");
    }
}

void HX711_update_stability_time() {
    int new_time_ms;
    if (get_value_from_nvs("scale_stab_t", CONFIG_TYPE_INT32, &new_time_ms) == ESP_OK) {
        SCALE_STABILITY_TIME_MS = new_time_ms;
        ESP_LOGI(TAG, "Scale stability time updated to: %d ms from NVS", SCALE_STABILITY_TIME_MS);
    } else {
        ESP_LOGE(TAG, "Failed to load scale_stab_t from NVS");
    }
}

// Interactive calibration routine - prompts user to place known weight on scale
esp_err_t HX711_calibrate_scale() {
    #define CALIBRATION_STABLE_READINGS 5
    #define CALIBRATION_WAIT_TIMEOUT_MS 12000
    #define STABILITY_PERCENTAGE 0.02f // Percentage of the average weight to consider as stable

    ESP_LOGI(TAG, "Starting interactive scale calibration.");

    // External calibration control (defined in app_main.c)
    extern void set_calibrating(bool is_calibrating);
    set_calibrating(true);

    esp_err_t result = ESP_FAIL;
    esp_err_t err;
    nvs_handle_t nvs_handle;

    // Tare the scale first
    HX711_tare();

    // Open NVS to read the calibration weight
    err = nvs_open("config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS for reading config: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int32_t calibration_weight_int;
    err = nvs_get_i32(nvs_handle, "cal_weight", &calibration_weight_int);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading 'cal_weight' from NVS: %s", esp_err_to_name(err));
        goto cleanup;
    }

    float known_weight_grams = (float)calibration_weight_int;
    ESP_LOGI(TAG, "Calibration weight expected: %.1f g", (double)known_weight_grams);

    ESP_LOGI(TAG, "Taking initial readings to establish baseline...");
    float initial_readings[3];
    float initial_sum = 0;
    for (int i = 0; i < 3; i++) {
        initial_readings[i] = HX711_get_units_median(3);
        initial_sum += initial_readings[i];
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay between readings
    }
    float initial_average = initial_sum / 3;
    ESP_LOGI(TAG, "Initial average reading: %.2f units", (double)initial_average);

    ESP_LOGI(TAG, "Waiting for stable reading with calibration weight placed on the scale...");

    // Set scale LED to blue - ready for weight placement
    set_color_scale("blue");

    float stable_weight = 0.0f;
    float readings[CALIBRATION_STABLE_READINGS];
    int readings_index = 0;
    uint32_t stable_start_time = esp_timer_get_time() / 1000;
    bool weight_placed = false;
    float weight_placed_threshold = initial_average + (fabs(initial_average) * STABILITY_PERCENTAGE * 2.0f + 5.0f);

    while (esp_timer_get_time() / 1000 - stable_start_time < CALIBRATION_WAIT_TIMEOUT_MS) {
        float current_reading = HX711_get_units_median(3);

        if (!weight_placed && current_reading > weight_placed_threshold) {
            ESP_LOGI(TAG, "Calibration weight detected.");
            weight_placed = true;
            stable_start_time = esp_timer_get_time() / 1000; // Reset timer once weight is detected
        }

        if (weight_placed) {
            readings[readings_index++] = current_reading;

            if (readings_index == CALIBRATION_STABLE_READINGS) {
                float sum = 0;
                float min = readings[0];
                float max = readings[0];

                for (int i = 0; i < CALIBRATION_STABLE_READINGS; i++) {
                    sum += readings[i];
                    if (readings[i] < min) min = readings[i];
                    if (readings[i] > max) max = readings[i];
                }

                float average = sum / CALIBRATION_STABLE_READINGS;
                float weight_range = max - min;

                if (average != 0 && (weight_range / fabs(average)) < STABILITY_PERCENTAGE) {
                    stable_weight = average;
                    ESP_LOGI(TAG, "Stable weight reached (within %.1f%%): %.2f units", (STABILITY_PERCENTAGE * 100.0), (double)stable_weight);
                    break;
                }
                readings_index = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!weight_placed) {
        ESP_LOGE(TAG, "Timeout: Calibration weight was not placed on the scale.");
        return ESP_FAIL;
    }

    if (stable_weight == 0.0f) {
        ESP_LOGE(TAG, "Timeout: Could not get a stable reading with the calibration weight.");
        return ESP_FAIL;
    }

    // Calculate the new scale factor
    unsigned long raw_value = HX711_get_value(5);
    if (raw_value == 0) {
        ESP_LOGE(TAG, "Error: Raw value is zero. Calibration failed.");
        return ESP_FAIL;
    }
    float new_scale = (float)raw_value / known_weight_grams;
    ESP_LOGI(TAG, "Calculated new scale factor: %.2f (raw=%lu, known_weight=%.1f)", 
             (double)new_scale, raw_value, (double)known_weight_grams);

    // Write the new scale factor to NVS
    err = set_value_in_nvs("scale_factor", CONFIG_TYPE_FLOAT, &new_scale);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing scale_factor to NVS: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Update active scale immediately so the new calibration takes effect right away.
    HX711_set_scale(new_scale);
    ESP_LOGI(TAG, "New scale factor written to NVS: %.2f and applied immediately", (double)new_scale);

    // Set scale LED to green - weight detected and confirmed
    set_color_scale("green");

    ESP_LOGI(TAG, "Please remove the calibration weight from the scale.");
    set_calibration_status(CAL_REMOVE_WEIGHT, "Please remove the calibration weight from the scale.");

    uint32_t remove_start_time = esp_timer_get_time() / 1000;
    bool weight_removed = false;
    float weight_removed_threshold;
    float current_weight;
    float removal_percentage = 0.3f; // Expecting the weight to drop by at least 30%

    // Calculate the threshold based on the stable weight
    weight_removed_threshold = stable_weight * (1.0f - removal_percentage);

    while (esp_timer_get_time() / 1000 - remove_start_time < CALIBRATION_WAIT_TIMEOUT_MS) {
        current_weight = HX711_get_units_median(3);
        if (current_weight < weight_removed_threshold) {
            weight_removed = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!weight_removed) {
        ESP_LOGW(TAG, "Timeout: Calibration weight was not removed within the expected time.");
    } else {
        ESP_LOGI(TAG, "Calibration weight removed. Calibration complete.");
    }

    // Reset scale LED to default - calibration done
    set_color_scale("default");
    result = ESP_OK;

cleanup:
    set_calibrating(false);
    return result;
    
    // Clear calibration flag to resume weight monitoring
    set_calibrating(false);
    
    return ESP_OK;
}

// EMA-filtered single reading for fast convergence detection
float HX711_get_units_filtered(float *ema_state, float alpha)
{
    float raw_reading = HX711_get_units_median(1);  // Single median sample
    
    // Initialize EMA on first call
    if (*ema_state == 0.0f) {
        *ema_state = raw_reading;
        return raw_reading;
    }
    
    // Apply exponential moving average: new_EMA = alpha * raw + (1-alpha) * old_EMA
    *ema_state = (alpha * raw_reading) + ((1.0f - alpha) * (*ema_state));
    return *ema_state;
}

float HX711_wait_for_settling_fast(float *ema_state, float threshold_roc, int max_wait_ms)
{
    const float EMA_ALPHA = 0.40f;  // Smoother EMA: 40% new, 60% history
    const int POLL_INTERVAL_MS = 30;  // Poll every 30ms
    const int ROC_CHECK_COUNT = 5;    // Need 5 consecutive low-ROC readings (~150ms stable)
    
    uint32_t start_time = esp_timer_get_time() / 1000;
    int consecutive_settled = 0;
    float prev_ema = 0.0f;
    
    // Prime the EMA with a few initial samples
    for (int i = 0; i < 2; i++) {
        HX711_get_units_filtered(ema_state, EMA_ALPHA);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    prev_ema = *ema_state;
    
    while (1) {
        uint32_t elapsed = esp_timer_get_time() / 1000 - start_time;
        
        if (elapsed > max_wait_ms) {
            ESP_LOGD(TAG, "Weight settling timeout after %lu ms (EMA: %.2f, consecutive settled: %d/%d)", 
                    elapsed, (double)*ema_state, consecutive_settled, ROC_CHECK_COUNT);
            if (consecutive_settled >= 3) {
                ESP_LOGI(TAG, "Timeout with partial settling accepted (EMA: %.2f)", (double)*ema_state);
                return *ema_state;
            }
            return -1.0f;  // Reject if motion continued until timeout
        }
        
        float new_ema = HX711_get_units_filtered(ema_state, EMA_ALPHA);
        
        // Calculate rate of change
        float roc = fabs(new_ema - prev_ema);
        prev_ema = new_ema;
        
        // Check if rate of change is small enough to consider settled
        if (roc <= threshold_roc) {
            consecutive_settled++;
            if (consecutive_settled >= ROC_CHECK_COUNT) {
                ESP_LOGI(TAG, "Weight settled after %lu ms (EMA: %.2f, final ROC: %.4f)", 
                        elapsed, (double)*ema_state, (double)roc);
                return *ema_state;
            }
        } else {
            consecutive_settled = 0;  // Reset if motion detected
        }
        
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    
    return -1.0f;
}
