/**
 * @file ts_config_schemas.h
 * @brief TianShanOS Configuration Module Schema Registration
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_CONFIG_SCHEMAS_H
#define TS_CONFIG_SCHEMAS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化配置模块系统并注册所有模块 Schema
 * 
 * 该函数完成以下工作：
 * 1. 初始化模块系统 (ts_config_module_system_init)
 * 2. 注册所有模块的 Schema (NET, DHCP, WIFI, LED, FAN, DEVICE, SYSTEM)
 * 3. 从系统 NVS 加载配置（旧 SD 仅在升级迁移时导入）
 * 
 * 应在 ts_config_init() 之后调用
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_config_schemas_init(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_CONFIG_SCHEMAS_H */
