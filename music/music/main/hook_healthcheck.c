#include "hooks.h"
#include "esp_system.h"
#include "esp_timer.h"

static cJSON *handle_healthcheck(const cJSON *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddNumberToObject(r, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(r, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "min_heap", esp_get_minimum_free_heap_size());
    return r;
}

void hook_healthcheck_register(void)
{
    hooks_register("healthcheck", handle_healthcheck);
}
