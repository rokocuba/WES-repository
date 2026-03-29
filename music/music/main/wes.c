#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "hooks.h"

#define WIFI_SSID      "Ian\xe2\x80\x99s iPhone"
#define WIFI_PASS      "sifra123"
#define MAX_RETRY      10
#define WS_URI         "ws://172.20.10.6:5000/ws/device"

static const char *TAG = "WES";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
static esp_websocket_client_handle_t s_ws_client = NULL;

/* ── Reassembly buffer for fragmented WebSocket messages ── */
static char *s_rx_buf = NULL;
static int   s_rx_len = 0;

/* ── WiFi ── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable WiFi power save — keeps connection alive on hotspots */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}

/* ── WebSocket message handler ── */

static void handle_ws_message(const char *data, int len)
{
    cJSON *msg = cJSON_ParseWithLength(data, len);
    if (!msg) {
        ESP_LOGE(TAG, "Failed to parse WS message");
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItem(msg, "cmd");
    const cJSON *id  = cJSON_GetObjectItem(msg, "id");
    if (!cJSON_IsString(cmd)) {
        ESP_LOGW(TAG, "WS message missing 'cmd'");
        cJSON_Delete(msg);
        return;
    }

    const char *cmd_str = cmd->valuestring;
    bool is_audio_data = (strcmp(cmd_str, "audio_data") == 0);

    if (!is_audio_data)
        ESP_LOGI(TAG, "Command: %s", cmd_str);

    /* Dispatch to registered hook */
    cJSON *result = hooks_dispatch(cmd_str, msg);

    /* Skip sending response for high-frequency audio_data to avoid congestion */
    if (!is_audio_data) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "cmd", cmd_str);
        if (cJSON_IsNumber(id))
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        if (result)
            cJSON_AddItemToObject(resp, "result", result);

        char *resp_str = cJSON_PrintUnformatted(resp);
        if (resp_str) {
            esp_websocket_client_send_text(s_ws_client, resp_str,
                                           strlen(resp_str), portMAX_DELAY);
            free(resp_str);
        }
        cJSON_Delete(resp);
    } else {
        cJSON_Delete(result);
    }
    cJSON_Delete(msg);
}

/* ── WebSocket event handler ── */

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected to server");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code != 0x01) break;  /* only text frames */

        /* Handle message reassembly for fragmented frames */
        if (ev->payload_offset == 0) {
            free(s_rx_buf);
            s_rx_buf = malloc(ev->payload_len + 1);
            s_rx_len = 0;
            if (!s_rx_buf) {
                ESP_LOGE(TAG, "OOM for WS rx buffer (%d)", ev->payload_len);
                break;
            }
        }
        if (s_rx_buf && ev->data_len > 0) {
            memcpy(s_rx_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
            s_rx_len = ev->payload_offset + ev->data_len;

            if (s_rx_len == ev->payload_len) {
                s_rx_buf[s_rx_len] = '\0';
                handle_ws_message(s_rx_buf, s_rx_len);
                free(s_rx_buf);
                s_rx_buf = NULL;
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected, will reconnect");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

static void ws_connect(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_URI,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 30000,
        .buffer_size = 32768,
        .pingpong_timeout_sec = 30,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
    ESP_LOGI(TAG, "WebSocket connecting to %s", WS_URI);
}

/* ── Main ── */

void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* WiFi */
    wifi_init_sta();

    /* Hook system — register your custom hooks here */
    hooks_init();
    hook_healthcheck_register();
    hook_camera_register();
    hook_audio_register();

    /* WebSocket */
    ws_connect();
}
