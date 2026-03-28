#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
    uint32_t sequence;
} sht31_reading_t;

typedef void (*sht31_reading_cb_t)(const sht31_reading_t *reading, void *user_ctx);

typedef struct {
    bool enable_ui_label_update;
    uint32_t read_period_ms;
    uint32_t ui_update_period_ms;
} sht31_service_config_t;

#define SHT31_SERVICE_DEFAULT_CONFIG() \
    (sht31_service_config_t){ \
        .enable_ui_label_update = true, \
        .read_period_ms = 2000, \
        .ui_update_period_ms = 500, \
    }

void sht31_service_set_callback(sht31_reading_cb_t cb, void *user_ctx);
esp_err_t sht31_service_start(const sht31_service_config_t *config);
esp_err_t sht31_service_get_latest(sht31_reading_t *out_reading);
