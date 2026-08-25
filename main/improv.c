#include "improv.h"
#include "config.h"
#include "wifi.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/usb_serial_jtag.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "IMPROV";
static const uint8_t IMPROV_HEADER[] = {'I', 'M', 'P', 'R', 'O', 'V'};
static const uint8_t IMPROV_VERSION = 1;

typedef enum {
    IMPROV_PACKET_CURRENT_STATE = 0x01,
    IMPROV_PACKET_ERROR_STATE = 0x02,
    IMPROV_PACKET_RPC_COMMAND = 0x03,
    IMPROV_PACKET_RPC_RESULT = 0x04,
} improv_packet_type_t;

typedef enum {
    IMPROV_STATE_STOPPED = 0x00,
    IMPROV_STATE_READY = 0x02,
    IMPROV_STATE_PROVISIONING = 0x03,
    IMPROV_STATE_PROVISIONED = 0x04,
} improv_state_t;

typedef enum {
    IMPROV_ERROR_NONE = 0x00,
    IMPROV_ERROR_INVALID_RPC_PACKET = 0x01,
    IMPROV_ERROR_UNKNOWN_RPC_COMMAND = 0x02,
    IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_ERROR_BAD_HOSTNAME = 0x05,
    IMPROV_ERROR_UNKNOWN = 0xFF,
} improv_error_t;

static int improv_write_packet(uint8_t type, const uint8_t *data, size_t length)
{
    size_t packet_len = 6 + 1 + 1 + 1 + length + 1;
    uint8_t *packet = malloc(packet_len);
    if (!packet) {
        return -1;
    }

    size_t offset = 0;
    memcpy(packet + offset, IMPROV_HEADER, sizeof(IMPROV_HEADER));
    offset += sizeof(IMPROV_HEADER);
    packet[offset++] = IMPROV_VERSION;
    packet[offset++] = type;
    packet[offset++] = (uint8_t)length;
    if (length > 0 && data != NULL) {
        memcpy(packet + offset, data, length);
        offset += length;
    }

    uint8_t checksum = 0;
    for (size_t i = 0; i < offset; i++) {
        checksum += packet[i];
    }
    packet[offset++] = checksum;

    int written = usb_serial_jtag_write_bytes(packet, offset, portMAX_DELAY);
    free(packet);
    return written;
}

static void improv_send_current_state(void)
{
    improv_state_t state = is_wifi_connected() ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY;
    improv_write_packet(IMPROV_PACKET_CURRENT_STATE, (uint8_t *)&state, 1);
}

static void improv_send_error_state(improv_error_t error)
{
    improv_write_packet(IMPROV_PACKET_ERROR_STATE, (uint8_t *)&error, 1);
}

static void improv_send_rpc_result(const char *const *strings, size_t count)
{
    size_t total_length = 0;
    for (size_t i = 0; i < count; i++) {
        total_length += 1 + strlen(strings[i]);
    }

    uint8_t *buffer = malloc(total_length);
    if (!buffer) {
        improv_send_error_state(IMPROV_ERROR_UNKNOWN);
        return;
    }

    uint8_t *ptr = buffer;
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(strings[i]);
        *ptr++ = (uint8_t)len;
        memcpy(ptr, strings[i], len);
        ptr += len;
    }

    improv_write_packet(IMPROV_PACKET_RPC_RESULT, buffer, total_length);
    free(buffer);
}

