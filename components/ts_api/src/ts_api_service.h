#pragma once
#include "ts_api.h"
#include "ts_ssh_service.h"
/* Shared admission response for service start, verification and stop. */
esp_err_t ts_api_service_control(const cJSON *params, ts_api_result_t *result,
                                 ts_service_operation_t kind);
