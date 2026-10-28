#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct { uint8_t eky[32], tky[32]; } nvs_sec_cfg_t;
typedef struct { int scheme_id; } nvs_sec_scheme_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_init_partition(const char *label);
esp_err_t nvs_flash_secure_init_partition(const char *label, nvs_sec_cfg_t *cfg);
nvs_sec_scheme_t *nvs_flash_get_default_security_scheme(void);
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *cfg);
