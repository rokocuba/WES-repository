#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

typedef struct {
	const uint8_t *data;
	size_t size;
	uint16_t width;
	uint16_t height;
	int components;
	uint32_t sequence;
} camera_capture_frame_t;

typedef void (*camera_capture_frame_cb_t)(const camera_capture_frame_t *frame, void *user_ctx);

typedef struct {
	bool enable_display;
	bool enable_button_trigger;
	gpio_num_t button_gpio;
	int button_active_level;
	uint32_t button_poll_ms;
	uint32_t button_debounce_ms;
} camera_capture_config_t;

#define CAMERA_CAPTURE_DEFAULT_CONFIG() \
	(camera_capture_config_t){ \
		.enable_display = true, \
		.enable_button_trigger = true, \
		.button_gpio = GPIO_NUM_36, \
		.button_active_level = 1, \
		.button_poll_ms = 20, \
		.button_debounce_ms = 40, \
	}

void camera_capture_set_frame_callback(camera_capture_frame_cb_t cb, void *user_ctx);
esp_err_t camera_capture_request_once(void);
esp_err_t camera_capture_start(const camera_capture_config_t *config);
