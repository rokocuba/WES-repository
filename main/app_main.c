//--------------------------------- INCLUDES ----------------------------------
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_spiffs.h"

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


#define I2S_BCLK    GPIO_NUM_25 // btn4
#define I2S_LRC     GPIO_NUM_33 // btn3
#define I2S_DOUT    GPIO_NUM_32 // btn2
#define SAMPLE_RATE 22050
#define TAG         "audio"

#define DEFAULT_AUDIO_WAV_PATH "/spiffs/output.wav"
#define SIREN_WAV_PATH         "/spiffs/siren.wav"

#define ANCHOR_SSID             "FTM_Anchor"
#define ANCHOR_CHANNEL           1
#define RSSI_SCAN_PERIOD_MS     3000
#define RSSI_ALERT_DISTANCE_M   6.0f
#define RSSI_REF_DBM_AT_1M     -45.0f
#define RSSI_PATH_LOSS_FACTOR   2.2f
#define RSSI_SIREN_COOLDOWN_MS  8000

#define SIREN_FREQ_LOW_HZ       650.0f
#define SIREN_FREQ_HIGH_HZ      1500.0f
#define SIREN_TOTAL_MS          2200
#define SIREN_CYCLE_MS          900
#define SIREN_AMPLITUDE         30000.0f


static i2s_chan_handle_t tx_chan;
static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_anchor_rssi_task = NULL;
static TickType_t s_last_siren_tick = 0;
static bool s_rssi_wifi_ready = false;

// WAV header is 44 bytes, this struct maps it
typedef struct {
    char     riff[4];        // "RIFF"
    uint32_t file_size;
    char     wave[4];        // "WAVE"
    char     fmt[4];         // "fmt "
    uint32_t fmt_size;
    uint16_t audio_format;   // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];        // "data"
    uint32_t data_size;
} wav_header_t;


static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = I2S_BCLK,
            .ws   = I2S_LRC,
            .dout = I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
}

static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/spiffs",
        .partition_label = NULL,
        .max_files       = 5,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}
static void play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return;
    }

    // Read RIFF header
    char riff[4];
    uint32_t file_size;
    char wave[4];
    fread(riff, 1, 4, f);
    fread(&file_size, 4, 1, f);
    fread(wave, 1, 4, f);

    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a WAV file");
        fclose(f);
        return;
    }

    // Scan chunks until we find fmt and data
    uint16_t num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    bool found_fmt = false, found_data = false;

    while (!found_data) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t audio_format;
            fread(&audio_format, 2, 1, f);
            fread(&num_channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            fseek(f, 6, SEEK_CUR);  // skip byte rate + block align
            fread(&bits_per_sample, 2, 1, f);
            // skip any extra fmt bytes
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            found_fmt = true;

        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;  // file position is now at audio data

        } else {
            // skip unknown chunk
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "Invalid WAV structure");
        fclose(f);
        return;
    }

    ESP_LOGI(TAG, "Playing %s - %uHz, %uch, %ubit, %u bytes",
             path,
             (unsigned)sample_rate,
             (unsigned)num_channels,
             (unsigned)bits_per_sample,
             (unsigned)data_size);

    // Stream audio data
    const size_t chunk_size_buf = 1024;
    uint8_t *buf = malloc(chunk_size_buf);
    size_t bytes_read;
    size_t bytes_written;
    size_t total = 0;

    while (total < data_size &&
           (bytes_read = fread(buf, 1, chunk_size_buf, f)) > 0) {
        i2s_channel_write(tx_chan, buf, bytes_read, &bytes_written, portMAX_DELAY);
        total += bytes_read;
    }

    free(buf);
    fclose(f);
    ESP_LOGI(TAG, "Done - played %u bytes", (unsigned)total);
}

static void play_generated_siren(void)
{
    const float two_pi = 6.28318530718f;
    const float amplitude = SIREN_AMPLITUDE;
    const size_t samples_per_chunk = 256;
    const uint32_t chunk_ms = (uint32_t)((samples_per_chunk * 1000U) / SAMPLE_RATE);
    int16_t pcm[samples_per_chunk];
    float phase = 0.0f;
    uint32_t elapsed_ms = 0;

    ESP_LOGI(TAG_MAIN, "Playing generated siren tone");

    while (elapsed_ms < SIREN_TOTAL_MS) {
        uint32_t cycle_pos_ms = elapsed_ms % SIREN_CYCLE_MS;
        float cycle_pos = (float)cycle_pos_ms / (float)SIREN_CYCLE_MS;
        float freq_hz = 0.0f;

        if (cycle_pos < 0.5f) {
            freq_hz = SIREN_FREQ_LOW_HZ +
                      (SIREN_FREQ_HIGH_HZ - SIREN_FREQ_LOW_HZ) * (cycle_pos / 0.5f);
        } else {
            freq_hz = SIREN_FREQ_HIGH_HZ -
                      (SIREN_FREQ_HIGH_HZ - SIREN_FREQ_LOW_HZ) * ((cycle_pos - 0.5f) / 0.5f);
        }

        float phase_step = two_pi * freq_hz / (float)SAMPLE_RATE;
        for (size_t i = 0; i < samples_per_chunk; i++) {
            pcm[i] = (int16_t)(sinf(phase) * amplitude);
            phase += phase_step;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }

        size_t bytes_written = 0;
        (void)i2s_channel_write(tx_chan, pcm, sizeof(pcm), &bytes_written, portMAX_DELAY);
        elapsed_ms += chunk_ms;
    }

    memset(pcm, 0, sizeof(pcm));
    for (int i = 0; i < 3; i++) {
        size_t bytes_written = 0;
        (void)i2s_channel_write(tx_chan, pcm, sizeof(pcm), &bytes_written, portMAX_DELAY);
    }
}

