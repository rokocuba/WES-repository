#include "hooks.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hook_camera";

/**
 * Dummy image capture — generates a small 8x8 black BMP.
 * Replace this with actual camera capture returning a JPEG buffer.
 */
static uint8_t *capture_image(size_t *out_len)
{
    const int w = 8, h = 8;
    const int row_bytes = ((w * 3 + 3) / 4) * 4;
    const int pixel_bytes = row_bytes * h;
    const int file_size = 54 + pixel_bytes;

    uint8_t *bmp = calloc(1, file_size);
    if (!bmp) return NULL;

    /* BMP file header (14 bytes) */
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = file_size & 0xFF;
    bmp[3] = (file_size >> 8) & 0xFF;
    bmp[10] = 54;

    /* DIB header (40 bytes) */
    bmp[14] = 40;
    bmp[18] = w;
    bmp[22] = h;
    bmp[26] = 1;   /* color planes */
    bmp[28] = 24;  /* bits per pixel */
    bmp[34] = pixel_bytes & 0xFF;
    bmp[35] = (pixel_bytes >> 8) & 0xFF;

    /* Pixel data is all zeros (black) from calloc */
    *out_len = file_size;
    ESP_LOGI(TAG, "Captured dummy %dx%d BMP (%d bytes)", w, h, file_size);
    return bmp;
}

static cJSON *handle_capture_image(const cJSON *msg)
{
    size_t img_len = 0;
    uint8_t *img = capture_image(&img_len);
    if (!img) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", "capture_failed");
        return r;
    }

    /* Base64 encode */
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, img, img_len);
    uint8_t *b64 = malloc(b64_len + 1);

    cJSON *r = cJSON_CreateObject();
    if (b64 && mbedtls_base64_encode(b64, b64_len + 1, &b64_len, img, img_len) == 0) {
        b64[b64_len] = '\0';
        cJSON_AddStringToObject(r, "status", "ok");
        cJSON_AddStringToObject(r, "format", "bmp");
        cJSON_AddNumberToObject(r, "size", (double)img_len);
        cJSON_AddStringToObject(r, "data", (char *)b64);
    } else {
        cJSON_AddStringToObject(r, "error", "encode_failed");
    }

    free(b64);
    free(img);
    return r;
}

void hook_camera_register(void)
{
    hooks_register("capture_image", handle_capture_image);
}
