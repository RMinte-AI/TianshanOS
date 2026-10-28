#pragma once
#include "ts_automation_config.h"

/* System metadata, never an automatic copy of the automation document. */
typedef struct {
    ts_auto_config_source_t source;
    bool paused;
} ts_auto_control_t;
esp_err_t ts_auto_control_load(ts_auto_control_t *control);
esp_err_t ts_auto_control_save(const ts_auto_control_t *control);
