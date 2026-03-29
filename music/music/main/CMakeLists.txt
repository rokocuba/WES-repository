idf_component_register(
    SRCS "wes.c" "hooks.c" "hook_healthcheck.c" "hook_camera.c" "hook_audio.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer mbedtls esp_driver_i2s esp_driver_gpio
)
