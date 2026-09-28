/**
 * @file ts_ws_subscriptions.h
 * @brief WebSocket Subscription Manager for Real-time Data Streaming
 * 
 * 订阅管理器 - 将 HTTP 轮询改为 WebSocket 推送
 * 支持 topic: system.info, device.status, ota.progress
 */

#pragma once

#include "esp_err.h"
#include "ts_ws_transport.h"
#include "ts_event.h"
#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化订阅管理器
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_ws_subscriptions_init(void);

/**
 * @brief 反初始化订阅管理器
 */
esp_err_t ts_ws_subscriptions_deinit(void);
esp_err_t ts_ws_subscriptions_pause(void);
esp_err_t ts_ws_subscriptions_drain(void);

/**
 * @brief 处理订阅请求
 * 
 * @param peer 已登记的 HTTPD 会话身份（不能只传 fd）
 * @param topic 订阅主题 (e.g., "device.status", "ota.progress")
 * @param params JSON 参数对象 (可选)
 * @return ESP_OK on success
 */
esp_err_t ts_ws_subscribe(ts_ws_peer_t peer, const char *topic, cJSON *params);

/**
 * @brief 处理取消订阅请求
 * 
 * @param peer 已登记的 HTTPD 会话身份（不能只传 fd）
 * @param topic 订阅主题
 * @return ESP_OK on success
 */
esp_err_t ts_ws_unsubscribe(ts_ws_peer_t peer, const char *topic);

/**
 * @brief 客户端断开连接时清理订阅
 * 
 * @param peer 已登记的 HTTPD 会话身份（不能只传 fd）
 */
void ts_ws_client_disconnected(ts_ws_peer_t peer);

/**
 * @brief 广播数据到订阅了特定 topic 的客户端
 * 
 * @param topic 主题名称
 * @param data JSON 数据对象
 */
void ts_ws_broadcast_to_topic(const char *topic, cJSON *data);

#ifdef __cplusplus
}
#endif

/* Stable existing worker wake; does not allocate or start a new task. */
void ts_ws_subscriptions_wake(void);
bool ts_ws_subscriptions_in_context(void);
