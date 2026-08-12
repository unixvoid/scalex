#include "config.h"
#include "led.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <nvs_flash.h>
#include <ctype.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_sntp.h>
#include <esp_netif_sntp.h>
#include <esp_mac.h>
#include <mbedtls/md.h>

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

bool have_endpoint_id() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Failed to open NVS handle!");
        return false;
    }
    
    char endpoint_id[32];
    size_t len = sizeof(endpoint_id);
    err = nvs_get_str(nvs_handle, "endpoint_id", endpoint_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || err != ESP_OK) {
        ESP_LOGI(TAG, "Endpoint ID not found in NVS.");
        nvs_close(nvs_handle);
        return false;
    }
    
    char pairing_key[128];
    len = sizeof(pairing_key);
    err = nvs_get_str(nvs_handle, "pairing_key", pairing_key, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || err != ESP_OK) {
        ESP_LOGI(TAG, "Pairing key not found in NVS.");
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    return true;
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

void hard_reset() {
    static const char *NVS_NAMESPACE = "config";
    static const char *SCALE_FACTOR_KEY = "scale_factor";

    ESP_LOGW(TAG, "Hard reset initiated! (Wi-Fi + endpoint credentials erased, scale_factor retained)");

    // 1. Read the scale_factor from NVS
    float scale_factor_value = 1.0f; // Default value
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t size = sizeof(float);
        esp_err_t read_err = nvs_get_blob(nvs_handle, SCALE_FACTOR_KEY, &scale_factor_value, &size);
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Error reading scale_factor: %s", esp_err_to_name(read_err));
        } else if (read_err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "scale_factor not found in NVS, using default: %f", scale_factor_value);
        } else if (size != sizeof(float)) {
            ESP_LOGW(TAG, "Warning: Size of scale_factor blob mismatch!");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Error opening NVS for reading: %s", esp_err_to_name(err));
    }

    // 2. Erase all NVS data
    ESP_LOGW(TAG, "Erasing all stored data (except scale_factor)...");
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing NVS flash: %s", esp_err_to_name(err));
    }

    // 3. Initialize NVS again
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing NVS flash after erase: %s", esp_err_to_name(err));
        return; // Abort if initialization fails
    }

    // 4. Write the scale_factor back to NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, SCALE_FACTOR_KEY, &scale_factor_value, sizeof(float));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error writing scale_factor back to NVS: %s", esp_err_to_name(err));
        }
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error committing scale_factor to NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Error opening NVS for writing: %s", esp_err_to_name(err));
    }

    // 5. Verify — read scale_factor back to confirm it survived the erase+restore cycle.
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        float verify_value = 0.0f;
        size_t verify_size = sizeof(float);
        esp_err_t verify_err = nvs_get_blob(nvs_handle, SCALE_FACTOR_KEY, &verify_value, &verify_size);
        if (verify_err == ESP_OK) {
            ESP_LOGW(TAG, "VERIFIED: scale_factor preserved after reset = %.6f (was %.6f before erase)",
                     verify_value, scale_factor_value);
        } else {
            ESP_LOGE(TAG, "FAILED to verify scale_factor after reset: %s", esp_err_to_name(verify_err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Error opening NVS for scale_factor verification: %s", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "Hard reset complete (scale_factor retained). Restarting...");
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_restart();
}

void get_device_service_name(char *service_name, size_t max)
{
    // Get base MAC address (factory programmed, globally unique)
    uint8_t base_mac[6];
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    
    // Convert MAC to 48-bit integer
    uint64_t mac_value = 0;
    for (int i = 0; i < 6; i++) {
        mac_value = (mac_value << 8) | base_mac[i];
    }
    
    // Encode to base36 (0-9, a-z)
    const char base36_chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char encoded[11];  // 10 chars + null terminator
    encoded[10] = '\0';
    
    for (int i = 9; i >= 0; i--) {
        encoded[i] = base36_chars[mac_value % 36];
        mac_value /= 36;
    }
    
    // Log hardware information
    ///ESP_LOGI(TAG, "Hardware ID Generation:");
    ///ESP_LOGI(TAG, "  Base MAC: %02X:%02X:%02X:%02X:%02X:%02X",
    ///         base_mac[0], base_mac[1], base_mac[2],
    ///         base_mac[3], base_mac[4], base_mac[5]);
    ///ESP_LOGI(TAG, "  Base36 Encoding: %s", encoded);
    
    // Format with product prefix
    const char *ssid_prefix = "scale-";
    snprintf(service_name, max, "%s%s", ssid_prefix, encoded);
    
    //ESP_LOGI(TAG, "  Device ID: %s", service_name);
}

void generate_custom_service_uuid(uint8_t *uuid_out)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac); // Get unique chip MAC address

    // Copy first 6 bytes of MAC into UUID for uniqueness
    memcpy(uuid_out, mac, 6);

    // Fill the rest with some fixed values and format as a proper UUID
    uuid_out[6] = 0x40 | (uuid_out[6] & 0x0F); // Set UUID version (4)
    uuid_out[8] = 0x80 | (uuid_out[8] & 0x3F); // Set UUID variant

    ESP_LOGI("UUID", "Generated UUID: %02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid_out[0], uuid_out[1], uuid_out[2], uuid_out[3],
             uuid_out[4], uuid_out[5], uuid_out[6], uuid_out[7],
             uuid_out[8], uuid_out[9], uuid_out[10], uuid_out[11],
             uuid_out[12], uuid_out[13], uuid_out[14], uuid_out[15]);
}

