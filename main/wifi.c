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
#include "esp_netif_defaults.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include <sys/time.h>

static const char *TAG = "WIFI";

const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group = NULL;
static httpd_handle_t server = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
static bool s_dns_server_running = false;
static int s_dns_sock = -1;
static bool s_ap_enabled = false;
static int s_sta_reconnect_attempts = 0;
static const int MAX_STA_RECONNECT_ATTEMPTS = 5;

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

static esp_err_t save_sta_credentials(const char *ssid, const char *password);

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

bool is_wifi_provisioned(void)
{
    char ssid[33] = {0};
    return (read_nvs_string("wifi_ssid", ssid, sizeof(ssid)) == ESP_OK && ssid[0] != '\0');
}

esp_err_t wifi_connect_with_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = save_sta_credentials(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure STA: %s", esp_err_to_name(err));
        return err;
    }

    return esp_wifi_connect();
}

static esp_err_t load_sta_credentials(wifi_config_t *config)
{
    char ssid[33];
    char password[65];
    memset(config, 0, sizeof(*config));

    esp_err_t err = read_nvs_string("wifi_ssid", ssid, sizeof(ssid));
    if (err != ESP_OK || ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy((char *)config->sta.ssid, ssid, sizeof(config->sta.ssid) - 1);

    err = read_nvs_string("wifi_password", password, sizeof(password));
    if (err == ESP_OK) {
        strncpy((char *)config->sta.password, password, sizeof(config->sta.password) - 1);
    }

    return ESP_OK;
}

static esp_err_t save_sta_credentials(const char *ssid, const char *password)
{
    esp_err_t err = write_nvs_string("wifi_ssid", ssid);
    if (err != ESP_OK) {
        return err;
    }
    return write_nvs_string("wifi_password", password ? password : "");
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    extern const char wifi_html_start[] asm("_binary_wifi_html_start");
    extern const char wifi_html_end[] asm("_binary_wifi_html_end");
    size_t len = wifi_html_end - wifi_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, wifi_html_start, len);
    return ESP_OK;
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/wifi");
    return httpd_resp_send(req, NULL, 0);
}

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)
#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

static uint32_t s_dns_answer_ip = 0;

static char *parse_dns_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = *label;
        name_len += (sub_name_len + 1);
        if (name_len > parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    } while (*label != 0);

    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

static int parse_dns_request(char *req, size_t req_len, char *dns_reply, size_t dns_reply_max_len, uint32_t answer_ip)
{
    if (req_len > dns_reply_max_len) {
        return -1;
    }

    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);

    dns_header_t *header = (dns_header_t *)dns_reply;
    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(header->qd_count);
    char *cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char *cur_ans_ptr = dns_reply + req_len;
    int answer_count = 0;

    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        char name[128];
        char *name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) {
            return -1;
        }

        dns_question_t *question = (dns_question_t *)name_end_ptr;
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);

        if (qd_type == QD_TYPE_A) {
            dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(qd_class);
            answer->ttl = htonl(ANS_TTL_SEC);
            answer->addr_len = htons(sizeof(answer->ip_addr));
            answer->ip_addr = answer_ip;
            cur_ans_ptr += sizeof(dns_answer_t);
            answer_count++;
        }

        cur_qd_ptr = name_end_ptr + sizeof(dns_question_t);
    }

    header->an_count = htons(answer_count);
    return req_len + answer_count * sizeof(dns_answer_t);
}

