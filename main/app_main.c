//--------------------------------- INCLUDES ----------------------------------
#include <stdio.h>
#include <stdlib.h>
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

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

// Define your UART port and pins
#define UART_PORT      UART_NUM_1
#define UART_BAUD_RATE 115200
#define UART_TX_PIN    27
#define UART_RX_PIN    26
#define BUF_SIZE       1024

#define CAPTURE_BUTTON_GPIO         GPIO_NUM_36
#define CAPTURE_BUTTON_ACTIVE_LEVEL 1
#define BUTTON_POLL_MS              20
#define BUTTON_DEBOUNCE_MS          40

// Match camera side FRAMESIZE_QQVGA (esp32-camera)
#define CAM_FRAME_WIDTH   160
#define CAM_FRAME_HEIGHT  120

// Define a tag so you know exactly where the log is coming from
static const char *TAG_UART = "UART_INIT";

// Changed from void to esp_err_t so we can report success/failure back to main
esp_err_t init_uart1(void) {
    ESP_LOGI(TAG_UART, "Starting UART1 initialization...");

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    // 1. Configure parameters and check for errors
    ESP_LOGI(TAG_UART, "Configuring UART parameters (Baud: %d)...", UART_BAUD_RATE);
    esp_err_t err = uart_param_config(UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UART, "Failed to configure UART parameters: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Set pins and check for errors
    ESP_LOGI(TAG_UART, "Setting UART pins (TX: %d, RX: %d)...", UART_TX_PIN, UART_RX_PIN);
    err = uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UART, "Failed to set UART pins: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Install driver and check for errors
    ESP_LOGI(TAG_UART, "Installing UART driver (Buffer: %d bytes)...", BUF_SIZE * 2);
    err = uart_driver_install(UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UART, "Failed to install UART driver: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_UART, "UART1 successfully initialized and ready!");
    return ESP_OK;
}

static const char *TAG_REQUESTER = "REQUESTER";
static const char *TAG_BUTTON = "CAPTURE_BTN";
// Match what the camera actually sends
const char* FRAME_START = "START\n";
const char* FRAME_END   = "END\n";
const char* REQUEST_COMMAND = "SEND_PIC\n";

static esp_err_t init_capture_button(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CAPTURE_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BUTTON, "Failed to configure capture button GPIO: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_BUTTON,
             "Capture button ready on GPIO %d (active level: %d)",
             CAPTURE_BUTTON_GPIO,
             CAPTURE_BUTTON_ACTIVE_LEVEL);
    return ESP_OK;
}

static bool is_valid_jpeg_buffer(const uint8_t *buf, size_t len) {
    if (buf == NULL || len < 4) return false;
    return (buf[0] == 0xFF && buf[1] == 0xD8 &&
            buf[len - 2] == 0xFF && buf[len - 1] == 0xD9);
}

static void log_jpeg_signature(const uint8_t *buf, size_t len) {
    if (buf == NULL || len < 12) return;
    ESP_LOGI(TAG_REQUESTER,
             "JPEG head: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
             buf[6], buf[7], buf[8], buf[9], buf[10], buf[11]);
}

static bool probe_lvgl_jpeg(const uint8_t *buf, size_t len, lv_coord_t *w, lv_coord_t *h) {
    lv_img_dsc_t probe = {0};
    lv_img_header_t hdr;

    probe.header.cf = LV_IMG_CF_RAW;
    probe.data = buf;
    probe.data_size = len;

    if (lv_img_decoder_get_info(&probe, &hdr) != LV_RES_OK) {
        return false;
    }

    if (w) *w = hdr.w;
    if (h) *h = hdr.h;
    return true;
}

static int jpeg_component_count(const uint8_t *buf, size_t len) {
    if (buf == NULL || len < 8) return -1;

    size_t i = 2; // Skip SOI
    while (i + 4 < len) {
        if (buf[i] != 0xFF) {
            i++;
            continue;
        }

        // Skip fill bytes FF FF ...
        while (i < len && buf[i] == 0xFF) i++;
        if (i >= len) break;

        uint8_t marker = buf[i++];

        // Markers without payload length
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        if (i + 1 >= len) break;
        uint16_t seg_len = ((uint16_t)buf[i] << 8) | buf[i + 1];
        if (seg_len < 2 || i + seg_len > len) break;

        // Start Of Frame markers carrying component count
        if ((marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF)) {
            // SOF payload: P(1), Y(2), X(2), Nf(1), ...
            if (seg_len >= 8) {
                return buf[i + 7];
            }
            return -1;
        }

        i += seg_len;
    }

    return -1;
}

// Reads bytes one at a time until the full marker string is found,
// or until it times out. Returns true on success.
static bool wait_for_marker(const char* marker, uint32_t timeout_ms) {
    size_t marker_len = strlen(marker);
    size_t matched = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (matched < marker_len) {
        // Check for overall timeout
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_LOGE(TAG_REQUESTER, "Timeout waiting for marker: %s", marker);
            return false;
        }

        uint8_t c;
        // Short per-byte timeout so the loop can check the overall timeout
        int len = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(50));
        if (len <= 0) continue; // No byte yet, loop and check overall timeout

        if (c == (uint8_t)marker[matched]) {
            matched++; // Got the next expected byte
        } else {
            matched = 0; // Wrong byte, reset and try again
            // Re-check first char in case this byte starts the marker
            if (c == (uint8_t)marker[0]) matched = 1;
        }
    }
    return true;
}

