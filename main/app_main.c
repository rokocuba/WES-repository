//--------------------------------- INCLUDES ----------------------------------
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "driver/uart.h"

#include "gui.h"
#include "ui.h"
#include "camera_capture.h"
#include "sht31_service.h"

static const char *TAG_MAIN = "MAIN";

/* ML UART protocol:
 * INFER_JPEG\n
 * <jpeg_size>\n
 * <jpeg bytes>
 *
 * Response examples:
 * RESULT <digit> <confidence>\n
 * ERR <reason>\n
 */
#define ML_UART_PORT            UART_NUM_2
#define ML_UART_BAUD_RATE       115200
#define ML_UART_TX_PIN          14
#define ML_UART_RX_PIN          2
#define ML_UART_RX_BUF_SIZE     2048
#define ML_UART_TX_BUF_SIZE     2048
#define ML_UART_LINE_MAX        96
#define ML_UART_READ_TIMEOUT_MS 100
#define ML_UART_REPLY_TIMEOUT_MS 3000

static bool s_ml_uart_ready = false;

extern SemaphoreHandle_t p_gui_semaphore;

static void update_result_label_from_reply(const char *reply_line) {
    int predicted_digit = -1;
    float confidence = 0.0f;

    if (reply_line == NULL) {
        return;
    }

    if (sscanf(reply_line, "RESULT %d %f", &predicted_digit, &confidence) != 2) {
        return;
    }

    if (predicted_digit < 0 || predicted_digit > 9) {
        ESP_LOGW(TAG_MAIN, "ML predicted digit out of range: %d", predicted_digit);
        return;
    }

    if (p_gui_semaphore == NULL || ui_Label2 == NULL) {
        ESP_LOGW(TAG_MAIN, "Number label not ready; skipping UI update");
        return;
    }

    if (pdTRUE == xSemaphoreTake(p_gui_semaphore, pdMS_TO_TICKS(200))) {
        char label_text[4];
        snprintf(label_text, sizeof(label_text), "%d", predicted_digit);
        lv_label_set_text(ui_Label2, label_text);
        lv_obj_invalidate(ui_Label2);
        xSemaphoreGive(p_gui_semaphore);
        ESP_LOGI(TAG_MAIN, "Updated ui_Label2 to %d (conf=%.3f)", predicted_digit, confidence);
    } else {
        ESP_LOGW(TAG_MAIN, "Timeout acquiring GUI semaphore for label update");
    }
}

static bool ml_uart_write_all(const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        int written = uart_write_bytes(ML_UART_PORT, (const char *)(ptr + sent), len - sent);
        if (written <= 0) {
            return false;
        }
        sent += (size_t)written;
    }

    return true;
}

static bool ml_uart_read_line(char *line_out, size_t line_out_size, uint32_t timeout_ms) {
    if (line_out == NULL || line_out_size < 2) {
        return false;
    }

    size_t pos = 0;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        uint8_t c = 0;
        int rd = uart_read_bytes(ML_UART_PORT, &c, 1, pdMS_TO_TICKS(ML_UART_READ_TIMEOUT_MS));
        if (rd <= 0) {
            elapsed_ms += ML_UART_READ_TIMEOUT_MS;
            continue;
        }

        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line_out[pos] = '\0';
            return true;
        }

        if (pos < (line_out_size - 1)) {
            line_out[pos++] = (char)c;
        }
    }

    line_out[0] = '\0';
    return false;
}

static bool ml_uart_wait_for_exact_line(const char *expected, uint32_t timeout_ms) {
    char line[ML_UART_LINE_MAX];
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        uint32_t step_ms = (remaining_ms > 500U) ? 500U : remaining_ms;

        if (!ml_uart_read_line(line, sizeof(line), step_ms)) {
            elapsed_ms += step_ms;
            continue;
        }

        if (strcmp(line, expected) == 0) {
            return true;
        }

        if (line[0] != '\0') {
            ESP_LOGW(TAG_MAIN, "ML unexpected line while waiting '%s': %s", expected, line);
        }
    }

    return false;
}

static bool ml_uart_wait_for_result_line(char *out_line, size_t out_line_size, uint32_t timeout_ms) {
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        uint32_t step_ms = (remaining_ms > 500U) ? 500U : remaining_ms;

        if (!ml_uart_read_line(out_line, out_line_size, step_ms)) {
            elapsed_ms += step_ms;
            continue;
        }

        if (strncmp(out_line, "RESULT ", 7) == 0 || strncmp(out_line, "ERR ", 4) == 0) {
            return true;
        }

        if (out_line[0] != '\0') {
            ESP_LOGW(TAG_MAIN, "ML non-terminal line: %s", out_line);
        }
    }

    return false;
}