static void dns_server_task(void *arg)
{
    char rx_buffer[DNS_MAX_LEN];
    char reply[DNS_MAX_LEN];
    char addr_str[128];
    uint32_t answer_ip = s_dns_answer_ip;

    while (s_dns_server_running) {
        struct sockaddr_in dest_addr = {
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT)
        };

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        s_dns_sock = sock;

        if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(sock);
            s_dns_sock = -1;
            break;
        }

        while (s_dns_server_running) {
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }

            if (source_addr.sin6_family == PF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
            } else {
                inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
            }

            int reply_len = parse_dns_request(rx_buffer, len, reply, sizeof(reply), answer_ip);
            if (reply_len <= 0) {
                ESP_LOGW(TAG, "No DNS reply generated for request from %s", addr_str);
                continue;
            }

            if (sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, socklen) < 0) {
                ESP_LOGE(TAG, "Error sending DNS reply: errno %d", errno);
                break;
            }
        }

        close(sock);
        s_dns_sock = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_dns_sock = -1;
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static void stop_dns_server(void)
{
    if (!s_dns_server_running) {
        return;
    }

    s_dns_server_running = false;
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }

    for (int i = 0; i < 100 && s_dns_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void stop_ap(void)
{
    if (!s_ap_enabled) {
        return;
    }

    stop_dns_server();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        s_ap_enabled = false;
        ESP_LOGI(TAG, "AP disabled after STA connected");
    } else {
        ESP_LOGE(TAG, "Failed to disable AP after STA connected: %s", esp_err_to_name(err));
    }
}

static void start_dns_server(void)
{
    if (s_dns_server_running) {
        return;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != IPADDR_ANY) {
            s_dns_answer_ip = ip_info.ip.addr;
        }
    }

    if (s_dns_answer_ip == 0) {
        s_dns_answer_ip = inet_addr("192.168.4.1");
    }

    s_dns_server_running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task_handle);
}

static void wifi_start_ap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .channel = PROV_AP_CHANNEL,
            .max_connection = PROV_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_ap_enabled = true;
}

static void enable_provisioning_ap(void)
{
    if (s_ap_enabled) {
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_start_ap();
    start_dns_server();
    ESP_LOGI(TAG, "AP provisioning mode active");
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    char response[128];
    snprintf(response, sizeof(response), "{\"provisioned\":%s,\"connected\":%s,\"ap_ssid\":\"%s\"}",
             is_wifi_provisioned() ? "true" : "false",
             is_wifi_connected() ? "true" : "false",
             PROV_AP_SSID);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int compare_wifi_ap_records(const void *a, const void *b)
{
    const wifi_ap_record_t *left = a;
    const wifi_ap_record_t *right = b;
    return right->rssi - left->rssi;
}

static esp_err_t wifi_networks_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi scan failed");
        return err;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get AP count");
        return err;
    }

    if (ap_count == 0) {
        cJSON *empty = cJSON_CreateArray();
        char *empty_response = cJSON_PrintUnformatted(empty);
        cJSON_Delete(empty);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, empty_response, HTTPD_RESP_USE_STRLEN);
        cJSON_free(empty_response);
        return ESP_OK;
    }

    if (ap_count > 64) {
        ap_count = 64;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(*ap_records));
    if (ap_records == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get AP records");
        return err;
    }

    qsort(ap_records, ap_count, sizeof(*ap_records), compare_wifi_ap_records);

    cJSON *networks = cJSON_CreateArray();
    if (networks == NULL) {
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    for (uint16_t i = 0; i < ap_count; ++i) {
        const wifi_ap_record_t *record = &ap_records[i];
        if (record->ssid[0] == '\0') {
            continue;
        }

        bool seen = false;
        for (cJSON *item = networks->child; item != NULL; item = item->next) {
            cJSON *ssid_item = cJSON_GetObjectItem(item, "ssid");
            if (ssid_item != NULL && cJSON_IsString(ssid_item) && strcmp(ssid_item->valuestring, (char *)record->ssid) == 0) {
                seen = true;
                break;
            }
        }

        if (seen) {
            continue;
        }

        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *)record->ssid);
        cJSON_AddNumberToObject(network, "rssi", record->rssi);
        cJSON_AddItemToArray(networks, network);
    }

    free(ap_records);

    char *response = cJSON_PrintUnformatted(networks);
    cJSON_Delete(networks);
    if (response == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload length");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read payload");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL || !cJSON_IsObject(root)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_json = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid_json) || ssid_json->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    const char *ssid = ssid_json->valuestring;
    const char *password = (cJSON_IsString(password_json) ? password_json->valuestring : "");

    esp_err_t err = save_sta_credentials(ssid, password);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return err;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    err = wifi_connect_with_credentials(ssid, password);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start STA connect");
        return err;
    }

    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_EVENT);
            if (is_wifi_provisioned()) {
                if (!s_ap_enabled && s_sta_reconnect_attempts < MAX_STA_RECONNECT_ATTEMPTS) {
                    s_sta_reconnect_attempts++;
                    esp_wifi_connect();
                } else if (!s_ap_enabled) {
                    ESP_LOGI(TAG, "STA connect failed after %d attempts, enabling AP provisioning mode", s_sta_reconnect_attempts);
                    enable_provisioning_ap();
                }
            }
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "Provisioning AP started: SSID=%s", PROV_AP_SSID);
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
        s_sta_reconnect_attempts = 0;
        stop_ap();
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!is_wifi_connected() || !is_wifi_provisioned()) {
        return wifi_get_handler(req);
    }

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

