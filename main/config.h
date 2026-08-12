#ifndef CONFIG_H
#define CONFIG_H

// Application settings
#define VERSION "1.0.9"

// Provisioning and network defaults
#define PROV_AP_SSID "ScaleX"
#define PROV_AP_CHANNEL 1
#define PROV_AP_MAX_CONN 4
#define MDNS_HOSTNAME "scale"
#define MDNS_INSTANCE_NAME "scale"

// GPIO definitions for new power/sense hardware
#define USB_SENSE_GPIO  GPIO_NUM_3  // VUSB → 100k → IO3 (active HIGH, internal pulldown)
#define BATT_SENSE_GPIO GPIO_NUM_1  // VBAT/2 via 2×100k divider (digital detection)
#define CAP_CONTROL_GPIO GPIO_NUM_2  // Bulk capacitor control transistor (HIGH = ON, LOW = OFF)

// Button timing constants (ms)
#define TARE_HOLD_TIME          800   // tap-to-tare maximum hold time
#define RESET_WIFI_HOLD_TIME  6000   // hold to erase Wi-Fi credentials and reboot

// HX711 load cell amp (scale) settings
#define SCALE_FACTOR 128 // scale factor for weight in grams
#define WEIGHT_THRESHOLD 50  // minimum weight of object that will trigger capture in grams
#define DETECTION_DEBOUNCE_COUNT 3  // number of consecutive readings above threshold to confirm detection
#define DEBOUNCE_COUNT 2 // number of cycles the scale must be empty before reset
#define SCALE_STABILITY_TIME 250 // ms to wait for weight stability
#define STABILITY_READS (SCALE_STABILITY_TIME / 100)  // Assuming 100ms per loop iteration
#define STABILITY_THRESHOLD 0.8  // Adjust based on scale sensitivity
#define CALIBRATION_WEIGHT 500 // weight in grams for scale calibration
#define SETTLING_ROC_THRESHOLD 1.0f // Rate-of-change threshold for settling detection (grams/poll)

// LED strip settings
#define SCALE_LED_COUNT 22
#define BRIGHTNESS_DEFAULT_SCALE 54
#define BRIGHTNESS_DEFAULT_STATUS 72
#define BRIGHTNESS_DEFAULT_GLOBAL 255
#define DEFAULT_COLOR_SCALE_R 64
#define DEFAULT_COLOR_SCALE_G 0
#define DEFAULT_COLOR_SCALE_B 128

//// // GPIO definitions - Test Device (XIAO ESP32C3)
//// #define HX711_DT_GPIO GPIO_NUM_3
//// #define HX711_SCK_GPIO GPIO_NUM_2
//// #define BUTTON_GPIO GPIO_NUM_6
//// #define SCALE_LED_GPIO GPIO_NUM_4
//// #define STATUS_LED_GPIO GPIO_NUM_5

// Production Device GPIO (ESP32-C3-Mini-1)
#define HX711_DT_GPIO GPIO_NUM_4
#define HX711_SCK_GPIO GPIO_NUM_5
#define BUTTON_GPIO GPIO_NUM_0
#define SCALE_LED_GPIO GPIO_NUM_6

// Define config types globally
typedef enum {
    CONFIG_TYPE_INT32,
    CONFIG_TYPE_FLOAT,
    CONFIG_TYPE_U8,
    CONFIG_TYPE_STRING,
    CONFIG_TYPE_BLOB
} config_data_type_t;

#endif // CONFIG_H
