/**
 * @file ts_services.h
 * @brief TianShanOS Service Registration
 *
 * 服务注册头文件
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#ifndef TS_SERVICES_H
#define TS_SERVICES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册所有核心服务
 * 
 * 在 ts_core_init() 之后、ts_core_start() 之前调用
 *
 * @return ESP_OK 成功
 */
/* Prepare and migrate persistent settings before any owning service starts. */
esp_err_t ts_services_prepare_config(void);
esp_err_t ts_services_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_SERVICES_H */