// At the top of your file, add this descriptor (static so it lives after the function returns)
static lv_img_dsc_t jpeg_descriptor;

extern lv_obj_t* ui_camImage; // This is the name you gave the image widget in SquareLine
extern SemaphoreHandle_t p_gui_semaphore; // This is the semaphore you created in gui.c

static bool wait_for_gui_ready(uint32_t timeout_ms) {
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        if (p_gui_semaphore != NULL && ui_camImage != NULL) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return false;
}

// After "Image successfully received!", replace the free() with this:
void display_received_image(uint8_t* image_buffer, size_t image_size) {
    static uint8_t* current_image_buffer = NULL;
    lv_coord_t decoded_w = CAM_FRAME_WIDTH;
    lv_coord_t decoded_h = CAM_FRAME_HEIGHT;

    if (!probe_lvgl_jpeg(image_buffer, image_size, &decoded_w, &decoded_h)) {
        ESP_LOGE(TAG_REQUESTER, "LVGL rejected JPEG decode probe; keeping previous image");
        log_jpeg_signature(image_buffer, image_size);
        free(image_buffer);
        return;
    }

    int components = jpeg_component_count(image_buffer, image_size);
    if (components > 0) {
        ESP_LOGI(TAG_REQUESTER, "JPEG component count: %d", components);
        if (components == 1) {
            ESP_LOGW(TAG_REQUESTER, "Camera is sending grayscale JPEG (1 component)");
        }
    }

    ESP_LOGI(TAG_REQUESTER, "LVGL JPEG probe OK: %dx%d", (int)decoded_w, (int)decoded_h);

    size_t decoder_cache_bytes = (size_t)decoded_w * (size_t)decoded_h * 3U;
    size_t estimated_total_bytes = decoder_cache_bytes + 8192U;
    ESP_LOGI(TAG_REQUESTER,
             "LVGL decode memory estimate: cache=%zu total~%zu, LV_MEM_SIZE=%u",
             decoder_cache_bytes,
             estimated_total_bytes,
             (unsigned int)LV_MEM_SIZE);
    if (estimated_total_bytes > (size_t)LV_MEM_SIZE) {
        ESP_LOGW(TAG_REQUESTER,
                 "LVGL heap is too small for this JPEG resolution; image may render as gray/blank");
        ESP_LOGW(TAG_REQUESTER,
                 "Dropping frame to keep previous valid image. Lower camera frame size or increase LV_MEM_SIZE.");
        free(image_buffer);
        return;
    }

    // The descriptor pointer is reused for every frame. Invalidate cached decode results
    // so LVGL doesn't reuse stale pixels from the previous capture.
    lv_img_cache_invalidate_src(&jpeg_descriptor);

    if (current_image_buffer != NULL) {
        free(current_image_buffer);
    }
    current_image_buffer = image_buffer;

    jpeg_descriptor.header.cf          = LV_IMG_CF_RAW;
    jpeg_descriptor.header.always_zero = 0;
    jpeg_descriptor.header.reserved    = 0;
    jpeg_descriptor.header.w           = decoded_w;
    jpeg_descriptor.header.h           = decoded_h;
    jpeg_descriptor.data_size          = image_size;
    jpeg_descriptor.data               = image_buffer;

    if (p_gui_semaphore == NULL || ui_camImage == NULL) {
        ESP_LOGW(TAG_REQUESTER, "GUI not ready (semaphore/image is NULL), dropping frame");
        free(image_buffer);
        return;
    }

    // Lock LVGL before touching any widgets from a non-GUI task
    if (pdTRUE == xSemaphoreTake(p_gui_semaphore, portMAX_DELAY)) {
        lv_img_set_src(ui_camImage, &jpeg_descriptor);
        lv_obj_invalidate(ui_camImage);
        xSemaphoreGive(p_gui_semaphore);
    }
}