// Function to hash the pairing key using SHA-256
esp_err_t hash_secret(const char *secret, uint8_t *out_hash) {
    mbedtls_md_context_t sha_ctx;
    const mbedtls_md_info_t *md_info;
    mbedtls_md_init(&sha_ctx);

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        mbedtls_md_free(&sha_ctx);
        return ESP_FAIL;
    }

    if (mbedtls_md_setup(&sha_ctx, md_info, 0) != 0) {
        mbedtls_md_free(&sha_ctx);
        return ESP_FAIL;
    }

    if (mbedtls_md_starts(&sha_ctx) != 0 ||
        mbedtls_md_update(&sha_ctx, (const unsigned char *)secret, strlen(secret)) != 0 ||
        mbedtls_md_finish(&sha_ctx, out_hash) != 0) {
        mbedtls_md_free(&sha_ctx);
        return ESP_FAIL;
    }

    mbedtls_md_free(&sha_ctx);
    return ESP_OK;
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

static bool g_ntp_synced = false;

bool is_ntp_synced() {
    return g_ntp_synced;
}

esp_err_t sync_time_with_ntp() {
    ESP_LOGI(TAG, "Initializing SNTP client..");

    // Configure SNTP with default settings
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.smooth_sync = false;  // Use step sync (faster)
    
    esp_netif_sntp_init(&config);

    setenv("TZ", "UTC", 1);
    tzset();

    // Wait for time to be set using the proper high-level API
    ESP_LOGI(TAG, "Waiting for SNTP time sync...");
    int retry = 0;
    const int retry_count = 15;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }

    // Check if time was synced
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    
    // Valid time should be after 2020
    if (tm_info->tm_year < (2020 - 1900)) {
        ESP_LOGE(TAG, "Failed to sync time with NTP server - time still unset");
        esp_netif_sntp_deinit();
        return ESP_FAIL;
    }

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", tm_info);
    ESP_LOGI(TAG, "Time synced successfully: %s", time_str);
    
    g_ntp_synced = true;
    return ESP_OK;
}

void ntp_sync_task(void *pvParameter) {
    esp_err_t ntp_err = sync_time_with_ntp();

    if (ntp_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sync time with NTP server.");
    } else {
        ESP_LOGI(TAG, "Time synced successfully.");
    }

    vTaskDelete(NULL); // Clean up the task when done
}
