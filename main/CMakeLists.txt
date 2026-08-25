idf_component_register(SRCS "app_main.c" "wifi.c" "improv.c" "led.c" "utils.c" "HX711.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_gpio esp_wifi esp_partition mbedtls esp_http_server esp_http_client esp_system nvs_flash network_provisioning led_strip mbedtls efuse cjson esp_adc esp_driver_usb_serial_jtag
                    EMBED_TXTFILES web/index.html
                    EMBED_TXTFILES web/style.css
                    EMBED_TXTFILES web/wifi.html
                    EMBED_TXTFILES web/refresh_24dp.svg
                    EMBED_TXTFILES web/visibility_24dp.svg)