static void improv_handle_command(const uint8_t *payload, size_t length)
{
    if (length < 2) {
        improv_send_error_state(IMPROV_ERROR_INVALID_RPC_PACKET);
        return;
    }

    uint8_t command = payload[0];
    const uint8_t *data = payload + 1;
    size_t data_len = length - 1;

    switch (command) {
    case 0x01: { // Send Wi-Fi settings
        if (data_len < 2) {
            improv_send_error_state(IMPROV_ERROR_INVALID_RPC_PACKET);
            return;
        }
        uint8_t ssid_len = data[0];
        if (ssid_len == 0 || data_len < 1 + ssid_len + 1) {
            improv_send_error_state(IMPROV_ERROR_INVALID_RPC_PACKET);
            return;
        }

        const char *ssid = (const char *)(data + 1);
        uint8_t password_len = data[1 + ssid_len];
        if (data_len != 1 + ssid_len + 1 + password_len) {
            improv_send_error_state(IMPROV_ERROR_INVALID_RPC_PACKET);
            return;
        }

        const char *password = (const char *)(data + 1 + ssid_len + 1);
        char ssid_buf[33] = {0};
        char password_buf[65] = {0};
        memcpy(ssid_buf, ssid, ssid_len);
        memcpy(password_buf, password, password_len);

        improv_send_current_state();
        improv_state_t provisioning_state = IMPROV_STATE_PROVISIONING;
        improv_write_packet(IMPROV_PACKET_CURRENT_STATE, (uint8_t *)&provisioning_state, 1);

        esp_err_t err = wifi_connect_with_credentials(ssid_buf, password_buf);
        if (err != ESP_OK) {
            improv_send_error_state(IMPROV_ERROR_UNABLE_TO_CONNECT);
            return;
        }

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
        while (xTaskGetTickCount() < deadline) {
            if (is_wifi_connected()) {
                const char *result[] = {"http://scale.local"};
                improv_send_rpc_result(result, 1);
                improv_send_current_state();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        improv_send_error_state(IMPROV_ERROR_UNABLE_TO_CONNECT);
        break;
    }
    case 0x02: // Request current state
        improv_send_current_state();
        break;
    case 0x03: {
        const char *device_info[] = {FIRMWARE_NAME, FIRMWARE_VERSION, "esp32c3", DEVICE_NAME};
        improv_send_rpc_result(device_info, sizeof(device_info) / sizeof(device_info[0]));
        break;
    }
    default:
        improv_send_error_state(IMPROV_ERROR_UNKNOWN_RPC_COMMAND);
        break;
    }
}

static bool improv_parse_packet(const uint8_t *buffer, size_t length, uint8_t *out_type, const uint8_t **out_payload, size_t *out_payload_len)
{
    if (length < 9) {
        return false;
    }

    if (memcmp(buffer, IMPROV_HEADER, sizeof(IMPROV_HEADER)) != 0) {
        return false;
    }
    if (buffer[6] != IMPROV_VERSION) {
        return false;
    }

    uint8_t type = buffer[7];
    uint8_t payload_len = buffer[8];
    size_t expected_len = 6 + 1 + 1 + 1 + payload_len + 1;
    if (length != expected_len) {
        return false;
    }

    uint8_t checksum = 0;
    for (size_t i = 0; i < length - 1; i++) {
        checksum += buffer[i];
    }
    if (checksum != buffer[length - 1]) {
        return false;
    }

    *out_type = type;
    *out_payload = buffer + 9;
    *out_payload_len = payload_len;
    return true;
}

static void improv_task(void *arg)
{
    usb_serial_jtag_driver_config_t driver_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    driver_config.rx_buffer_size = 1024;
    driver_config.tx_buffer_size = 1024;

    esp_err_t err = usb_serial_jtag_driver_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Improv serial started");

    uint8_t buffer[1024];
    size_t buffer_pos = 0;

    while (1) {
        int read = usb_serial_jtag_read_bytes(buffer + buffer_pos, sizeof(buffer) - buffer_pos, pdMS_TO_TICKS(100));
        if (read <= 0) {
            if (!usb_serial_jtag_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        buffer_pos += read;
        if (buffer_pos < 9) {
            continue;
        }

        bool packet_found = false;
        for (size_t i = 0; i + 9 <= buffer_pos; i++) {
            if (memcmp(buffer + i, IMPROV_HEADER, sizeof(IMPROV_HEADER)) != 0) {
                continue;
            }

            uint8_t payload_len = buffer[i + 8];
            size_t packet_len = 6 + 1 + 1 + 1 + payload_len + 1;
            if (i + packet_len > buffer_pos) {
                break;
            }

            uint8_t type;
            const uint8_t *payload;
            size_t payload_len_out;
            if (improv_parse_packet(buffer + i, packet_len, &type, &payload, &payload_len_out)) {
                if (type == IMPROV_PACKET_RPC_COMMAND) {
                    improv_handle_command(payload, payload_len_out);
                }
                size_t remaining = buffer_pos - (i + packet_len);
                memmove(buffer, buffer + i + packet_len, remaining);
                buffer_pos = remaining;
                packet_found = true;
                break;
            }
        }

        if (!packet_found) {
            if (buffer_pos > sizeof(IMPROV_HEADER)) {
                buffer_pos -= 1;
                memmove(buffer, buffer + 1, buffer_pos);
            }
        }
    }
}

void improv_start(void)
{
    xTaskCreate(improv_task, "improv_task", 4096, NULL, 5, NULL);
}
