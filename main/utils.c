#include "config.h"

#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "UTILS";

bool is_demo_mode() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Failed to open NVS handle!");
        return false;
    }
    
    uint8_t demo_mode = 0;
    err = nvs_get_u8(nvs_handle, "demo_mode", &demo_mode);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && demo_mode == 1) {
        return true;
    }
    return false;
}

void soft_reset() {
    ESP_LOGW(TAG, "Soft reset initiated! (Wi-Fi credentials only)");

    nvs_handle_t nvs_handle;

    // Erase credentials stored in IDF-managed Wi-Fi namespace.
    if (nvs_flash_init() == ESP_OK) {
        if (nvs_open("nvs.net80211", NVS_READWRITE, &nvs_handle) == ESP_OK) {
            ESP_LOGW(TAG, "Erasing Wi-Fi driver credentials...");
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }

    // Erase app-specific stored Wi-Fi credentials.
    if (nvs_open("config", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        ESP_LOGW(TAG, "Erasing app Wi-Fi credentials...");
        nvs_erase_key(nvs_handle, "wifi_ssid");
        nvs_erase_key(nvs_handle, "wifi_password");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGW(TAG, "Wi-Fi credentials erased. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_restart();
}

esp_err_t get_value_from_nvs(const char *key, config_data_type_t type, void *out_value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for reading");
        return err;
    }

    esp_err_t ret = ESP_FAIL; // Initialize to failure

    switch (type) {
        case CONFIG_TYPE_U8:
            ret = nvs_get_u8(handle, key, (uint8_t*)out_value);
            break;
        case CONFIG_TYPE_INT32:
            ret = nvs_get_i32(handle, key, (int32_t*)out_value);
            break;
        case CONFIG_TYPE_FLOAT: {
            size_t required_size = sizeof(float);
            ret = nvs_get_blob(handle, key, out_value, &required_size);
            if (ret == ESP_OK && required_size != sizeof(float)) {
                ESP_LOGE(TAG, "Size mismatch for float key: %s", key);
                ret = ESP_FAIL;
            }
            break;
        }
        case CONFIG_TYPE_STRING: {
            char *str_buf = (char*)out_value;
            size_t required_size = 128; // Assume max 128 chars for string
            ret = nvs_get_str(handle, key, str_buf, &required_size);
            break;
        }
        case CONFIG_TYPE_BLOB: {
            ESP_LOGE(TAG, "Blob type not directly supported in get_value_from_nvs. Use nvs_get_blob directly.");
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        default:
            ESP_LOGE(TAG, "Unsupported config data type for key: %s", key);
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    nvs_close(handle);
    return ret;
}

esp_err_t set_value_in_nvs(const char *key, config_data_type_t type, const void *value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return err;
    }

    esp_err_t ret = ESP_FAIL; // Initialize to failure

    switch (type) {
        case CONFIG_TYPE_U8:
            ret = nvs_set_u8(handle, key, *(uint8_t*)value);
            break;
        case CONFIG_TYPE_INT32:
            ret = nvs_set_i32(handle, key, *(int32_t*)value);
            break;
        case CONFIG_TYPE_FLOAT:
            ret = nvs_set_blob(handle, key, value, sizeof(float));
            break;
        case CONFIG_TYPE_STRING:
            ret = nvs_set_str(handle, key, (char*)value);
            break;
        case CONFIG_TYPE_BLOB: {
            ESP_LOGE(TAG, "Blob type not directly supported in set_value_in_nvs. Use nvs_set_blob directly.");
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        default:
            ESP_LOGE(TAG, "Unsupported config data type for key: %s", key);
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    if (ret == ESP_ERR_NVS_TYPE_MISMATCH) {
        ESP_LOGW(TAG, "Type mismatch for key %s, erasing and retrying", key);
        esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err == ESP_OK) {
            switch (type) {
                case CONFIG_TYPE_U8:
                    ret = nvs_set_u8(handle, key, *(uint8_t*)value);
                    break;
                case CONFIG_TYPE_INT32:
                    ret = nvs_set_i32(handle, key, *(int32_t*)value);
                    break;
                case CONFIG_TYPE_FLOAT:
                    ret = nvs_set_blob(handle, key, value, sizeof(float));
                    break;
                case CONFIG_TYPE_STRING:
                    ret = nvs_set_str(handle, key, (char*)value);
                    break;
                case CONFIG_TYPE_BLOB:
                    ret = ESP_ERR_NOT_SUPPORTED;
                    break;
                default:
                    ret = ESP_ERR_INVALID_ARG;
                    break;
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Retry failed for key %s after erase", key);
            }
        } else {
            ESP_LOGE(TAG, "Failed to erase key %s after type mismatch: %s", key, esp_err_to_name(erase_err));
            ret = erase_err;
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set value for key: %s (%s)", key, esp_err_to_name(ret));
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes for key %s (%s)", key, esp_err_to_name(err));
        if (ret == ESP_OK) {
            ret = err;
        }
    }

    nvs_close(handle);
    return ret;
}
