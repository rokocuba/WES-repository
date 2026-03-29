#include "hooks.h"
#include "esp_log.h"
#include <string.h>

#define MAX_HOOKS 32
static const char *TAG = "hooks";

typedef struct {
    const char *command;
    hook_handler_t handler;
} hook_entry_t;

static hook_entry_t s_hooks[MAX_HOOKS];
static int s_count = 0;

void hooks_init(void)
{
    s_count = 0;
}

void hooks_register(const char *command, hook_handler_t handler)
{
    if (s_count >= MAX_HOOKS) {
        ESP_LOGE(TAG, "Hook table full, cannot register '%s'", command);
        return;
    }
    s_hooks[s_count].command = command;
    s_hooks[s_count].handler = handler;
    s_count++;
    ESP_LOGI(TAG, "Registered hook: %s", command);
}

cJSON *hooks_dispatch(const char *command, const cJSON *msg)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_hooks[i].command, command) == 0) {
            return s_hooks[i].handler(msg);
        }
    }
    ESP_LOGW(TAG, "No hook registered for: %s", command);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "error", "unknown_command");
    return err;
}