static esp_err_t svg_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *data = NULL;
    size_t len = 0;

    if (strcmp(uri, "/refresh_24dp.svg") == 0) {
        extern const char refresh_24dp_svg_start[] asm("_binary_refresh_24dp_svg_start");
        extern const char refresh_24dp_svg_end[] asm("_binary_refresh_24dp_svg_end");
        data = refresh_24dp_svg_start;
        len = refresh_24dp_svg_end - refresh_24dp_svg_start;
    } else if (strcmp(uri, "/visibility_24dp.svg") == 0) {
        extern const char visibility_24dp_svg_start[] asm("_binary_visibility_24dp_svg_start");
        extern const char visibility_24dp_svg_end[] asm("_binary_visibility_24dp_svg_end");
        data = visibility_24dp_svg_start;
        len = visibility_24dp_svg_end - visibility_24dp_svg_start;
    } else {
        return httpd_resp_send_404(req);
    }

    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, data, len);
    return ESP_OK;
}

static esp_err_t weight_get_handler(httpd_req_t *req)
{
    float weight_g = HX711_get_cached_weight();
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
    config.max_uri_handlers = 32;
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

    httpd_uri_t portal_root = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &portal_root);

    httpd_uri_t portal_root_alt = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &portal_root_alt);

    httpd_uri_t apple_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &apple_detect);

    httpd_uri_t ms_detect = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ms_detect);

    httpd_uri_t android_detect = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &android_detect);

    httpd_uri_t style = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &style);

    httpd_uri_t refresh_icon = {
        .uri = "/refresh_24dp.svg",
        .method = HTTP_GET,
        .handler = svg_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &refresh_icon);

    httpd_uri_t visibility_icon = {
        .uri = "/visibility_24dp.svg",
        .method = HTTP_GET,
        .handler = svg_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &visibility_icon);

    httpd_uri_t wifi_page = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_page);

    httpd_uri_t wifi_status = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = wifi_status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_status);

    httpd_uri_t wifi_networks = {
        .uri = "/api/wifi/networks",
        .method = HTTP_GET,
        .handler = wifi_networks_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_networks);

    httpd_uri_t wifi_post = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_post);

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
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_log_level_set("wifi", ESP_LOG_WARN);

    esp_netif_create_default_wifi_ap();

    bool provisioned = is_wifi_provisioned();
    ESP_ERROR_CHECK(esp_wifi_set_mode(provisioned ? WIFI_MODE_STA : WIFI_MODE_APSTA));

    if (!provisioned) {
        wifi_start_ap();
        ESP_ERROR_CHECK(esp_wifi_start());
        start_dns_server();
        ESP_LOGI(TAG, "No Wi-Fi credentials found, AP provisioning mode active");
    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials found, attempting STA connect");
        wifi_config_t sta_config = {0};
        if (load_sta_credentials(&sta_config) == ESP_OK) {
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
            ESP_ERROR_CHECK(esp_wifi_start());
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi credentials were provisioned but failed to load. Enabling AP provisioning mode.");
            enable_provisioning_ap();
            ESP_ERROR_CHECK(esp_wifi_start());
        }
    }

    // Start mDNS regardless of STA state so the device is discoverable by hostname.
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE_NAME));
    ESP_ERROR_CHECK(mdns_service_add(MDNS_INSTANCE_NAME, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS ready at %s.local", MDNS_HOSTNAME);
}