static void audio_task(void *arg)
{
    const char *path = (const char *)arg;
    if (path == NULL) {
        path = DEFAULT_AUDIO_WAV_PATH;
    }

    if (strcmp(path, SIREN_WAV_PATH) == 0) {
        play_generated_siren();
    } else {
        play_wav(path);
    }

    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_play_path_request_once(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_audio_task != NULL) {
        ESP_LOGI(TAG_MAIN, "Audio playback already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(audio_task, "audio", 4096, (void *)path, 5, &s_audio_task);
    if (ok != pdPASS) {
        s_audio_task = NULL;
        ESP_LOGE(TAG_MAIN, "Failed to create audio playback task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_play_request_once(void)
{
    return audio_play_path_request_once(DEFAULT_AUDIO_WAV_PATH);
}

static float estimate_distance_from_rssi_m(int8_t rssi_dbm)
{
    return powf(10.0f,
                (RSSI_REF_DBM_AT_1M - (float)rssi_dbm) / (10.0f * RSSI_PATH_LOSS_FACTOR));
}

static esp_err_t rssi_wifi_init(void)
{
    if (s_rssi_wifi_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "NVS init failed for RSSI monitor: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_MAIN, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_MAIN, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_MAIN, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG_MAIN, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_rssi_wifi_ready = true;
    ESP_LOGI(TAG_MAIN, "RSSI monitor Wi-Fi initialized in STA mode");
    return ESP_OK;
}

static bool anchor_read_rssi_once(int8_t *out_rssi_dbm)
{
    if (out_rssi_dbm == NULL) {
        return false;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)ANCHOR_SSID,
        .channel = ANCHOR_CHANNEL,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK || found == 0) {
        return false;
    }

    wifi_ap_record_t ap_record;
    uint16_t ap_count = 1;
    err = esp_wifi_scan_get_ap_records(&ap_count, &ap_record);
    if (err != ESP_OK || ap_count == 0) {
        return false;
    }

    *out_rssi_dbm = ap_record.rssi;
    return true;
}

static void maybe_play_siren(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t cooldown_ticks = pdMS_TO_TICKS(RSSI_SIREN_COOLDOWN_MS);
    if ((now - s_last_siren_tick) < cooldown_ticks) {
        return;
    }

    s_last_siren_tick = now;
    esp_err_t err = audio_play_path_request_once(SIREN_WAV_PATH);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG_MAIN, "Failed to trigger siren playback: %s", esp_err_to_name(err));
    }
}

static void anchor_rssi_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        int8_t rssi_dbm = 0;
        bool has_anchor = anchor_read_rssi_once(&rssi_dbm);
        bool out_of_range = true;

        if (has_anchor) {
            float distance_m = estimate_distance_from_rssi_m(rssi_dbm);
            out_of_range = (distance_m > RSSI_ALERT_DISTANCE_M);

            ESP_LOGI(TAG_MAIN,
                     "Anchor '%s' RSSI=%d dBm, est distance=%.2f m (limit=%.2f m)",
                     ANCHOR_SSID,
                     (int)rssi_dbm,
                     distance_m,
                     RSSI_ALERT_DISTANCE_M);
        } else {
            out_of_range = false;  // treat as in-range if we can't read it at all
            ESP_LOGW(TAG_MAIN, "Anchor '%s' not found in scan", ANCHOR_SSID);
        }

        if (out_of_range) {
            maybe_play_siren();
        }

        vTaskDelay(pdMS_TO_TICKS(RSSI_SCAN_PERIOD_MS));
    }
}

static esp_err_t start_anchor_rssi_monitor(void)
{
    if (s_anchor_rssi_task != NULL) {
        return ESP_OK;
    }

    esp_err_t err = rssi_wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(anchor_rssi_monitor_task,
                                "anchor_rssi",
                                4096,
                                NULL,
                                5,
                                &s_anchor_rssi_task);
    if (ok != pdPASS) {
        s_anchor_rssi_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_MAIN,
             "RSSI distance monitor started for SSID '%s' (alert > %.2f m)",
             ANCHOR_SSID,
             RSSI_ALERT_DISTANCE_M);
    return ESP_OK;
}

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

    i2s_init();
    spiffs_init();

    if (start_anchor_rssi_monitor() != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "RSSI monitor init failed; siren range alert disabled");
    }

    if (camera_capture_start(&cfg) != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "Halting program. Capture module failed to initialize.");
        return;
    }

}