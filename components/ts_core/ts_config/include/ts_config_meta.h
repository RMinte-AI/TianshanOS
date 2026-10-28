#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "ts_config_module.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ts_config_meta_init(void);
esp_err_t ts_config_meta_deinit(void);
uint32_t ts_config_meta_get_global_seq(void);
/* Zero indicates that the new sequence could not be persisted. */
uint32_t ts_config_meta_increment_global_seq(void);
uint16_t ts_config_meta_get_schema_version(ts_config_module_t module);
esp_err_t ts_config_meta_set_schema_version(ts_config_module_t module, uint16_t version);
void ts_config_meta_dump(void);

#ifdef __cplusplus
}
#endif