static esp_err_t ml_uart_init(void) {
    if (s_ml_uart_ready) {
        return ESP_OK;
    }

    uart_config_t cfg = {
        .baud_rate = ML_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION_MAJOR >= 5
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };

    esp_err_t err = uart_driver_install(ML_UART_PORT, ML_UART_RX_BUF_SIZE, ML_UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_MAIN, "ML UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(ML_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "ML UART param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(ML_UART_PORT, ML_UART_TX_PIN, ML_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "ML UART pin config failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_flush_input(ML_UART_PORT);
    s_ml_uart_ready = true;
    ESP_LOGI(TAG_MAIN, "ML UART ready on UART%d (TX=%d RX=%d)", ML_UART_PORT, ML_UART_TX_PIN, ML_UART_RX_PIN);
    return ESP_OK;
}

static void send_frame_to_ml_node(const camera_capture_frame_t *frame) {
    if (!s_ml_uart_ready || frame == NULL || frame->data == NULL || frame->size == 0) {
        return;
    }

    /* Clear any stale protocol lines from previous exchanges. */
    uart_flush_input(ML_UART_PORT);

    static const char infer_cmd[] = "INFER_JPEG\n";
    if (!ml_uart_write_all(infer_cmd, sizeof(infer_cmd) - 1U)) {
        ESP_LOGE(TAG_MAIN, "Failed to send ML command");
        return;
    }

    if (!ml_uart_wait_for_exact_line("READY_SIZE", ML_UART_REPLY_TIMEOUT_MS)) {
        ESP_LOGW(TAG_MAIN, "ML timeout waiting READY_SIZE");
        return;
    }

    char size_line[24];
    int size_len = snprintf(size_line, sizeof(size_line), "%u\n", (unsigned)frame->size);
    if (size_len <= 0) {
        ESP_LOGE(TAG_MAIN, "Failed to format ML size line");
        return;
    }

    if (!ml_uart_write_all(size_line, (size_t)size_len)) {
        ESP_LOGE(TAG_MAIN, "Failed to send ML size line");
        return;
    }

    if (!ml_uart_wait_for_exact_line("READY_DATA", ML_UART_REPLY_TIMEOUT_MS)) {
        ESP_LOGW(TAG_MAIN, "ML timeout waiting READY_DATA");
        return;
    }

    if (!ml_uart_write_all(frame->data, frame->size)) {
        ESP_LOGE(TAG_MAIN, "Failed to send ML JPEG payload (%u bytes)", (unsigned)frame->size);
        return;
    }

    uart_wait_tx_done(ML_UART_PORT, pdMS_TO_TICKS(1000));

    char reply[ML_UART_LINE_MAX];
    if (!ml_uart_wait_for_result_line(reply, sizeof(reply), ML_UART_REPLY_TIMEOUT_MS)) {
        ESP_LOGW(TAG_MAIN, "ML node timeout waiting for reply");
        return;
    }

    if (strncmp(reply, "RESULT ", 7) == 0) {
        ESP_LOGI(TAG_MAIN, "ML %s", reply);
        update_result_label_from_reply(reply);
    } else if (strncmp(reply, "ERR ", 4) == 0) {
        ESP_LOGW(TAG_MAIN, "ML %s", reply);
    } else {
        ESP_LOGW(TAG_MAIN, "ML unknown reply: %s", reply);
    }
}

static void on_camera_frame(const camera_capture_frame_t *frame, void *user_ctx) {
    (void)user_ctx;

    ESP_LOGI(TAG_MAIN,
             "Frame #%lu: %ux%u, size=%u bytes, components=%d",
             (unsigned long)frame->sequence,
             frame->width,
             frame->height,
             (unsigned)frame->size,
             frame->components);

    send_frame_to_ml_node(frame);
}

void app_main() {
    if (ml_uart_init() != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "ML UART init failed, continuing without ML forwarding");
    }

    gui_init();

    sht31_service_config_t sht_cfg = SHT31_SERVICE_DEFAULT_CONFIG();
    sht_cfg.enable_ui_label_update = true;
    if (sht31_service_start(&sht_cfg) != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "SHT31 service failed to initialize.");
    }

    camera_capture_config_t cfg = CAMERA_CAPTURE_DEFAULT_CONFIG();
    cfg.enable_display = true;         // Keep LVGL preview enabled
    cfg.enable_button_trigger = true;  // Capture when GPIO36 button is pressed

    // Register frame callback for custom processing/analytics/storage.
    camera_capture_set_frame_callback(on_camera_frame, NULL);

    if (camera_capture_start(&cfg) != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "Halting program. Capture module failed to initialize.");
        return;
    }

}