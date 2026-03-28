//--------------------------------- INCLUDES ----------------------------------
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "gui.h"
#include "lvgl.h"
#include "sht3x.h"
#include "i2cdev.h"

//---------------------------------- MACROS -----------------------------------

static const char *TAG = "SHT31";

// I2C configuration
#define I2C_PORT        0
#define I2C_SDA_PIN     22
#define I2C_SCL_PIN     21
#define I2C_FREQ_HZ     100000  // 100kHz for SHT3x compatibility

//-------------------------------- GLOBAL DATA --------------------------------

extern SemaphoreHandle_t p_gui_semaphore; 
float current_temp = 0.0;
extern lv_obj_t *ui_temp;
static sht3x_t sht3x_dev;  // SHT3x sensor device handle

//------------------------------ INITIALIZATION FUNCTIONS ----------------------

/**
 * @brief Initialize I2C Master and SHT3x sensor
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t i2c_and_sensor_init(void)
{
    esp_err_t ret;
    float probe_temp = 0.0f;
    float probe_hum = 0.0f;

    memset(&sht3x_dev, 0, sizeof(sht3x_t));

    ret = i2cdev_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Initializing SHT3x sensor on I2C port %d (SDA: GPIO%d, SCL: GPIO%d, Speed: %d Hz)",
             I2C_PORT, I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);

    // Initialize the SHT3x sensor descriptor
    // sht3x_init_desc(dev, addr, port, sda_gpio, scl_gpio)
    ret = sht3x_init_desc(&sht3x_dev, SHT3X_I2C_ADDR_GND, I2C_PORT, I2C_SDA_PIN, I2C_SCL_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init descriptor failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sht3x_init(&sht3x_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Probe the sensor to verify communication
    ret = sht3x_measure(&sht3x_dev, &probe_temp, &probe_hum);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SHT3x probe failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SHT3x sensor initialized successfully");
    return ESP_OK;
}

static void sht31_task(void *pvParameters)
{
    // The I2C bus and sensor are initialized in app_main() before this task starts
    // Wait a moment to ensure everything is ready
    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        esp_err_t ret;
        float temperature, humidity;

        // Read temperature and humidity using the esp-idf-lib library
        ret = sht3x_measure(&sht3x_dev, &temperature, &humidity);

        if (ret == ESP_OK) {
            current_temp = temperature;
            ESP_LOGI(TAG, "Temp: %.2f °C, Humidity: %.2f %%", temperature, humidity);
        } else {
            ESP_LOGE(TAG, "Failed to read sensor: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // Read every 2 seconds
    }
}
void ui_update_task(void *pvParameters) {
    char buf[32]; 

    while (1) {
        snprintf(buf, sizeof(buf), "%.1f °C", current_temp);

        if (p_gui_semaphore == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (xSemaphoreTake(p_gui_semaphore, portMAX_DELAY) == pdTRUE) {
            
            // We no longer create the label. We just check if the SquareLine 
            // label is ready, and if so, we update its text!
            if (ui_temp != NULL && lv_obj_is_valid(ui_temp)) {
                lv_label_set_text(ui_temp, buf);
            }
            
            xSemaphoreGive(p_gui_semaphore);
        }

        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
}
void app_main(void)
{
    // Initialize I2C and SHT3x sensor FIRST - CRITICAL for sensor communication
    // This must be done before starting any sensor-dependent tasks
    esp_err_t ret = i2c_and_sensor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C/SHT3x: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Halting - cannot communicate with SHT3x sensor");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Start GUI (This initializes LVGL, SPI for display/touch, etc.)
    gui_init();
    
    // Give the GUI a moment to fully initialize
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Now start sensor tasks on the initialized I2C bus
    xTaskCreate(sht31_task, "sht31_task", 4096, NULL, 5, NULL);
    xTaskCreate(ui_update_task, "ui_upd", 4096, NULL, 5, NULL);
}