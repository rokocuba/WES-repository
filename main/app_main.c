//--------------------------------- INCLUDES ----------------------------------
#include "esp_log.h"

#include "gui.h"
#include "camera_capture.h"

static const char *TAG_MAIN = "MAIN";

static void on_camera_frame(const camera_capture_frame_t *frame, void *user_ctx) {
    (void)user_ctx;

    ESP_LOGI(TAG_MAIN,
             "Frame #%lu: %ux%u, size=%u bytes, components=%d",
             (unsigned long)frame->sequence,
             frame->width,
             frame->height,
             (unsigned)frame->size,
             frame->components);

    // Example: run your own processing on frame->data/frame->size here.
    // Copy the bytes if you need to use them outside this callback.
}

void app_main() {
    gui_init();

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