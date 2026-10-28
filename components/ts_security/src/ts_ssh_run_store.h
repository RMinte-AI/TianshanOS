#pragma once
#include "ts_ssh_service.h"

/* One key per occupied control slot. No configuration or private credentials. */
esp_err_t ts_ssh_run_load(unsigned slot, ts_ssh_run_record_t *record);
esp_err_t ts_ssh_run_save(unsigned slot, const ts_ssh_run_record_t *record);
esp_err_t ts_ssh_run_remove(unsigned slot);
