/**
 * @file ts_source_manager.h
 * @brief TianShanOS Automation Engine - Data Source Manager API
 *
 * Manages data sources:
 * - WebSocket connections to external servers
 * - REST API polling (supports local API and external HTTP)
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_SOURCE_MANAGER_H
#define TS_SOURCE_MANAGER_H

#include "ts_automation_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                           Initialization                                   */
/*===========================================================================*/

/**
 * @brief Initialize source manager
 *
 * @return ESP_OK on success
 */
esp_err_t ts_source_manager_init(void);

/**
 * @brief Deinitialize source manager
 *
 * @return ESP_OK on success
 */
esp_err_t ts_source_manager_deinit(void);

/*===========================================================================*/
/*                           Source Management                                */
/*===========================================================================*/

/**
 * @brief Register a data source
 *
 * @param source Source definition
 * @return ESP_OK on success
 */
esp_err_t ts_source_register(const ts_auto_source_t *source);

/**
 * @brief Unregister a data source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_unregister(const char *id);

/**
 * @brief Enable a data source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_enable(const char *id);

/**
 * @brief Disable a data source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_disable(const char *id);

/**
 * @brief Copy a source definition and its current status
 */
esp_err_t ts_source_get_copy(const char *id, ts_auto_source_t *out);
int ts_source_count(void);

/**
 * @brief Get source by index with thread-safe copy
 *
 * @param index Source index (0 to source_count-1)
 * @param out_source Output buffer for source copy
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if index out of range
 */
esp_err_t ts_source_get_by_index_copy(int index, ts_auto_source_t *out_source);

/*===========================================================================*/
/*                           Data Collection                                  */
/*===========================================================================*/

/**
 * @brief Start data collection for all enabled sources
 *
 * @return ESP_OK on success
 */
esp_err_t ts_source_start_all(void);

/**
 * @brief Stop data collection for all sources
 *
 * @return ESP_OK on success
 */
esp_err_t ts_source_stop_all(void);

/**
 * @brief Poll a specific source for new data
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_poll(const char *id);

/**
 * @brief Poll all sources that are due for update
 *
 * Called periodically by automation engine.
 *
 * @return Number of sources polled
 */
int ts_source_poll_all(void);

/*===========================================================================*/
/*                           WebSocket Sources                                */
/*===========================================================================*/

/**
 * @brief WebSocket message callback
 *
 * @param source_id Source ID
 * @param data Received data
 * @param data_len Data length
 * @param user_data User data
 */
typedef void (*ts_source_ws_callback_t)(const char *source_id, 
                                         const void *data, size_t data_len,
                                         void *user_data);

/**
 * @brief Connect WebSocket source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_ws_connect(const char *id);

/**
 * @brief Disconnect WebSocket source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_ws_disconnect(const char *id);

/**
 * @brief Check WebSocket connection status
 *
 * @param id Source ID
 * @return true if connected
 */
bool ts_source_ws_is_connected(const char *id);

/*===========================================================================*/
/*                           Socket.IO Sources                                */
/*===========================================================================*/

/* Both the test and the runtime upgrade the same polling URL. */
void ts_source_sio_websocket_url(const char *base_url, const char *sid, char *out, size_t size);

/**
 * @brief Connect Socket.IO source
 *
 * Initiates HTTP handshake and WebSocket upgrade.
 * Data will be received via event subscription.
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_sio_connect(const char *id);

/**
 * @brief Disconnect Socket.IO source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_sio_disconnect(const char *id);

/**
 * @brief Check Socket.IO connection status
 *
 * @param id Source ID
 * @return true if connected and upgraded
 */
bool ts_source_sio_is_connected(const char *id);

/*===========================================================================*/
/*                           REST Sources                                     */
/*===========================================================================*/

/**
 * @brief Fetch data from REST source
 *
 * @param id Source ID
 * @return ESP_OK on success
 */
esp_err_t ts_source_rest_fetch(const char *id);

/*===========================================================================*/
/*                           Source Enumeration                               */
/*===========================================================================*/

/**
 * @brief Source enumeration callback
 *
 * @param source Source definition
 * @param user_data User data
 * @return true to continue, false to stop
 */
typedef bool (*ts_source_enum_cb_t)(const ts_auto_source_t *source, void *user_data);

/**
 * @brief Enumerate all sources
 *
 * @param type_filter Filter by type, or -1 for all
 * @param callback Callback function
 * @param user_data User data
 * @return Number of sources enumerated
 */
int ts_source_enumerate(int type_filter, ts_source_enum_cb_t callback, void *user_data);

/*===========================================================================*/
/*                           Statistics                                       */
/*===========================================================================*/

/**
 * @brief Source manager statistics
 */
typedef struct {
    uint32_t total_polls;                /**< Total poll operations */
    uint32_t successful_polls;           /**< Successful polls */
    uint32_t failed_polls;               /**< Failed polls */
    uint32_t ws_messages;                /**< WebSocket messages received */
    uint32_t rest_requests;              /**< REST requests made */
} ts_source_manager_stats_t;

/**
 * @brief Get source manager statistics
 *
 * @param stats Output statistics
 * @return ESP_OK on success
 */
esp_err_t ts_source_manager_get_stats(ts_source_manager_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* TS_SOURCE_MANAGER_H */
