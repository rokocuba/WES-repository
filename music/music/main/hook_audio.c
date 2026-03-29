#include "hooks.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hook_audio";

/* ── I2S pin / config (match your hardware) ── */
#define I2S_BCLK       GPIO_NUM_48
#define I2S_LRC        GPIO_NUM_21
#define I2S_DOUT       GPIO_NUM_47
#define SAMPLE_RATE    22050
#define AUDIO_BUF_SIZE (SAMPLE_RATE * 6)   /* ~3 s of 16-bit mono PCM */

static i2s_chan_handle_t s_tx_chan;
static StreamBufferHandle_t s_audio_buf;
static TaskHandle_t s_writer_task;
static volatile bool s_streaming = false;

/* ── I2S hardware init ── */
static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_LOGI(TAG, "I2S ready: %d Hz, 16-bit mono", SAMPLE_RATE);
}

/* ── Background task: drains stream buffer → I2S DMA ── */
static void audio_writer_task(void *arg)
{
    uint8_t buf[1024];
    size_t written;

    for (;;) {
        size_t n = xStreamBufferReceive(s_audio_buf, buf, sizeof(buf),
                                        pdMS_TO_TICKS(100));
        if (n > 0 && s_streaming) {
            i2s_channel_write(s_tx_chan, buf, n, &written, portMAX_DELAY);
        }
    }
}

/* ── Hook handlers ── */
static cJSON *handle_audio_start(const cJSON *msg)
{
    xStreamBufferReset(s_audio_buf);
    s_streaming = true;
    i2s_channel_enable(s_tx_chan);
    ESP_LOGI(TAG, "Audio streaming started");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_audio_stop(const cJSON *msg)
{
    s_streaming = false;
    xStreamBufferReset(s_audio_buf);

    /* Flush a short silence so the DAC doesn't hold the last sample */
    uint8_t silence[512] = {0};
    size_t w;
    i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    i2s_channel_disable(s_tx_chan);

    ESP_LOGI(TAG, "Audio streaming stopped");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static cJSON *handle_audio_data(const cJSON *msg)
{
    cJSON *r = cJSON_CreateObject();

    if (!s_streaming) {
        cJSON_AddStringToObject(r, "error", "not_streaming");
        return r;
    }

    const cJSON *data_field = cJSON_GetObjectItem(msg, "data");
    if (!cJSON_IsString(data_field)) {
        cJSON_AddStringToObject(r, "error", "missing_data");
        return r;
    }

    /* Base64 → raw PCM */
    const char *b64 = data_field->valuestring;
    size_t b64_len = strlen(b64);
    size_t decoded_len = 0;
    mbedtls_base64_decode(NULL, 0, &decoded_len,
                          (const uint8_t *)b64, b64_len);

    uint8_t *pcm = malloc(decoded_len);
    if (pcm && mbedtls_base64_decode(pcm, decoded_len, &decoded_len,
                                      (const uint8_t *)b64, b64_len) == 0) {
        size_t sent = xStreamBufferSend(s_audio_buf, pcm, decoded_len,
                                        pdMS_TO_TICKS(50));
        cJSON_AddStringToObject(r, "status", "ok");
        cJSON_AddNumberToObject(r, "bytes_queued", (double)sent);
    } else {
        cJSON_AddStringToObject(r, "error", "decode_failed");
    }

    free(pcm);
    return r;
}

/* ── Registration ── */
void hook_audio_register(void)
{
    i2s_init();

    s_audio_buf = xStreamBufferCreate(AUDIO_BUF_SIZE, 1);
    xTaskCreate(audio_writer_task, "audio_wr", 4096, NULL, 5, &s_writer_task);

    hooks_register("audio_start", handle_audio_start);
    hooks_register("audio_stop",  handle_audio_stop);
    hooks_register("audio_data",  handle_audio_data);
}
