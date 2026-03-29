#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "sht3x.h"
#include "i2cdev.h"
#include "lvgl.h"

#include "sht31_service.h"

static const char *TAG = "SHT31";

#define I2C_PORT    0
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 21

static sht3x_t s_sht3x_dev;
static SemaphoreHandle_t s_reading_mutex = NULL;
static sht31_reading_t s_latest_reading = {0};
static sht31_service_config_t s_cfg = {
    .enable_ui_label_update = true,
    .read_period_ms = 2000,
    .ui_update_period_ms = 500,
};

static sht31_reading_cb_t s_reading_cb = NULL;
static void *s_reading_cb_ctx = NULL;

extern SemaphoreHandle_t p_gui_semaphore;
extern lv_obj_t *ui_temp;
extern lv_obj_t *ui_Vlagavalue;

static esp_err_t sht31_i2c_sensor_init(void)
{
    esp_err_t ret;
    float probe_temp = 0.0f;
    float probe_hum = 0.0f;

    memset(&s_sht3x_dev, 0, sizeof(s_sht3x_dev));

    ret = i2cdev_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sht3x_init_desc(&s_sht3x_dev, SHT3X_I2C_ADDR_GND, I2C_PORT, I2C_SDA_PIN, I2C_SCL_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sht3x_init_desc failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sht3x_init(&s_sht3x_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sht3x_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sht3x_measure(&s_sht3x_dev, &probe_temp, &probe_hum);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sht3x probe failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SHT3x ready on I2C%d (SDA=%d, SCL=%d)", I2C_PORT, I2C_SDA_PIN, I2C_SCL_PIN);
    return ESP_OK;
}

void sht31_service_set_callback(sht31_reading_cb_t cb, void *user_ctx)
{
    s_reading_cb = cb;
    s_reading_cb_ctx = user_ctx;
}

esp_err_t sht31_service_get_latest(sht31_reading_t *out_reading)
{
    if (out_reading == NULL) return ESP_ERR_INVALID_ARG;
    if (s_reading_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (pdTRUE != xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    *out_reading = s_latest_reading;
    xSemaphoreGive(s_reading_mutex);
    return ESP_OK;
}

static void sht31_read_task(void *pvParameters)
{
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(100));

    for (;;) {
        float temperature = 0.0f;
        float humidity = 0.0f;
        esp_err_t ret = sht3x_measure(&s_sht3x_dev, &temperature, &humidity);

        if (ret == ESP_OK) {
            if (pdTRUE == xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(200))) {
                s_latest_reading.temperature_c = temperature;
                s_latest_reading.humidity_percent = humidity;
                s_latest_reading.sequence++;
                sht31_reading_t snapshot = s_latest_reading;
                xSemaphoreGive(s_reading_mutex);

                ESP_LOGI(TAG, "Temp: %.2f C, Humidity: %.2f %%", temperature, humidity);
                if (s_reading_cb != NULL) {
                    s_reading_cb(&snapshot, s_reading_cb_ctx);
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to read sensor: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.read_period_ms));
    }
}

static void sht31_ui_task(void *pvParameters)
{
    (void)pvParameters;

    char temp_buf[32];
    char humidity_buf[32];

    for (;;) {
        sht31_reading_t reading;
        if (sht31_service_get_latest(&reading) == ESP_OK) {
            snprintf(temp_buf, sizeof(temp_buf), "%.1f C", reading.temperature_c);
            snprintf(humidity_buf, sizeof(humidity_buf), "%.1f %%", reading.humidity_percent);

            if (p_gui_semaphore != NULL && (ui_temp != NULL || ui_Vlagavalue != NULL)) {
                if (pdTRUE == xSemaphoreTake(p_gui_semaphore, pdMS_TO_TICKS(200))) {
                    if (ui_temp != NULL && lv_obj_is_valid(ui_temp)) {
                        lv_label_set_text(ui_temp, temp_buf);
                    }
                    if (ui_Vlagavalue != NULL && lv_obj_is_valid(ui_Vlagavalue)) {
                        lv_label_set_text(ui_Vlagavalue, humidity_buf);
                    }
                    xSemaphoreGive(p_gui_semaphore);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.ui_update_period_ms));
    }
}

esp_err_t sht31_service_start(const sht31_service_config_t *config)
{
    if (config != NULL) {
        s_cfg = *config;
    }

    if (s_reading_mutex == NULL) {
        s_reading_mutex = xSemaphoreCreateMutex();
        if (s_reading_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create reading mutex");
            return ESP_FAIL;
        }
    }

    esp_err_t ret = sht31_i2c_sensor_init();
    if (ret != ESP_OK) {
        return ret;
    }

    BaseType_t ok = xTaskCreate(sht31_read_task, "sht31_read", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SHT31 read task");
        return ESP_FAIL;
    }

    if (s_cfg.enable_ui_label_update) {
        ok = xTaskCreate(sht31_ui_task, "sht31_ui", 4096, NULL, 5, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SHT31 UI task");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}
