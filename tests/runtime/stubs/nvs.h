#pragma once
#include "platform.h"
#define NVS_READONLY 0
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
