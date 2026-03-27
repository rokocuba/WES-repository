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

// Use the legacy driver to match the Byte-Lab template!
#include "driver/i2c.h"

//---------------------------------- MACROS -----------------------------------

static const char *TAG = "SHT31_LEGACY";

// I2C configuration - MUST use GPIO 21 (SDA) and GPIO 22 (SCL) for standard ESP32
#define I2C_PORT        0
#define I2C_SDA_PIN     22
#define I2C_SCL_PIN     21
#define I2C_FREQ_HZ     100000  // 100kHz for SHT31 compatibility
#define SHT31_ADDR      0x44    // Default I2C address for SHT31

//-------------------------------- GLOBAL DATA --------------------------------

extern SemaphoreHandle_t p_gui_semaphore; 
float current_temp = 0.0;
extern lv_obj_t *ui_temp; 

//------------------------------ INITIALIZATION FUNCTIONS ----------------------

/**
 * @brief Initialize I2C Master for SHT31 sensor
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Install the I2C driver
    // Using 0, 0 for RX/TX buffer sizes (disabled) as per legacy driver
    ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C Master initialized on port %d (SDA: GPIO%d, SCL: GPIO%d)",
             I2C_PORT, I2C_SDA_PIN, I2C_SCL_PIN);
    return ESP_OK;
}

static void sht31_task(void *pvParameters)
{
    // The I2C bus is initialized in app_main() before this task starts
    // Wait a moment to ensure I2C initialization is complete
    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        esp_err_t ret;
        i2c_cmd_handle_t cmd;

        // --- 1. SEND MEASUREMENT COMMAND (0x24, 0x00) ---
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (SHT31_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, 0x24, true); // Command MSB
        i2c_master_write_byte(cmd, 0x00, true); // Command LSB
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(1000));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            // Give the sensor ~15ms to perform the measurement
            vTaskDelay(pdMS_TO_TICKS(20));

            // --- 2. READ THE 6 BYTES OF DATA ---
            uint8_t data[6];
            cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (SHT31_ADDR << 1) | I2C_MASTER_READ, true);
            i2c_master_read(cmd, data, 5, I2C_MASTER_ACK);      // ACK the first 5 bytes
            i2c_master_read_byte(cmd, data + 5, I2C_MASTER_NACK); // NACK the last byte
            i2c_master_stop(cmd);
            
            ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(1000));
            i2c_cmd_link_delete(cmd);

            if (ret == ESP_OK) {
                // --- 3. CALCULATE TEMP AND HUMIDITY ---
                uint16_t raw_temp = (data[0] << 8) | data[1];
                uint16_t raw_hum = (data[3] << 8) | data[4];
                
                float temperature = -45.0 + (175.0 * raw_temp / 65535.0);
                float humidity = 100.0 * raw_hum / 65535.0;
                
                current_temp = temperature;
                ESP_LOGI(TAG, "Temp: %.2f °C, Humidity: %.2f %%", temperature, humidity);
            } else {
                ESP_LOGE(TAG, "Failed to read data from sensor");
            }
        } else {
            ESP_LOGE(TAG, "Failed to send measurement command");
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
    // Initialize I2C FIRST - CRITICAL for SHT31 sensor communication
    // This must be done before starting any I2C-dependent tasks
    esp_err_t ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Halting - cannot communicate with SHT31 sensor");
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