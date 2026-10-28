#pragma once
#include "ts_automation_config.h"

/* Called once at boot, after empty runtime managers and PKI are ready.
 * If installed is true, the migrated set is already the published view.
 * Otherwise normal source selection opens only the new-format documents. */
esp_err_t ts_auto_config_migrate(bool sd_available, bool *installed, ts_auto_config_error_t *error);
