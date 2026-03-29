#pragma once

#include "cJSON.h"

/**
 * Hook handler function type.
 * Receives the full incoming JSON message, returns a cJSON result object.
 * The caller takes ownership of the returned cJSON and will free it.
 */
typedef cJSON *(*hook_handler_t)(const cJSON *msg);

/** Initialize the hook registry. Call once before registering hooks. */
void hooks_init(void);

/**
 * Register a named hook. The command string must be a static/persistent pointer.
 * To add a new hook, implement a handler and call this from your _register function.
 */
void hooks_register(const char *command, hook_handler_t handler);

/**
 * Dispatch a command to its registered hook.
 * Returns the hook's result, or an error object if no hook matches.
 * Caller must cJSON_Delete the result.
 */
cJSON *hooks_dispatch(const char *command, const cJSON *msg);

/* Default hook registration — call these from app_main before connecting. */
void hook_healthcheck_register(void);
void hook_camera_register(void);
void hook_audio_register(void);
