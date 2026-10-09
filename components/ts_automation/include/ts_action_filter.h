#pragma once
#include "ts_automation_types.h"
#include "cJSON.h"

esp_err_t ts_action_filter_decode(const cJSON *json, ts_auto_filter_params_t *out);
/* Add filter_params to a template/rule LED object; omit when absent in old data. */
bool ts_action_filter_encode(const ts_auto_filter_params_t *params, cJSON *led);
/* Add explicit parameters, in led.filter.start API units, to the API request. */
bool ts_action_filter_api_params(const ts_auto_filter_params_t *params, cJSON *request);
