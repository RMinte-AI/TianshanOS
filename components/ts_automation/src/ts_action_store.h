#pragma once
#include "esp_err.h"
#include <stdbool.h>

/* Calls are serialized by the action manager's template mutex. */
esp_err_t ts_action_store_save(const char *id, const char *json, bool replace_pack);
esp_err_t ts_action_store_recover(void);