void request_and_receive_image() {
    // Drop any stale bytes from previous failed/incomplete transactions.
    uart_flush_input(UART_PORT);

    // 1. Send the trigger command
    uart_write_bytes(UART_PORT, REQUEST_COMMAND, strlen(REQUEST_COMMAND));
    ESP_LOGI(TAG_REQUESTER, "Requested picture...");

    // 2. Wait for the START
    // Wait up to 5 seconds for the start marker
    if (!wait_for_marker(FRAME_START, 5000)) {
        ESP_LOGE(TAG_REQUESTER, "Failed to receive start marker");
        return;
    }
    ESP_LOGI(TAG_REQUESTER, "Got start marker");


    // 3. Read the image size (Read byte-by-byte until we hit the newline '\n')
    char size_str[16] = {0};
    int i = 0;
    while (i < sizeof(size_str) - 1) {
        uint8_t c;
        if (uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(1000)) > 0) {
            size_str[i++] = c;
            if (c == '\n') {
                size_str[i] = '\0'; // Null terminate the string
                break;
            }
        } else {
            ESP_LOGE(TAG_REQUESTER, "Timeout reading size");
            return;
        }
    }
    
    // Convert the string to an integer
    size_t image_size = (size_t)atoi(size_str);
    if (image_size == 0) return;

    ESP_LOGI(TAG_REQUESTER, "Incoming image size: %zu bytes", image_size);
    ESP_LOGI(TAG_REQUESTER, "Free PSRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG_REQUESTER, "Free internal RAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // 4. Allocate memory for the image (USE PSRAM IF AVAILABLE!)
    // heap_caps_malloc ensures it goes to external RAM if you have it configured
    uint8_t* image_buffer = (uint8_t*) malloc(image_size);
    if (image_buffer == NULL) {
        ESP_LOGE(TAG_REQUESTER, "Not enough memory for image! (size: %zu, free: %zu)",
                image_size, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }

    // 5. Sync to JPEG SOI (FF D8) to survive leading UART text/noise,
    // then read exactly image_size bytes of JPEG payload.
    bool soi_found = false;
    uint8_t prev = 0;
    TickType_t soi_start_tick = xTaskGetTickCount();
    TickType_t soi_timeout_ticks = pdMS_TO_TICKS(1500);

    while (!soi_found) {
        if ((xTaskGetTickCount() - soi_start_tick) >= soi_timeout_ticks) {
            ESP_LOGE(TAG_REQUESTER, "Timeout waiting for JPEG SOI marker");
            free(image_buffer);
            return;
        }

        uint8_t c;
        int len = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(50));
        if (len <= 0) continue;

        if (prev == 0xFF && c == 0xD8) {
            image_buffer[0] = 0xFF;
            image_buffer[1] = 0xD8;
            soi_found = true;
            break;
        }
        prev = c;
    }

    size_t bytes_received = 2;
    while (bytes_received < image_size) {
        int read_len = uart_read_bytes(UART_PORT,
                                       image_buffer + bytes_received,
                                       image_size - bytes_received,
                                       pdMS_TO_TICKS(1000));
        if (read_len > 0) {
            bytes_received += (size_t)read_len;
        } else {
            ESP_LOGE(TAG_REQUESTER, "Timeout during image data transfer");
            free(image_buffer);
            return;
        }
    }

    if (!is_valid_jpeg_buffer(image_buffer, image_size)) {
        ESP_LOGE(TAG_REQUESTER,
                 "Invalid JPEG markers. SOI=%02X %02X EOI=%02X %02X (size=%zu)",
                 image_buffer[0], image_buffer[1],
                 image_buffer[image_size - 2], image_buffer[image_size - 1],
                 image_size);
        ESP_LOGE(TAG_REQUESTER,
                 "Likely UART stream corruption (often from logs mixed with binary data)");
        free(image_buffer);
        return;
    }

    // 6. Wait for the END MARKER
   if (!wait_for_marker(FRAME_END, 3000)) {
        ESP_LOGE(TAG_REQUESTER, "Failed to receive end marker");
        free(image_buffer);
    } else {
        ESP_LOGI(TAG_REQUESTER, "Image successfully received!");
        display_received_image(image_buffer, image_size); // buffer ownership passed to LVGL
    }

}

void image_task(void *pvParameters) {
    if (!wait_for_gui_ready(5000)) {
        ESP_LOGE(TAG_REQUESTER, "GUI was not ready within timeout, aborting image request");
        vTaskDelete(NULL);
        return;
    }

    int last_level = gpio_get_level(CAPTURE_BUTTON_GPIO);
    TickType_t last_press_tick = 0;
    TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    for (;;) {
        int level = gpio_get_level(CAPTURE_BUTTON_GPIO);

        if (level == CAPTURE_BUTTON_ACTIVE_LEVEL && last_level != CAPTURE_BUTTON_ACTIVE_LEVEL) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_press_tick) >= debounce_ticks) {
                last_press_tick = now;
                ESP_LOGI(TAG_BUTTON, "Button pressed, requesting picture");
                request_and_receive_image();
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}


void app_main() {
    // Initialize the camera first...
    
    // Attempt to initialize UART1
    esp_err_t uart_status = init_uart1();

    gui_init();
    
    esp_err_t button_status = init_capture_button();

    if (uart_status == ESP_OK && button_status == ESP_OK) {
        // Start button listener task; each valid press triggers one capture request.
        xTaskCreate(image_task, "image_task", 8192, NULL, 5, NULL);
    } else {
        ESP_LOGE("MAIN", "Halting program. UART or button init failed.");
        // Add error handling here, like blinking a red LED
    }
}