#pragma once
#include "ts_automation_document.h"

/* One-time upgrade input. Reads the old physical NVS and SD files without
 * repairing, exporting, deleting, or starting any of the old managers. */
esp_err_t ts_auto_legacy_read(bool sd_available, cJSON **out, ts_auto_config_error_t *error);
