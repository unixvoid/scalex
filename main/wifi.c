/* Smart scale firmware
   Minimal Wi-Fi provisioning, mDNS, and HTTP server for a live weight page.
*/

#include "config.h"
#include "led.h"
#include "wifi.h"
#include "HX711.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <cJSON.h>
#include <mdns.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_ble.h>

static const char *TAG = "WIFI";
static const char *pop = PROOF_OF_POSSESSION;

const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group = NULL;
static httpd_handle_t server = NULL;

static calibration_state_t s_cal_state = CAL_IDLE;
static char s_cal_message[128] = "";
static TaskHandle_t s_cal_task_handle = NULL;
static bool parse_hex_color(const char *hex, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (hex == NULL || *hex != '#') {
        return false;
    }

    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    if (sscanf(hex, "#%2x%2x%2x", &r, &g, &b) != 3) {
        return false;
    }

    *red = (uint8_t)r;
    *green = (uint8_t)g;
    *blue = (uint8_t)b;
    return true;
}

static esp_err_t read_nvs_string(const char *key, char *out_value, size_t out_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = out_size;
    err = nvs_get_str(handle, key, out_value, &required_size);
    nvs_close(handle);
    return err;
}

static esp_err_t write_nvs_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// Called from HX711.c during calibration to update UI state
void set_calibration_status(calibration_state_t state, const char *msg)
{
    s_cal_state = state;
    if (msg) {
        strncpy(s_cal_message, msg, sizeof(s_cal_message) - 1);
        s_cal_message[sizeof(s_cal_message) - 1] = '\0';
    }
}

calibration_state_t get_calibration_state(void)
{
    return s_cal_state;
}

const char *get_calibration_message(void)
{
    return s_cal_message;
}

bool is_wifi_connected(void)
{
    if (wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_EVENT) != 0;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth error" : "AP not found");
            break;
        }
        case NETWORK_PROV_END:
            network_prov_mgr_deinit();
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_EVENT);
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern const char index_html_start[] asm("_binary_index_html_start");
    extern const char index_html_end[] asm("_binary_index_html_end");
    size_t len = index_html_end - index_html_start;
    httpd_resp_send(req, index_html_start, len);
    return ESP_OK;
}

static esp_err_t style_get_handler(httpd_req_t *req)
{
    extern const char style_css_start[] asm("_binary_style_css_start");
    extern const char style_css_end[] asm("_binary_style_css_end");
    size_t len = style_css_end - style_css_start;
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, style_css_start, len);
    return ESP_OK;
}

