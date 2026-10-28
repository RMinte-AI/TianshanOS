#pragma once
#include "ts_api.h"
#include "ts_automation_config.h"

typedef enum { TS_API_AUTO_UPSERT, TS_API_AUTO_CREATE, TS_API_AUTO_UPDATE } ts_api_auto_write_t;
/* One response contract for configuration writes, including partial outcomes. */
bool ts_api_auto_reply_prepare(ts_api_result_t *result);
void ts_api_auto_reply_finish(ts_api_result_t *result,esp_err_t ret,const ts_auto_config_error_t *error);
esp_err_t ts_api_auto_write(ts_auto_config_kind_t kind,const cJSON *params,ts_api_auto_write_t mode,ts_api_result_t *result);
esp_err_t ts_api_auto_delete(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result);
esp_err_t ts_api_auto_list(ts_auto_config_kind_t kind,const cJSON *params,const char *key,ts_api_result_t *result);
esp_err_t ts_api_auto_get(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result);
esp_err_t ts_api_auto_enabled(ts_auto_config_kind_t kind,const cJSON *params,bool enabled,ts_api_result_t *result);
esp_err_t ts_api_auto_mapping(const cJSON *params,bool remove,ts_api_result_t *result);
esp_err_t ts_api_auto_pack_import(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result);
