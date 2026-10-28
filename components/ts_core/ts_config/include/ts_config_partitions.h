#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_CONFIG_KEYS_PARTITION "keys_nvs"
#define TS_CONFIG_SYSTEM_PARTITION "nvs"
#define TS_CONFIG_AUTO_PARTITION "auto_cfg"
#define TS_CONFIG_AUTO_MOUNT "/autocfg"
#define TS_CONFIG_AUTO_SD_PARENT "/sdcard/config"
#define TS_CONFIG_AUTO_SD_DIR TS_CONFIG_AUTO_SD_PARENT "/automation"

/* Initialize the two NVS regions without erasing either region on failure. */
esp_err_t ts_config_partitions_init(void);

/* Before configuration schemas, authentication, networking or drivers start.
 * SD must already have been checked. Completion is recorded before cleanup. */
esp_err_t ts_config_migrate_system(bool sd_available);

#ifdef __cplusplus
}
#endif