static esp_err_t weight_get_handler(httpd_req_t *req)
{
    float weight_g = HX711_get_units_median(3);
    if (weight_g < 0.0f) {
        weight_g = 0.0f;
    }

    char response[64];
    snprintf(response, sizeof(response), "{\"weight_g\":%.2f}", (double)weight_g);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t tare_post_handler(httpd_req_t *req)
{
    HX711_tare();
    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t cal_weight_post_handler(httpd_req_t *req)
{
    char buf[64];
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, total_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read payload");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    int32_t weight_value = atoi(buf);
    if (weight_value <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid weight value");
        return ESP_FAIL;
    }

    esp_err_t err = set_value_in_nvs("cal_weight", CONFIG_TYPE_INT32, &weight_value);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save calibration weight");
        return err;
    }

    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static void calibrate_task(void *arg)
{
    esp_err_t err = HX711_calibrate_scale();
    if (err != ESP_OK) {
        set_calibration_status(CAL_FAILED, "Calibration failed");
    } else {
        set_calibration_status(CAL_COMPLETE, "Calibration complete");
    }
    s_cal_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t calibrate_post_handler(httpd_req_t *req)
{
    // Reset state to allow re-calibration
    s_cal_state = CAL_IDLE;

    set_calibration_status(CAL_PLACE_WEIGHT, "Calibrating... please place the calibration weight on the scale");
    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    // Run calibration in a separate task so the HTTP handler returns immediately
    xTaskCreate(calibrate_task, "calibrate", 4096, NULL, 5, &s_cal_task_handle);
    return ESP_OK;
}

static esp_err_t cal_status_get_handler(httpd_req_t *req)
{
    char response[256];
    const char *msg = get_calibration_message();
    int state = (int)get_calibration_state();
    snprintf(response, sizeof(response), "{\"state\":%d,\"message\":\"%s\"}", state, msg ? msg : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t colors_get_handler(httpd_req_t *req)
{
    int32_t scale_r = DEFAULT_COLOR_SCALE_R;
    int32_t scale_g = DEFAULT_COLOR_SCALE_G;
    int32_t scale_b = DEFAULT_COLOR_SCALE_B;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_i32(handle, "col_scale_r", &scale_r);
        nvs_get_i32(handle, "col_scale_g", &scale_g);
        nvs_get_i32(handle, "col_scale_b", &scale_b);
        nvs_close(handle);
    }

    cJSON *root = cJSON_CreateObject();
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", (unsigned int)scale_r, (unsigned int)scale_g, (unsigned int)scale_b);
    cJSON_AddStringToObject(root, "scale_color", hex);

    int32_t global_brightness = BRIGHTNESS_DEFAULT_GLOBAL;
    int32_t scale_led_gpio = (int32_t)SCALE_LED_GPIO;
    int32_t scale_led_count = SCALE_LED_COUNT;

    if (nvs_open("config", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, "bri_def_global", &global_brightness);
        nvs_get_i32(handle, "scale_led_gpio", &scale_led_gpio);
        nvs_get_i32(handle, "scale_led_count", &scale_led_count);
        nvs_close(handle);
    }
    cJSON_AddNumberToObject(root, "global_brightness", global_brightness);
    cJSON_AddNumberToObject(root, "scale_led_gpio", scale_led_gpio);
    cJSON_AddNumberToObject(root, "scale_led_count", scale_led_count);

    char *response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t colors_post_handler(httpd_req_t *req)
{
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read payload");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL || !cJSON_IsObject(root)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    cJSON *scale_color = cJSON_GetObjectItem(root, "scale_color");
    cJSON *global_brightness = cJSON_GetObjectItem(root, "global_brightness");
    if (!cJSON_IsString(scale_color) || !cJSON_IsNumber(global_brightness)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing color payload");
        return ESP_FAIL;
    }

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    if (!parse_hex_color(scale_color->valuestring, &r, &g, &b)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid scale color");
        return ESP_FAIL;
    }

    int32_t scale_r = r;
    int32_t scale_g = g;
    int32_t scale_b = b;
    esp_err_t err = set_value_in_nvs("col_scale_r", CONFIG_TYPE_INT32, &scale_r);
    if (err == ESP_OK) {
        err = set_value_in_nvs("col_scale_g", CONFIG_TYPE_INT32, &scale_g);
    }
    if (err == ESP_OK) {
        err = set_value_in_nvs("col_scale_b", CONFIG_TYPE_INT32, &scale_b);
    }

    int32_t global_value = global_brightness->valueint;
    if (global_value < 0 || global_value > 255) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "global_brightness must be 0..255");
        return ESP_FAIL;
    }

    if (err == ESP_OK) {
        err = set_value_in_nvs("bri_def_global", CONFIG_TYPE_INT32, &global_value);
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save color settings");
        return err;
    }

    refresh_led_colors();

    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t led_config_post_handler(httpd_req_t *req)
{
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read payload");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL || !cJSON_IsObject(root)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    cJSON *scale_led_gpio = cJSON_GetObjectItem(root, "scale_led_gpio");
    cJSON *scale_led_count = cJSON_GetObjectItem(root, "scale_led_count");
    if (!cJSON_IsNumber(scale_led_gpio) || !cJSON_IsNumber(scale_led_count)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing LED hardware payload");
        return ESP_FAIL;
    }

    int32_t scale_gpio_value = scale_led_gpio->valueint;
    int32_t scale_count_value = scale_led_count->valueint;

    if (scale_gpio_value < 0 || scale_gpio_value > 39) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO values must be 0..39");
        return ESP_FAIL;
    }
    if (scale_count_value < 0 || scale_count_value > 255) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LED counts must be 0..255");
        return ESP_FAIL;
    }

    esp_err_t err = set_value_in_nvs("scale_led_gpio", CONFIG_TYPE_INT32, &scale_gpio_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED config save failed at scale_led_gpio: %s", esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = set_value_in_nvs("scale_led_count", CONFIG_TYPE_INT32, &scale_count_value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LED config save failed at scale_led_count: %s", esp_err_to_name(err));
        }
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save LED hardware");
        return err;
    }

    // Reinitialize LED strips to apply new GPIO/count settings immediately.
    deinit_led();
    init_led();
    refresh_led_colors();

    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
    if (server != NULL) {
        return server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 11;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return NULL;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t style = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &style);

    httpd_uri_t weight = {
        .uri = "/api/weight",
        .method = HTTP_GET,
        .handler = weight_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &weight);

    httpd_uri_t tare = {
        .uri = "/api/tare",
        .method = HTTP_POST,
        .handler = tare_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &tare);

    httpd_uri_t cal_weight = {
        .uri = "/api/cal_weight",
        .method = HTTP_POST,
        .handler = cal_weight_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cal_weight);

    httpd_uri_t calibrate = {
        .uri = "/api/calibrate",
        .method = HTTP_POST,
        .handler = calibrate_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &calibrate);

    httpd_uri_t cal_status = {
        .uri = "/api/cal_status",
        .method = HTTP_GET,
        .handler = cal_status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cal_status);

    httpd_uri_t colors_get = {
        .uri = "/api/colors",
        .method = HTTP_GET,
        .handler = colors_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &colors_get);

    httpd_uri_t colors_post = {
        .uri = "/api/colors",
        .method = HTTP_POST,
        .handler = colors_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &colors_post);

    httpd_uri_t led_config_post = {
        .uri = "/api/led_config",
        .method = HTTP_POST,
        .handler = led_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &led_config_post);

    ESP_LOGI(TAG, "Web server ready");
    return server;
}

void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_log_level_set("wifi", ESP_LOG_WARN);

    network_prov_mgr_config_t prov_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "Starting BLE provisioning");
        set_color_scale("blue");
        char service_name[16];
        get_device_service_name(service_name, sizeof(service_name));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, pop, service_name, NULL));
    } else {
        ESP_LOGI(TAG, "Device already provisioned, connecting to Wi-Fi");
        network_prov_mgr_deinit();
        wifi_init_sta();
    }

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("scale"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Scale"));
    ESP_ERROR_CHECK(mdns_service_add("Scale", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS ready at scale.local");
}
