set(COMPONENT_SRCS "app_main.c" "gui.c" "camera_capture.c" "sht31_service.c")
set(COMPONENT_ADD_INCLUDEDIRS "")
set(COMPONENT_REQUIRES
	esp_driver_gpio
	esp_driver_i2s
	esp_driver_uart
	spiffs
	lvgl
	lvgl_esp32_drivers
	ui_app
	i2cdev
	sht3x
)

register_component()
