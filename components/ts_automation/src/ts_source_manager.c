#include "ts_action_codec.h"
/**
 * @file ts_source_manager.c
 * @brief TianShanOS 自动化引擎 - 数据源管理器实现
 *
 * 管理多类数据源：
 * 1. WebSocket - 外部服务器实时数据
 * 2. Socket.IO - Socket.IO v4 协议（如 AGX Monitor）
 * 3. REST - HTTP API 轮询（支持本地 API 和外部 API）
 *
 * 数据提取使用 JSONPath 表达式，支持动态映射到变量
 *
 * @author TianShanOS Team
 * @version 1.1.0
 */

#include "ts_source_manager.h"
#include "ts_source_runtime.h"
#include "ts_automation_runtime.h"
#include "ts_variable.h"
#include "ts_jsonpath.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

// HTTP 客户端（用于 REST 源）
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

// API 层（用于本地 API 调用）
#include "ts_api.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "ts_source_mgr";

/*===========================================================================*/
/*                          Socket.IO 协议常量                                */
/*===========================================================================*/

#define SIO_PROBE_MSG           "2probe"
#define SIO_UPGRADE_MSG         "5"
#define SIO_PING_MSG            "2"
#define SIO_PONG_MSG            "3"
#define SIO_EVENT_PREFIX        "42"
#define SIO_SID_MAX_LEN         64
#define SIO_MAX_CONNECTIONS     CONFIG_TS_AUTOMATION_MAX_SOURCES

/*===========================================================================*/
/*                              配置常量                                      */
/*===========================================================================*/

#ifndef CONFIG_TS_AUTOMATION_MAX_SOURCES
#define CONFIG_TS_AUTOMATION_MAX_SOURCES  16
#endif

/*===========================================================================*/
/*                              内部状态                                      */
/*===========================================================================*/

/*===========================================================================*/
/*                          Socket.IO 连接上下文                              */
/*===========================================================================*/

/** Socket.IO 消息缓冲区大小 */
#define SIO_MSG_BUF_SIZE    8192

/** Socket.IO 连接状态 */
typedef struct {
    char source_id[TS_AUTO_NAME_MAX_LEN];   // 关联的数据源 ID
    esp_websocket_client_handle_t client;    // WebSocket 客户端
    char session_id[SIO_SID_MAX_LEN];        // Socket.IO session ID
    atomic_bool connected;                   // WebSocket 已连接
    atomic_bool upgraded;                    // Socket.IO 升级完成
    atomic_uint last_message_ms;            // 最近消息毫秒值，按无符号差计算超时
    TaskHandle_t task_handle;                // 连接任务句柄
    atomic_bool should_stop;                 // 停止标志
    atomic_bool allocated;
    ts_auto_source_type_t type;
    uint64_t source_instance;
    // 消息分片缓冲
    char *msg_buf;                           // 消息缓冲区（动态分配）
    size_t msg_buf_len;                      // 当前缓冲长度
    // 待处理数据（延迟到主任务中处理，避免回调栈溢出）
    char *pending_json;                      // 待处理的 JSON 字符串
    SemaphoreHandle_t pending_mutex;         // 保护 pending_json
    bool auto_discovered;                    // 已完成自动发现（防止重复创建）
} sio_connection_t;

/** Socket.IO 连接管理 */
static struct {
    sio_connection_t connections[SIO_MAX_CONNECTIONS];
    SemaphoreHandle_t mutex;
    atomic_bool activation_pending;
} s_sio_ctx = {0};

/* Caller holds the source mutex. An old transport cannot address a recreated ID. */
static int find_source_index(const char *id);
static int current_source_index(const char *id, uint64_t instance)
{
    int index = find_source_index(id);
    return !ts_source_runtime.closing && index >= 0 && ts_source_runtime.sources[index].enabled &&
        ts_source_runtime.sources[index].instance == instance ? index : -1;
}

static int current_connection_index(const sio_connection_t *conn)
{
    return ts_source_runtime.running && !conn->should_stop ?
        current_source_index(conn->source_id, conn->source_instance) : -1;
}

/*===========================================================================*/
/*                              辅助函数                                      */
/*===========================================================================*/

/**
 * 查找源索引
 */
static int find_source_index(const char *id)
{
    for (int i = 0; i < ts_source_runtime.count; i++) {
        if (strcmp(ts_source_runtime.sources[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/*===========================================================================*/
/*                              辅助函数                                      */
/*===========================================================================*/

/**
 * 从 JSON 中按路径提取值
 * @param json JSON 对象
 * @param path 路径字符串，支持多种格式：
 *   - 点分隔: "data.cpu.usage"
 *   - 数组索引: "items[0].name" 或 "items.0.name"
 *   - 混合: "data.servers[2].metrics.cpu"
 * @param value 输出值
 * @return ESP_OK 成功
 */
static esp_err_t extract_json_value(cJSON *json, const char *path, ts_auto_value_t *value)
{
    if (!json || !path || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    // 特殊情况：空路径或 "$" 表示根对象
    if (path[0] == '\0' || (path[0] == '$' && path[1] == '\0')) {
        // 返回整个 JSON 作为字符串
        char *str = cJSON_PrintUnformatted(json);
        if (str) {
            value->type = TS_AUTO_VAL_STRING;
            strncpy(value->str_val, str, sizeof(value->str_val) - 1);
            value->str_val[sizeof(value->str_val) - 1] = '\0';
            free(str);
            return ESP_OK;
        }
        return ESP_ERR_NO_MEM;
    }

    // 复制路径用于处理
    char path_copy[TS_AUTO_NAME_MAX_LEN];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    // 跳过开头的 "$." (JSONPath 根符号)
    char *p = path_copy;
    if (p[0] == '$' && p[1] == '.') {
        p += 2;
    }

    cJSON *current = json;
    char token[64];
    size_t token_idx = 0;

    while (*p && current) {
        // 跳过分隔符
        if (*p == '.' || *p == '/') {
            p++;
            continue;
        }

        // 处理数组索引 [n]
        if (*p == '[') {
            p++;  // 跳过 '['
            int index = 0;
            while (*p >= '0' && *p <= '9') {
                index = index * 10 + (*p - '0');
                p++;
            }
            if (*p == ']') p++;  // 跳过 ']'

            if (cJSON_IsArray(current)) {
                current = cJSON_GetArrayItem(current, index);
            } else {
                ESP_LOGD(TAG, "Path '%s': expected array at index %d", path, index);
                return ESP_ERR_NOT_FOUND;
            }
            continue;
        }

        // 提取字段名
        token_idx = 0;
        while (*p && *p != '.' && *p != '[' && *p != '/' && token_idx < sizeof(token) - 1) {
            token[token_idx++] = *p++;
        }
        token[token_idx] = '\0';

        if (token_idx == 0) continue;

        // 检查是否是纯数字（数组索引简写：items.0.name）
        bool is_numeric = true;
        for (size_t i = 0; i < token_idx; i++) {
            if (token[i] < '0' || token[i] > '9') {
                is_numeric = false;
                break;
            }
        }

        if (is_numeric && cJSON_IsArray(current)) {
            int index = atoi(token);
            current = cJSON_GetArrayItem(current, index);
        } else {
            current = cJSON_GetObjectItem(current, token);
        }
    }

    if (!current) {
        return ESP_ERR_NOT_FOUND;
    }

    // 根据 JSON 类型设置值
    if (cJSON_IsNumber(current)) {
        double num = current->valuedouble;
        if (num == (int32_t)num) {
            value->type = TS_AUTO_VAL_INT;
            value->int_val = (int32_t)num;
        } else {
            value->type = TS_AUTO_VAL_FLOAT;
            value->float_val = (float)num;
        }
    } else if (cJSON_IsBool(current)) {
        value->type = TS_AUTO_VAL_BOOL;
        value->bool_val = cJSON_IsTrue(current);
    } else if (cJSON_IsString(current)) {
        value->type = TS_AUTO_VAL_STRING;
        strncpy(value->str_val, current->valuestring, sizeof(value->str_val) - 1);
        value->str_val[sizeof(value->str_val) - 1] = '\0';
    } else {
        // 对象或数组，转为字符串
        char *str = cJSON_PrintUnformatted(current);
        if (str) {
            value->type = TS_AUTO_VAL_STRING;
            strncpy(value->str_val, str, sizeof(value->str_val) - 1);
            value->str_val[sizeof(value->str_val) - 1] = '\0';
            free(str);
        } else {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

/**
 * 将 cJSON 值转换为 ts_auto_value_t
 */
static void cjson_to_value(cJSON *json, ts_auto_value_t *value)
{
    if (!json || !value) return;

    if (cJSON_IsNumber(json)) {
        double num = json->valuedouble;
        if (num == (int32_t)num && num >= INT32_MIN && num <= INT32_MAX) {
            value->type = TS_AUTO_VAL_INT;
            value->int_val = (int32_t)num;
        } else {
            value->type = TS_AUTO_VAL_FLOAT;
            value->float_val = (float)num;
        }
    } else if (cJSON_IsBool(json)) {
        value->type = TS_AUTO_VAL_BOOL;
        value->bool_val = cJSON_IsTrue(json);
    } else if (cJSON_IsString(json)) {
        value->type = TS_AUTO_VAL_STRING;
        strncpy(value->str_val, json->valuestring, sizeof(value->str_val) - 1);
        value->str_val[sizeof(value->str_val) - 1] = '\0';
    } else if (cJSON_IsNull(json)) {
        value->type = TS_AUTO_VAL_NULL;
    } else {
        // Object/Array -> JSON string
        char *str = cJSON_PrintUnformatted(json);
        if (str) {
            value->type = TS_AUTO_VAL_STRING;
            strncpy(value->str_val, str, sizeof(value->str_val) - 1);
            value->str_val[sizeof(value->str_val) - 1] = '\0';
            free(str);
        }
    }
}

/**
 * 处理数据源的所有映射
 *
 * 使用 JSONPath 从 JSON 数据中提取值，并更新对应的变量
 *
 * @param src 数据源
 * @param json_data JSON 数据
 * @return 成功处理的映射数量
 */
static int process_source_mappings(ts_auto_source_t *src, cJSON *json_data);

/**
 * 自动发现 JSON 中的叶子节点并创建/更新变量
 * 用于 Socket.IO 数据源没有配置 mappings 时的自动处理
 *
 * @param src 数据源
 * @param json_data JSON 数据
 * @param prefix 变量名前缀（通常是 source id）
 * @param max_depth 最大递归深度
 * @param create_new 是否创建新变量（false 时只更新已存在的变量）
 * @return 成功创建/更新的变量数量
 */
static int auto_discover_json_fields(ts_auto_source_t *src, cJSON *json_data,
                                     const char *prefix, int max_depth, bool create_new)
{
    if (!src || !json_data || max_depth <= 0) {
        return 0;
    }

    int count = 0;
    char var_name[96];  /* 增大缓冲区以容纳完整变量名 */

    if (cJSON_IsObject(json_data)) {
        cJSON *item;
        cJSON_ArrayForEach(item, json_data) {
            // 构建变量名
            if (prefix && prefix[0]) {
                snprintf(var_name, sizeof(var_name), "%s.%s", prefix, item->string);
            } else {
                snprintf(var_name, sizeof(var_name), "%s.%s", src->id, item->string);
            }

            // 只处理叶子节点（数值、布尔、字符串）
            if (cJSON_IsBool(item) || cJSON_IsNumber(item) || cJSON_IsString(item)) {
                ts_auto_value_t value = {0};
                cjson_to_value(item, &value);

                // 尝试更新变量
                esp_err_t ret = ts_variable_set(var_name, &value);
                if (ret == ESP_ERR_NOT_FOUND && create_new) {
                    // 变量不存在且允许创建
                    ts_auto_variable_t new_var = {0};
                    strncpy(new_var.name, var_name, sizeof(new_var.name) - 1);
                    strncpy(new_var.source_id, src->id, sizeof(new_var.source_id) - 1);
                    new_var.value = value;
                    new_var.flags = 0;

                    ret = ts_variable_upsert(&new_var);
                    if (ret == ESP_OK) {
                        ESP_LOGD(TAG, "Auto-discovered variable: %s", var_name);
                        count++;
                    }
                    // 存储满时不再尝试创建，继续更新已有变量
                } else if (ret == ESP_OK) {
                    count++;
                }
            } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
                // 递归处理子对象
                count += auto_discover_json_fields(src, item, var_name, max_depth - 1, create_new);
            }
        }
    } else if (cJSON_IsArray(json_data)) {
        // 对数组，只处理前几个元素
        int arr_size = cJSON_GetArraySize(json_data);
        int max_elements = (arr_size > 4) ? 4 : arr_size;  // 最多处理4个元素

        for (int i = 0; i < max_elements; i++) {
            cJSON *item = cJSON_GetArrayItem(json_data, i);
            char arr_prefix[64];
            snprintf(arr_prefix, sizeof(arr_prefix), "%s[%d]", prefix, i);

            if (cJSON_IsBool(item) || cJSON_IsNumber(item) || cJSON_IsString(item)) {
                ts_auto_value_t value = {0};
                cjson_to_value(item, &value);

                esp_err_t ret = ts_variable_set(arr_prefix, &value);
                if (ret == ESP_ERR_NOT_FOUND && create_new) {
                    ts_auto_variable_t new_var = {0};
                    strncpy(new_var.name, arr_prefix, sizeof(new_var.name) - 1);
                    strncpy(new_var.source_id, src->id, sizeof(new_var.source_id) - 1);
                    new_var.value = value;
                    new_var.flags = 0;

                    ret = ts_variable_upsert(&new_var);
                    if (ret == ESP_OK) {
                        count++;
                    }
                } else if (ret == ESP_OK) {
                    count++;
                }
            } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
                count += auto_discover_json_fields(src, item, arr_prefix, max_depth - 1, create_new);
            }
        }
    }

    return count;
}

/**
 * 处理数据源映射，从 JSON 中提取值并更新变量
 *
 * @param src 数据源
 * @param json_data JSON 数据
 * @return 成功处理的映射数量
 */
static int process_source_mappings(ts_auto_source_t *src, cJSON *json_data)
{
    if (!src || !json_data) {
        return 0;
    }

    int processed = 0;

    // 遍历所有映射
    for (uint8_t i = 0; i < src->mapping_count && i < TS_AUTO_MAX_MAPPINGS; i++) {
        ts_auto_mapping_t *mapping = &src->mappings[i];

        // 跳过空映射
        if (mapping->json_path[0] == '\0' || mapping->var_name[0] == '\0') {
            continue;
        }

        // 使用 JSONPath 提取值
        cJSON *result = ts_jsonpath_get(json_data, mapping->json_path);
        if (!result) {
            ESP_LOGD(TAG, "Mapping %d: path '%s' not found", i, mapping->json_path);
            continue;
        }

        // 如果结果是数组或对象，递归展开为多个变量
        if (cJSON_IsArray(result) || cJSON_IsObject(result)) {
            // 使用 auto_discover 函数来展开，变量名前缀为 mapping->var_name
            int expanded = auto_discover_json_fields(src, result, mapping->var_name, 3, true);
            ESP_LOGD(TAG, "Mapping %d: expanded '%s' into %d variables",
                     i, mapping->json_path, expanded);
            processed += expanded;
            cJSON_Delete(result);
            continue;
        }

        // 转换为 ts_auto_value_t（仅处理叶子节点）
        ts_auto_value_t value = {0};
        cjson_to_value(result, &value);
        cJSON_Delete(result);  // ts_jsonpath_get 返回副本，需要释放

        // 尝试更新变量
        esp_err_t ret = ts_variable_set(mapping->var_name, &value);

        // 如果变量不存在，自动创建
        if (ret == ESP_ERR_NOT_FOUND) {
            ts_auto_variable_t new_var = {0};
            strncpy(new_var.name, mapping->var_name, sizeof(new_var.name) - 1);
            strncpy(new_var.source_id, src->id, sizeof(new_var.source_id) - 1);
            new_var.value = value;
            new_var.flags = 0;  // 可读写

            ret = ts_variable_upsert(&new_var);
            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Auto-created variable '%s' from mapping", mapping->var_name);
                processed++;
            } else {
                ESP_LOGW(TAG, "Failed to create variable '%s': %s",
                         mapping->var_name, esp_err_to_name(ret));
            }
        } else if (ret == ESP_OK) {
            processed++;
            ESP_LOGD(TAG, "Mapping %d: %s -> %s (type=%d)",
                     i, mapping->json_path, mapping->var_name, value.type);
        } else {
            ESP_LOGW(TAG, "Failed to set variable '%s': %s",
                     mapping->var_name, esp_err_to_name(ret));
        }
    }

    // 更新时间戳
    src->last_update_ms = esp_timer_get_time() / 1000;

    return processed;
}

/**
 * 批量处理多个 JSONPath 并返回结果数组
 *
 * @param json_data JSON 数据
 * @param paths JSONPath 数组
 * @param count 路径数量
 * @param results 输出值数组
 * @return 成功提取的数量
 * @note Reserved for batch JSONPath extraction feature
 */
__attribute__((unused))
static int batch_extract_json(cJSON *json_data, const char **paths, int count, ts_auto_value_t *results)
{
    if (!json_data || !paths || !results || count <= 0) {
        return 0;
    }

    int success_count = 0;

    for (int i = 0; i < count; i++) {
        results[i].type = TS_AUTO_VAL_NULL;

        if (!paths[i] || paths[i][0] == '\0') {
            continue;
        }

        cJSON *result = ts_jsonpath_get(json_data, paths[i]);
        if (result) {
            cjson_to_value(result, &results[i]);
            cJSON_Delete(result);
            success_count++;
        }
    }

    return success_count;
}

/**
 * 读取 REST 数据源
 * 支持：
 * 1. 本地 API (127.0.0.1 或 localhost) - 直接调用 ts_api_call
 * 2. 外部 API - 通过 HTTP 客户端请求
 */
static esp_err_t read_rest_source(ts_auto_source_t *src, ts_auto_value_t *value)
{
    const char *url = src->rest.url;
    const char *path = src->rest.path;

    ESP_LOGD(TAG, "Reading REST source: %s, path: %s", url, path);

    // 检测是否是本地 API 调用
    bool is_local = (strstr(url, "://127.0.0.1") != NULL ||
                     strstr(url, "://localhost") != NULL);

    cJSON *response_json = NULL;
    esp_err_t ret = ESP_OK;

    if (is_local) {
        // 本地 API 调用 - 解析路径并直接调用
        const char *api_path = strstr(url, "/api/v1/");
        if (!api_path) {
            ESP_LOGW(TAG, "Invalid local API URL: %s", url);
            return ESP_ERR_INVALID_ARG;
        }
        api_path += 8;  // 跳过 "/api/v1/"

        // 将路径转换为 API 名称: system/memory -> system.memory
        char api_name[64] = {0};
        size_t i = 0, j = 0;
        while (api_path[i] && j < sizeof(api_name) - 1) {
            if (api_path[i] == '/') {
                api_name[j++] = '.';
            } else if (api_path[i] == '?') {
                break;
            } else {
                api_name[j++] = api_path[i];
            }
            i++;
        }
        api_name[j] = '\0';

        ESP_LOGD(TAG, "Local API call: %s", api_name);

        // 调用本地 API
        ts_api_result_t api_result = {0};
        cJSON *api_params = cJSON_CreateObject();

        ret = ts_api_call(api_name, api_params, &api_result);
        cJSON_Delete(api_params);

        if (ret == ESP_OK && api_result.code == TS_API_OK && api_result.data) {
            response_json = cJSON_Duplicate(api_result.data, true);
        } else {
            ESP_LOGW(TAG, "Local API call failed: %s, code=%d",
                     api_result.message ? api_result.message : "unknown", api_result.code);
            ret = ESP_FAIL;
        }

        // 清理
        if (api_result.message) free(api_result.message);
        if (api_result.data) cJSON_Delete(api_result.data);

    } else {
        // 外部 HTTP 请求
        char *response_buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!response_buf) {
            response_buf = malloc(4096);
        }
        if (!response_buf) {
            return ESP_ERR_NO_MEM;
        }

        int response_len = 0;

        esp_http_client_config_t config = {
            .url = url,
            .method = !strcmp(src->rest.method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET,
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .skip_cert_common_name_check = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }

        // 添加认证头
        if (src->rest.auth_header[0] != '\0') {
            esp_http_client_set_header(client, "Authorization", src->rest.auth_header);
        }

        ret = esp_http_client_open(client, 0);
        if (ret == ESP_OK) {
            esp_http_client_fetch_headers(client);  // 获取头部（返回值未使用，chunked 模式下可能为负数）
            int status_code = esp_http_client_get_status_code(client);

            if (status_code == 200) {
                response_len = esp_http_client_read(client, response_buf, 4095);
                if (response_len > 0) {
                    response_buf[response_len] = '\0';
                    response_json = cJSON_Parse(response_buf);
                    if (!response_json) {
                        ESP_LOGW(TAG, "Failed to parse JSON response");
                        ret = ESP_ERR_INVALID_RESPONSE;
                    }
                } else {
                    ret = ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                ESP_LOGW(TAG, "HTTP request failed with status: %d, URL: %s", status_code, url);
                ret = ESP_FAIL;
            }
        } else {
            ESP_LOGW(TAG, "HTTP connection failed: %s", esp_err_to_name(ret));
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(response_buf);
    }

    // 处理 JSON 响应
    if (ret == ESP_OK && response_json) {
        ts_variable_registration_begin();
        xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
        int index = current_source_index(src->id, src->instance);
        if (index < 0) {
            xSemaphoreGive(ts_source_runtime.mutex);
            ts_variable_registration_end();
            cJSON_Delete(response_json);
            return ESP_ERR_INVALID_STATE;
        }
        src->auto_discovered = ts_source_runtime.sources[index].auto_discovered;
        // 方式 1：使用映射（推荐）- 与 Socket.IO 一致
        if (src->mapping_count > 0) {
            int mapped = process_source_mappings(src, response_json);
            ESP_LOGD(TAG, "REST source '%s': processed %d/%d mappings",
                     src->id, mapped, src->mapping_count);
        }

        // 方式 2：自动发现 - 与 Socket.IO 一致
        if (src->auto_discover) {
            if (!src->auto_discovered) {
                // 首次：自动发现并创建变量
                int discovered = auto_discover_json_fields(src, response_json, NULL, 3, true);
                src->auto_discovered = true;
                ESP_LOGD(TAG, "REST source '%s': auto-discovered %d variables (first time)",
                         src->id, discovered);
            } else {
                // 后续：只更新变量值（不尝试创建新变量）
                int updated = auto_discover_json_fields(src, response_json, NULL, 3, false);
                ESP_LOGD(TAG, "REST source '%s': updated %d variables", src->id, updated);
            }
        }

        // 方式 3：使用单一路径（向后兼容，仅当无 mapping 和无 auto_discover 时）
        if (path[0] != '\0' && value && src->mapping_count == 0 && !src->auto_discover) {
            const char *actual_path = path;

            // 对于本地 API，response_json 已经是 data 对象
            if (is_local && strncmp(path, "data.", 5) == 0) {
                actual_path = path + 5;
                ESP_LOGD(TAG, "Local API: stripped 'data.' prefix, using path: %s", actual_path);
            }

            // 使用 JSONPath 提取
            cJSON *result = ts_jsonpath_get(response_json, actual_path);
            if (result) {
                cjson_to_value(result, value);
                cJSON_Delete(result);
            } else {
                // Fallback 到旧方法
                ret = extract_json_value(response_json, actual_path, value);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to extract path '%s' from response", actual_path);
                }
            }
        } else if (value && src->mapping_count == 0 && !src->auto_discover) {
            // 无路径也无映射也无自动发现，返回整个 JSON 字符串
            char *str = cJSON_PrintUnformatted(response_json);
            if (str) {
                value->type = TS_AUTO_VAL_STRING;
                strncpy(value->str_val, str, sizeof(value->str_val) - 1);
                value->str_val[sizeof(value->str_val) - 1] = '\0';
                free(str);
            }
        }
        ts_source_runtime.sources[index].auto_discovered = src->auto_discovered;
        xSemaphoreGive(ts_source_runtime.mutex);
        ts_variable_registration_end();
        cJSON_Delete(response_json);
    }

    return ret;
}

/**
 * 读取变量数据源
 *
 * 统一使用 ts_automation 变量系统（ts_variable API）
 *
 * 两种模式：
 * 1. 监视指令变量：基于前缀读取 SSH 命令执行结果
 * 2. 读取单个变量：直接获取指定变量的值
 */
static esp_err_t read_variable_source(ts_auto_source_t *src, ts_auto_value_t *value)
{
    const char *var_prefix = src->variable.var_prefix;
    const char *var_name = src->variable.var_name;
    bool watch_all = src->variable.watch_all;

    // 模式 1：监视指令变量（基于前缀）
    // 数据源只读取已有变量，不执行 SSH 命令
    // SSH 指令由其他机制触发执行（手动、定时任务等）
    if (var_prefix[0] != '\0') {
        int count = 0;

        // 从 var_prefix 提取命令别名（去掉末尾的 "."）
        char alias[TS_AUTO_NAME_MAX_LEN];
        strncpy(alias, var_prefix, sizeof(alias) - 1);
        alias[sizeof(alias) - 1] = '\0';
        size_t len = strlen(alias);
        if (len > 0 && alias[len - 1] == '.') {
            alias[len - 1] = '\0';
        }

        // 读取 SSH 命令执行后产生的标准变量（使用 ts_automation 系统）
        const char *suffixes[] = {"status", "exit_code", "extracted", "expect_matched",
                                   "fail_matched", "host", "timestamp", "output"};
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            char full_name[96];
            snprintf(full_name, sizeof(full_name), "%s.%s", alias, suffixes[i]);

            // 直接从 ts_automation 变量系统读取
            ts_auto_value_t var_value = {0};
            if (ts_variable_get(full_name, &var_value) == ESP_OK) {
                // 如果数据源 ID 与变量前缀不同，则复制到数据源命名空间
                if (strcmp(src->id, alias) != 0) {
                    char auto_var_name[128];
                    snprintf(auto_var_name, sizeof(auto_var_name), "%s.%s", src->id, suffixes[i]);
                    ts_variable_set(auto_var_name, &var_value);
                }
                count++;
            }
        }

        ESP_LOGD(TAG, "Variable source '%s': read %d variables from prefix '%s'",
                 src->id, count, alias);

        if (count > 0) {
            // 返回主要状态变量
            char status_var[96];
            snprintf(status_var, sizeof(status_var), "%s.status", alias);
            ts_auto_value_t status_value = {0};
            if (ts_variable_get(status_var, &status_value) == ESP_OK) {
                *value = status_value;
            } else {
                value->type = TS_AUTO_VAL_INT;
                value->int_val = count;
            }
            return ESP_OK;
        }

        // 变量不存在（指令可能尚未执行）
        ESP_LOGD(TAG, "Variable source '%s': no variables found for prefix '%s' (command may not have been executed yet)",
                 src->id, alias);
        return ESP_ERR_NOT_FOUND;
    }

    // 模式 2：读取单个变量（直接使用 ts_automation 系统）
    if (var_name[0] != '\0') {
        ts_auto_value_t var_value = {0};
        esp_err_t ret = ts_variable_get(var_name, &var_value);

        if (ret == ESP_OK) {
            *value = var_value;

            // 同时更新自动化变量（如果有映射）
            if (src->mapping_count > 0) {
                ts_variable_set(src->mappings[0].var_name, &var_value);
            }

            return ESP_OK;
        }
        return ret;
    }

    // 模式 3：监视前缀下的所有变量（使用 ts_automation 枚举）
    if (watch_all && var_prefix[0] != '\0') {
        int count = 0;

        // 使用 ts_variable_enumerate 遍历所有匹配前缀的变量
        ts_variable_iterate_ctx_t ctx = {0};
        ts_auto_variable_t var;

        while (ts_variable_iterate(&ctx, &var) == ESP_OK) {
            // 检查变量名是否以 var_prefix 开头
            if (strncmp(var.name, var_prefix, strlen(var_prefix)) == 0) {
                // 如果数据源 ID 与前缀不同，复制到数据源命名空间
                if (strncmp(src->id, var_prefix, strlen(src->id)) != 0) {
                    char auto_var_name[128];
                    const char *suffix = var.name + strlen(var_prefix);
                    snprintf(auto_var_name, sizeof(auto_var_name), "%s%s", src->id, suffix);
                    ts_variable_set(auto_var_name, &var.value);
                }
                count++;
            }
        }

        ESP_LOGD(TAG, "Variable source '%s': synced %d variables from prefix '%s'",
                 src->id, count, var_prefix);

        if (count > 0) {
            value->type = TS_AUTO_VAL_INT;
            value->int_val = count;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

/*===========================================================================*/
/*                              初始化                                        */
/*===========================================================================*/

esp_err_t ts_source_manager_init(void)
{
    esp_err_t ret = ts_source_runtime_init();
    if (ret != ESP_OK) return ret;
    if (!s_sio_ctx.mutex) s_sio_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_sio_ctx.mutex) { ts_source_runtime_deinit();return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t ts_source_manager_deinit(void)
{
    if (!ts_source_runtime.initialized) return ESP_OK;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    ts_source_runtime.closing = true;
    xSemaphoreGive(ts_source_runtime.mutex);
    ts_source_stop_all();
    bool pending;
    do {
        pending = false;
        for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i) pending |= atomic_load(&s_sio_ctx.connections[i].allocated);
        xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
        pending |= ts_source_runtime.active_polls != 0;
        xSemaphoreGive(ts_source_runtime.mutex);
        if (pending) vTaskDelay(pdMS_TO_TICKS(10));
    } while (pending);
    vSemaphoreDelete(s_sio_ctx.mutex);s_sio_ctx.mutex = NULL;
    s_sio_ctx.activation_pending = false;
    ts_source_runtime_deinit();
    return ESP_OK;
}

/*===========================================================================*/
/*                              数据采集                                      */
/*===========================================================================*/

esp_err_t ts_source_start_all(void)
{
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    ts_source_runtime.running = true;
    xSemaphoreGive(ts_source_runtime.mutex);
    ts_source_config_activate();
    return ESP_OK;
}

esp_err_t ts_source_stop_all(void)
{
    if (!ts_source_runtime.initialized) return ESP_OK;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    ts_source_runtime.running = false;
    for (int i = 0; i < ts_source_runtime.count; ++i) ts_source_runtime.sources[i].connected = false;
    xSemaphoreGive(ts_source_runtime.mutex);
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i) {
        sio_connection_t *conn = &s_sio_ctx.connections[i];
        if (!conn->allocated) continue;
        conn->should_stop = true;
        if (conn->task_handle) xTaskNotifyGive(conn->task_handle);
    }
    xSemaphoreGive(s_sio_ctx.mutex);
    return ESP_OK;
}

static esp_err_t poll_instance(const char *id, uint64_t expected)
{
    if (!id || !ts_source_runtime.initialized) return ESP_ERR_INVALID_ARG;
    ts_auto_source_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int index = find_source_index(id);
    esp_err_t ret = ts_source_runtime.closing ? ESP_ERR_INVALID_STATE : index < 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (ret == ESP_OK) {
        *copy = ts_source_runtime.sources[index];
        if (!copy->enabled || (expected && expected != copy->instance)) ret = ESP_ERR_INVALID_STATE;
        else ++ts_source_runtime.active_polls;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    if (ret != ESP_OK) { free(copy);return ret; }
    ts_auto_value_t value = {0};
    if (copy->type == TS_AUTO_SRC_REST) ret = read_rest_source(copy, &value);
    else if (copy->type == TS_AUTO_SRC_VARIABLE) {
        ts_variable_registration_begin();
        xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
        ret = current_source_index(id, copy->instance) >= 0 ? read_variable_source(copy, &value) : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(ts_source_runtime.mutex);
        ts_variable_registration_end();
    } else ret = copy->type == TS_AUTO_SRC_SOCKETIO || copy->type == TS_AUTO_SRC_WEBSOCKET ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    ++ts_source_runtime.stats.total_polls;
    if (copy->type == TS_AUTO_SRC_REST) ++ts_source_runtime.stats.rest_requests;
    index = current_source_index(id, copy->instance);
    if (ret == ESP_OK && index >= 0) {
        ts_source_runtime.sources[index].last_value = value;
        ts_source_runtime.sources[index].last_update_ms = esp_timer_get_time() / 1000;
        ++ts_source_runtime.stats.successful_polls;
    } else {
        ++ts_source_runtime.stats.failed_polls;
        if (ret == ESP_OK) ret = ESP_ERR_INVALID_STATE;
    }
    --ts_source_runtime.active_polls;
    xSemaphoreGive(ts_source_runtime.mutex);
    free(copy);return ret;
}

esp_err_t ts_source_poll(const char *id) { return poll_instance(id, 0); }

int ts_source_poll_all(void)
{
    if (!ts_source_runtime.initialized) return 0;
    if (atomic_exchange(&s_sio_ctx.activation_pending, false)) ts_source_config_activate();
    char ids[CONFIG_TS_AUTOMATION_MAX_SOURCES][TS_AUTO_NAME_MAX_LEN];
    uint64_t instances[CONFIG_TS_AUTOMATION_MAX_SOURCES];
    int count = 0;
    int64_t now = esp_timer_get_time() / 1000;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; ts_source_runtime.running && i < ts_source_runtime.count; ++i) {
        const ts_auto_source_t *source = &ts_source_runtime.sources[i];
        if (!source->enabled || source->type == TS_AUTO_SRC_SOCKETIO || source->type == TS_AUTO_SRC_WEBSOCKET ||
            (source->poll_interval_ms && now - source->last_update_ms < source->poll_interval_ms)) continue;
        strcpy(ids[count], source->id);instances[count++] = source->instance;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    int polled = 0;
    for (int i = 0; i < count; ++i) polled += poll_instance(ids[i], instances[i]) == ESP_OK;
    return polled;
}

/*===========================================================================*/
/*                              WebSocket 源                                  */
/*===========================================================================*/

static esp_err_t stream_connect(const char *id, ts_auto_source_type_t type);

esp_err_t ts_source_ws_connect(const char *id)
{
    return stream_connect(id, TS_AUTO_SRC_WEBSOCKET);
}

esp_err_t ts_source_ws_disconnect(const char *id)
{
    return ts_source_sio_disconnect(id);
}

bool ts_source_ws_is_connected(const char *id)
{
    if (!id || !ts_source_runtime.initialized) {
        return false;
    }

    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);

    int idx = find_source_index(id);
    bool connected = false;
    if (idx >= 0 && ts_source_runtime.sources[idx].type == TS_AUTO_SRC_WEBSOCKET) {
        connected = ts_source_runtime.sources[idx].connected;
    }

    xSemaphoreGive(ts_source_runtime.mutex);
    return connected;
}

/*===========================================================================*/
/*                              Socket.IO 源                                  */
/*===========================================================================*/

/**
 * 查找 Socket.IO 连接
 */
void ts_source_sio_websocket_url(const char *base_url, const char *sid, char *out, size_t size)
{
    const char *host = strstr(base_url, "://");
    host = host ? host + 3 : base_url;
    snprintf(out, size, "%s://%s/socket.io/?EIO=4&transport=websocket&sid=%s",
             !strncmp(base_url, "https://", 8) ? "wss" : "ws", host, sid);
}

/* Caller owns the connection mutex; slots never move while a task uses them. */
static sio_connection_t *sio_find_connection(const char *id)
{
    for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i)
        if (s_sio_ctx.connections[i].allocated && !strcmp(s_sio_ctx.connections[i].source_id, id)) return &s_sio_ctx.connections[i];
    return NULL;
}

static void sio_release_connection(sio_connection_t *conn, bool activate)
{
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    conn->task_handle = NULL;
    if (activate) s_sio_ctx.activation_pending = true;
    xSemaphoreGive(s_sio_ctx.mutex);
    /* This is the task's final access to the slot. Deinit/reuse may proceed
     * as soon as it observes false, so no mutex operation may follow it. */
    conn->allocated = false;
}

void ts_source_transport_changed(void)
{
    if (!s_sio_ctx.mutex) return;
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i) {
        sio_connection_t *conn = &s_sio_ctx.connections[i];
        if (conn->allocated && current_connection_index(conn) < 0) {
            conn->should_stop = true;
            if (conn->task_handle) xTaskNotifyGive(conn->task_handle);
        }
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    xSemaphoreGive(s_sio_ctx.mutex);
}

void ts_source_config_activate(void)
{
    if (!ts_source_runtime.initialized) return;
    char ids[CONFIG_TS_AUTOMATION_MAX_SOURCES][TS_AUTO_NAME_MAX_LEN];
    ts_auto_source_type_t types[CONFIG_TS_AUTOMATION_MAX_SOURCES];
    int count = 0;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; ts_source_runtime.running && i < ts_source_runtime.count; ++i)
        if (ts_source_runtime.sources[i].enabled &&
            (ts_source_runtime.sources[i].type == TS_AUTO_SRC_SOCKETIO || ts_source_runtime.sources[i].type == TS_AUTO_SRC_WEBSOCKET)) {
            types[count] = ts_source_runtime.sources[i].type;
            strcpy(ids[count++], ts_source_runtime.sources[i].id);
        }
    xSemaphoreGive(ts_source_runtime.mutex);
    for (int i = 0; i < count; ++i) {
        esp_err_t ret = stream_connect(ids[i], types[i]);
        if (ret == ESP_ERR_NOT_FINISHED) s_sio_ctx.activation_pending = true;
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED)
            ESP_LOGW(TAG, "Source %s is saved; connection start failed: %s", ids[i], esp_err_to_name(ret));
    }
}

/**
 * 处理 Socket.IO 事件消息
 * 格式：42["event_name", {...data...}]
 */
static void sio_handle_event_message(sio_connection_t *conn, const char *msg)
{
    if (!msg || strncmp(msg, SIO_EVENT_PREFIX, 2) != 0) {
        return;
    }

    const char *json_start = strchr(msg, '[');
    if (!json_start) return;

    cJSON *array = cJSON_Parse(json_start);
    if (!array || !cJSON_IsArray(array)) {
        if (array) cJSON_Delete(array);
        return;
    }

    cJSON *event_name = cJSON_GetArrayItem(array, 0);
    cJSON *event_data = cJSON_GetArrayItem(array, 1);

    if (!event_name || !cJSON_IsString(event_name)) {
        cJSON_Delete(array);
        return;
    }

    ESP_LOGD(TAG, "SIO event: %s from source %s", event_name->valuestring, conn->source_id);

    // 查找关联的数据源
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int idx = current_connection_index(conn);
    if (idx >= 0) {
        ts_auto_source_t *src = &ts_source_runtime.sources[idx];

        // 检查是否是我们订阅的事件
        if (strcmp(event_name->valuestring, src->socketio.event) == 0 && event_data) {
            ESP_LOGD(TAG, "Matched subscribed event '%s', updating source value...", event_name->valuestring);

            // 更新源的 last_value
            // 由于 str_val 只有 64 字节，存储摘要而非完整 JSON
            char *json_str = cJSON_PrintUnformatted(event_data);
            if (json_str) {
                size_t json_len = strlen(json_str);
                src->last_value.type = TS_AUTO_VAL_STRING;

                // 存储格式：前50字符... (总大小)
                if (json_len > 50) {
                    snprintf(src->last_value.str_val, sizeof(src->last_value.str_val),
                             "%.45s...(%zu)", json_str, json_len);
                } else {
                    strncpy(src->last_value.str_val, json_str, sizeof(src->last_value.str_val) - 1);
                    src->last_value.str_val[sizeof(src->last_value.str_val) - 1] = '\0';
                }
                ESP_LOGD(TAG, "Source '%s' last_value: %s", src->id, src->last_value.str_val);

                // 处理数据到变量 - 所有处理都在主任务中执行，避免栈溢出
                // （mapping 可能需要递归展开数组/对象，auto_discover 也是递归的）
                if (src->mapping_count > 0 || src->auto_discover) {
                    if (xSemaphoreTake(conn->pending_mutex, 0) == pdTRUE) {
                        if (conn->pending_json) {
                            free(conn->pending_json);  // 释放旧数据
                        }
                        conn->pending_json = json_str;  // 转移所有权
                        json_str = NULL;
                        xSemaphoreGive(conn->pending_mutex);
                        ESP_LOGD(TAG, "SIO source '%s': queued JSON for processing", src->id);
                    } else {
                        ESP_LOGW(TAG, "SIO source '%s': pending mutex busy, dropping data", src->id);
                    }
                }

                if (json_str) free(json_str);
            }

            // 更新源状态
            src->last_update_ms = esp_timer_get_time() / 1000;
            src->connected = true;
        }
    }
    xSemaphoreGive(ts_source_runtime.mutex);

    cJSON_Delete(array);
}

/**
 * Socket.IO WebSocket 事件处理器
 */
static void ws_handle_message(sio_connection_t *conn, const char *message)
{
    if (!conn->connected) return;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int index = current_connection_index(conn);
    if (index >= 0) {
        ts_auto_source_t *src = &ts_source_runtime.sources[index];
        if (!src->websocket.path[0]) {
            src->last_value.type = TS_AUTO_VAL_STRING;
            snprintf(src->last_value.str_val, sizeof(src->last_value.str_val), "%s", message);
        }
        if (src->mapping_count || src->auto_discover || src->websocket.path[0]) {
            if (xSemaphoreTake(conn->pending_mutex, 0) == pdTRUE) {
                char *copy = strdup(message);
                if (copy) { free(conn->pending_json); conn->pending_json = copy; }
                xSemaphoreGive(conn->pending_mutex);
            }
        }
        src->last_update_ms = esp_timer_get_time() / 1000;
        src->connected = true;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
}

static void sio_websocket_event_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    sio_connection_t *conn = (sio_connection_t *)arg;
    if (conn->should_stop) return;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGD(TAG, "SIO [%s] WebSocket connected, sending probe", conn->source_id);
            conn->connected = true;
            conn->last_message_ms = esp_timer_get_time() / 1000;
            if (conn->type == TS_AUTO_SRC_WEBSOCKET) {
                conn->upgraded = true;
                xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
                int index = current_connection_index(conn);
                if (index >= 0) ts_source_runtime.sources[index].connected = true;
                xSemaphoreGive(ts_source_runtime.mutex);
            } else {
                esp_websocket_client_send_text(conn->client, SIO_PROBE_MSG, strlen(SIO_PROBE_MSG), portMAX_DELAY);
            }
            break;

        case WEBSOCKET_EVENT_CLOSED:
            if (conn->type != TS_AUTO_SRC_WEBSOCKET) break;
            /* A clean server close has a distinct event in esp-websocket. */
            /* fall through */
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "SIO [%s] WebSocket disconnected", conn->source_id);
            conn->connected = false;
            conn->upgraded = false;
            if (conn->type == TS_AUTO_SRC_WEBSOCKET) conn->msg_buf_len = 0;
            // 更新源状态
            xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
            int idx = current_connection_index(conn);
            if (idx >= 0) {
                ts_source_runtime.sources[idx].connected = false;
            }
            xSemaphoreGive(ts_source_runtime.mutex);
            break;

        case WEBSOCKET_EVENT_DATA:
            if ((data->data_len > 0 && data->data_ptr) ||
                (conn->type == TS_AUTO_SRC_WEBSOCKET && data->data_len == 0 &&
                 data->fin && conn->msg_buf_len > 0)) {
                if (conn->type == TS_AUTO_SRC_WEBSOCKET && data->op_code >= 8) break;
                conn->last_message_ms = esp_timer_get_time() / 1000;

                // 检查是否是分片消息
                bool is_complete = (data->payload_len == 0) ||
                                   (data->payload_offset + data->data_len >= data->payload_len);
                if (conn->type == TS_AUTO_SRC_WEBSOCKET) is_complete = is_complete && data->fin;

                // 如果是分片消息，需要缓冲
                if (!is_complete || conn->msg_buf_len > 0) {
                    // 分配或扩展缓冲区
                    if (!conn->msg_buf) {
                        conn->msg_buf = heap_caps_malloc(SIO_MSG_BUF_SIZE, MALLOC_CAP_SPIRAM);
                        if (!conn->msg_buf) conn->msg_buf = malloc(SIO_MSG_BUF_SIZE);
                        if (!conn->msg_buf) {
                            ESP_LOGE(TAG, "SIO [%s] failed to alloc msg buffer", conn->source_id);
                            break;
                        }
                        conn->msg_buf_len = 0;
                    }

                    // 追加数据到缓冲区
                    size_t copy_len = data->data_len;
                    if (conn->msg_buf_len + copy_len >= SIO_MSG_BUF_SIZE) {
                        copy_len = SIO_MSG_BUF_SIZE - conn->msg_buf_len - 1;
                        ESP_LOGW(TAG, "SIO [%s] msg buffer overflow, truncating", conn->source_id);
                    }
                    if (copy_len > 0) memcpy(conn->msg_buf + conn->msg_buf_len, data->data_ptr, copy_len);
                    conn->msg_buf_len += copy_len;
                    conn->msg_buf[conn->msg_buf_len] = '\0';

                    ESP_LOGD(TAG, "SIO [%s] buffered %d bytes, total %zu",
                             conn->source_id, data->data_len, conn->msg_buf_len);

                    if (!is_complete) {
                        // 还有更多分片，等待
                        break;
                    }

                    // 消息完整，处理缓冲区中的消息
                    ESP_LOGD(TAG, "SIO [%s] complete msg: %zu bytes", conn->source_id, conn->msg_buf_len);

                    // 处理事件消息
                    if (conn->type == TS_AUTO_SRC_WEBSOCKET) {
                        ws_handle_message(conn, conn->msg_buf);
                    } else if (strncmp(conn->msg_buf, SIO_EVENT_PREFIX, 2) == 0) {
                        sio_handle_event_message(conn, conn->msg_buf);
                    }

                    // 清空缓冲区
                    conn->msg_buf_len = 0;
                } else {
                    // 完整的单帧消息
                    char *msg = strndup(data->data_ptr, data->data_len);
                    if (msg) {
                        ESP_LOGD(TAG, "SIO [%s] recv: %.100s%s", conn->source_id, msg,
                                 strlen(msg) > 100 ? "..." : "");

                        // 处理 Socket.IO 协议消息
                        if (conn->type == TS_AUTO_SRC_WEBSOCKET) {
                            ws_handle_message(conn, msg);
                        }
                        else if (strcmp(msg, "3probe") == 0) {
                            ESP_LOGD(TAG, "SIO [%s] probe response, sending upgrade", conn->source_id);
                            esp_websocket_client_send_text(conn->client, SIO_UPGRADE_MSG,
                                                           strlen(SIO_UPGRADE_MSG), portMAX_DELAY);
                        }
                        else if (strcmp(msg, "6") == 0) {
                            // 收到 NOOP (6) 表示升级完成，发送 CONNECT 到默认命名空间
                            ESP_LOGD(TAG, "SIO [%s] upgrade ack, sending CONNECT", conn->source_id);
                            esp_websocket_client_send_text(conn->client, "40", 2, portMAX_DELAY);
                        }
                        else if (strncmp(msg, "40{", 3) == 0 || strcmp(msg, "40") == 0) {
                            // 收到 CONNECT 响应，连接完成
                            conn->upgraded = true;
                            xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
                            int i = current_connection_index(conn);
                            if (i >= 0) {
                                ts_source_runtime.sources[i].connected = true;
                            }
                            xSemaphoreGive(ts_source_runtime.mutex);
                            ESP_LOGD(TAG, "SIO [%s] connected to namespace", conn->source_id);
                        }
                        else if (strcmp(msg, SIO_PING_MSG) == 0) {
                            ESP_LOGD(TAG, "SIO [%s] ping, sending pong", conn->source_id);
                            esp_websocket_client_send_text(conn->client, SIO_PONG_MSG,
                                                           strlen(SIO_PONG_MSG), portMAX_DELAY);
                        }
                        else if (strncmp(msg, SIO_EVENT_PREFIX, 2) == 0) {
                            sio_handle_event_message(conn, msg);
                        }

                        free(msg);
                    }
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "SIO [%s] WebSocket error", conn->source_id);
            break;
    }
}

/**
 * Socket.IO HTTP 握手获取 session ID
 */
static esp_err_t sio_http_handshake(const char *base_url, char *sid_out, size_t sid_len)
{
    char handshake_url[256];
    snprintf(handshake_url, sizeof(handshake_url),
             "%s/socket.io/?EIO=4&transport=polling", base_url);

    char *response_buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!response_buf) response_buf = malloc(1024);
    if (!response_buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = handshake_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t ret = ESP_FAIL;

    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int len = esp_http_client_read(client, response_buf, 1023);
        if (len > 0) {
            response_buf[len] = '\0';
            ESP_LOGD(TAG, "SIO handshake response: %s", response_buf);

            // 响应格式：0{"sid":"xxx","upgrades":["websocket"],...}
            char *json_start = strchr(response_buf, '{');
            if (json_start) {
                cJSON *root = cJSON_Parse(json_start);
                if (root) {
                    cJSON *sid = cJSON_GetObjectItem(root, "sid");
                    if (sid && cJSON_IsString(sid)) {
                        strncpy(sid_out, sid->valuestring, sid_len - 1);
                        ret = ESP_OK;
                        ESP_LOGD(TAG, "SIO got session ID: %s", sid_out);
                    }
                    cJSON_Delete(root);
                }
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(response_buf);

    return ret;
}

/**
 * Socket.IO 连接任务（带自动重连）
 *
 * 连接流程：
 * 1. 等待网络就绪
 * 2. 尝试 HTTP 握手，失败则延迟重试（指数退避）
 * 3. 建立 WebSocket 连接
 * 4. 保持连接，处理数据
 * 5. 断开后自动重连
 */
static void sio_connection_task(void *arg)
{
    sio_connection_t *conn = (sio_connection_t *)arg;

    // 保存 source_id 副本用于清理
    char source_id_copy[TS_AUTO_NAME_MAX_LEN];
    strncpy(source_id_copy, conn->source_id, sizeof(source_id_copy) - 1);
    source_id_copy[sizeof(source_id_copy) - 1] = '\0';

    // 重连参数
    const uint32_t INITIAL_RETRY_DELAY_MS = 5000;   // 初始重试延迟 5秒
    const uint32_t MAX_RETRY_DELAY_MS = 60000;      // 最大重试延迟 60秒
    uint32_t retry_delay_ms = INITIAL_RETRY_DELAY_MS;
    int retry_count = 0;
    int idx = -1;  // 源索引，在函数级别声明

connection_retry:
    while (!conn->should_stop) {
        // 检查源是否仍然存在
        xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
        idx = current_connection_index(conn);
        if (idx < 0) {
            xSemaphoreGive(ts_source_runtime.mutex);
            ESP_LOGE(TAG, "SIO source not found: %s", conn->source_id);
            goto task_exit;
        }
        char base_url[TS_AUTO_PATH_MAX_LEN];
        const ts_auto_source_t *source = &ts_source_runtime.sources[idx];
        strncpy(base_url, conn->type == TS_AUTO_SRC_WEBSOCKET ? source->websocket.uri : source->socketio.url, sizeof(base_url) - 1);
        base_url[sizeof(base_url) - 1] = '\0';

        // 获取配置的重连间隔（如果有）
        uint32_t configured_reconnect_ms = conn->type == TS_AUTO_SRC_WEBSOCKET ? source->websocket.reconnect_ms : source->socketio.reconnect_ms;
        if (configured_reconnect_ms > 0) {
            // 使用配置值作为初始延迟
            if (retry_count == 0) {
                retry_delay_ms = configured_reconnect_ms;
            }
        }
        xSemaphoreGive(ts_source_runtime.mutex);

        if (retry_count > 0) {
            ESP_LOGD(TAG, "SIO [%s] retry #%d, waiting %lu ms before reconnect...",
                     conn->source_id, retry_count, (unsigned long)retry_delay_ms);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retry_delay_ms));
            if (conn->should_stop) goto task_exit;

            // 指数退避，最大不超过 MAX_RETRY_DELAY_MS
            retry_delay_ms = (retry_delay_ms * 2 > MAX_RETRY_DELAY_MS)
                           ? MAX_RETRY_DELAY_MS
                           : retry_delay_ms * 2;
        }

        ESP_LOGD(TAG, "SIO [%s] attempting connection to %s", conn->source_id, base_url);

        if (conn->type == TS_AUTO_SRC_WEBSOCKET) break;

        // 1. HTTP 握手获取 session ID
        if (sio_http_handshake(base_url, conn->session_id, sizeof(conn->session_id)) != ESP_OK) {
            ESP_LOGW(TAG, "SIO [%s] handshake failed (host unreachable?), will retry", conn->source_id);
            retry_count++;
            continue;  // 重试
        }

        // 握手成功，重置重试计数
        retry_count = 0;
        retry_delay_ms = INITIAL_RETRY_DELAY_MS;

        // 跳出重试循环，继续建立 WebSocket 连接
        break;
    }

    if (conn->should_stop) {
        goto task_exit;
    }

    // 重新获取 base_url（需要新的变量名避免作用域冲突）
    char ws_base_url[TS_AUTO_PATH_MAX_LEN];
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    idx = current_connection_index(conn);
    if (idx < 0) {
        xSemaphoreGive(ts_source_runtime.mutex);
        goto task_exit;
    }
    const ts_auto_source_t *source = &ts_source_runtime.sources[idx];
    strncpy(ws_base_url, conn->type == TS_AUTO_SRC_WEBSOCKET ? source->websocket.uri : source->socketio.url, sizeof(ws_base_url) - 1);
    ws_base_url[sizeof(ws_base_url) - 1] = '\0';
    uint32_t ws_reconnect_ms = conn->type == TS_AUTO_SRC_WEBSOCKET ? source->websocket.reconnect_ms : 0;
    xSemaphoreGive(ts_source_runtime.mutex);

    // 2. 构建 WebSocket URL
    char ws_url[256];
    if (conn->type == TS_AUTO_SRC_WEBSOCKET) {
        snprintf(ws_url, sizeof(ws_url), "%s", ws_base_url);
    } else {
        ts_source_sio_websocket_url(ws_base_url, conn->session_id, ws_url, sizeof(ws_url));
    }

    ESP_LOGD(TAG, "SIO [%s] WebSocket URL: %s", conn->source_id, ws_url);

    // 3. 创建 WebSocket 连接
    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .buffer_size = 2048,
        .reconnect_timeout_ms = conn->type == TS_AUTO_SRC_WEBSOCKET && ws_reconnect_ms ? ws_reconnect_ms : 10000,
        .enable_close_reconnect = conn->type == TS_AUTO_SRC_WEBSOCKET,
        .network_timeout_ms = 10000,     /* 网络超时 10s */
        .ping_interval_sec = 0,          /* 禁用 WebSocket ping，使用 Socket.IO ping */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    conn->client = esp_websocket_client_init(&ws_cfg);
    if (!conn->client) {
        ESP_LOGE(TAG, "SIO [%s] failed to init WebSocket client", conn->source_id);
        goto task_exit;
    }

    if (esp_websocket_register_events(conn->client, WEBSOCKET_EVENT_ANY,
                                     sio_websocket_event_handler, conn) != ESP_OK) goto task_exit;

    if (esp_websocket_client_start(conn->client) != ESP_OK) {
        ESP_LOGE(TAG, "SIO [%s] failed to start WebSocket client", conn->source_id);
        esp_websocket_client_destroy(conn->client);
        conn->client = NULL;
        goto task_exit;
    }

    // 4. 主循环：保持连接，处理心跳超时和待处理数据
    while (!conn->should_stop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (conn->should_stop) break;

        // 处理待处理的自动发现数据（在此任务中执行，有足够栈空间）
        if (xSemaphoreTake(conn->pending_mutex, 0) == pdTRUE) {
            if (conn->pending_json) {
                char *json_to_process = conn->pending_json;
                conn->pending_json = NULL;
                xSemaphoreGive(conn->pending_mutex);

                // 解析 JSON
                cJSON *parsed = cJSON_Parse(json_to_process);
                if (parsed) {
                    ts_variable_registration_begin();
                    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
                    int idx = current_connection_index(conn);
                    if (idx >= 0) {
                        ts_auto_source_t *src = &ts_source_runtime.sources[idx];
                        if (conn->type == TS_AUTO_SRC_WEBSOCKET && src->websocket.path[0]) {
                            ts_auto_value_t value = {0};
                            if (extract_json_value(parsed, src->websocket.path, &value) == ESP_OK)
                                src->last_value = value;
                        }

                        // 先处理手动配置的映射（在主任务中执行，有足够栈空间）
                        if (src->mapping_count > 0) {
                            int mapped = process_source_mappings(src, parsed);
                            ESP_LOGD(TAG, "SIO source '%s': processed %d/%d mappings",
                                     src->id, mapped, src->mapping_count);
                        }

                        // 如果启用了自动发现
                        if (src->auto_discover) {
                            if (!conn->auto_discovered) {
                                // 首次：自动发现并创建变量
                                int discovered = auto_discover_json_fields(src, parsed, NULL, 3, true);
                                conn->auto_discovered = true;
                                src->auto_discovered = true;
                                ESP_LOGD(TAG, "SIO source '%s': auto-discovered %d variables (first time)",
                                         src->id, discovered);
                            } else {
                                // 后续：只更新变量值（不尝试创建新变量）
                                int updated = auto_discover_json_fields(src, parsed, NULL, 3, false);
                                ESP_LOGD(TAG, "SIO source '%s': updated %d variables", src->id, updated);
                            }
                        }
                    }
                    xSemaphoreGive(ts_source_runtime.mutex);
                    ts_variable_registration_end();
                    cJSON_Delete(parsed);
                }
                free(json_to_process);
            } else {
                xSemaphoreGive(conn->pending_mutex);
            }
        }

        // 检查心跳超时（30秒无消息则重连）
        uint32_t now_ms = esp_timer_get_time() / 1000;
        if (conn->type == TS_AUTO_SRC_SOCKETIO && conn->upgraded && (uint32_t)(now_ms - conn->last_message_ms) > 30000) {
            ESP_LOGW(TAG, "SIO [%s] heartbeat timeout, reconnecting...", conn->source_id);
            break;
        }
    }

    // 清理 WebSocket 资源（但保留连接槽以便重连）
    if (conn->client) {
        esp_websocket_client_stop(conn->client);
        esp_websocket_client_destroy(conn->client);
        conn->client = NULL;
    }
    if (conn->msg_buf) {
        free(conn->msg_buf);
        conn->msg_buf = NULL;
        conn->msg_buf_len = 0;
    }
    if (conn->pending_json) {
        free(conn->pending_json);
        conn->pending_json = NULL;
    }
    conn->connected = false;
    conn->upgraded = false;

    // 检查是否应该重连
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    idx = current_connection_index(conn);
    bool should_reconnect = (idx >= 0);  // 源仍然存在则重连
    xSemaphoreGive(ts_source_runtime.mutex);

    if (should_reconnect && !conn->should_stop) {
        ESP_LOGD(TAG, "SIO [%s] will reconnect...", source_id_copy);
        retry_count++;  // 增加重试计数，触发延迟
        goto connection_retry;
    }

task_exit:
    if (conn->client) {
        esp_websocket_client_stop(conn->client);
        esp_websocket_client_destroy(conn->client);
        conn->client = NULL;
    }
    free(conn->msg_buf);conn->msg_buf = NULL;conn->msg_buf_len = 0;
    free(conn->pending_json);conn->pending_json = NULL;
    if (conn->pending_mutex) { vSemaphoreDelete(conn->pending_mutex);conn->pending_mutex = NULL; }
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int remaining = find_source_index(conn->source_id);
    bool activate = remaining < 0 || ts_source_runtime.sources[remaining].instance != conn->source_instance;
    if (remaining >= 0 && !activate) ts_source_runtime.sources[remaining].connected = false;
    xSemaphoreGive(ts_source_runtime.mutex);
    ESP_LOGI(TAG, "SIO [%s] connection task ended", source_id_copy);
    sio_release_connection(conn, activate);
    vTaskDeleteWithCaps(NULL);
}

/**
 * @brief 连接 Socket.IO 数据源
 */
static esp_err_t stream_connect(const char *id, ts_auto_source_type_t type)
{
    if (!id || !ts_source_runtime.initialized) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int index = find_source_index(id);
    bool ready = index >= 0 && ts_source_runtime.running && ts_source_runtime.sources[index].enabled &&
        ts_source_runtime.sources[index].type == type;
    uint64_t instance = ready ? ts_source_runtime.sources[index].instance : 0;
    xSemaphoreGive(ts_source_runtime.mutex);
    if (!ready) { xSemaphoreGive(s_sio_ctx.mutex);return ESP_ERR_INVALID_STATE; }
    sio_connection_t *conn = sio_find_connection(id);
    if (conn) {
        esp_err_t ret = conn->source_instance == instance && !conn->should_stop ? ESP_OK : ESP_ERR_NOT_FINISHED;
        if (ret != ESP_OK) { conn->should_stop = true;if (conn->task_handle) xTaskNotifyGive(conn->task_handle); }
        xSemaphoreGive(s_sio_ctx.mutex);return ret;
    }
    for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i)
        if (!s_sio_ctx.connections[i].allocated) { conn = &s_sio_ctx.connections[i];break; }
    if (!conn) {
        bool closing = false;
        for (int i = 0; i < SIO_MAX_CONNECTIONS; ++i) closing |= s_sio_ctx.connections[i].should_stop;
        xSemaphoreGive(s_sio_ctx.mutex);return closing ? ESP_ERR_NOT_FINISHED : ESP_ERR_NO_MEM;
    }
    memset(conn, 0, sizeof(*conn));
    strcpy(conn->source_id, id);conn->source_instance = instance;conn->type = type;
    conn->pending_mutex = xSemaphoreCreateMutex();
    if (!conn->pending_mutex) { xSemaphoreGive(s_sio_ctx.mutex);return ESP_ERR_NO_MEM; }
    conn->allocated = true;
    char task_name[32];snprintf(task_name, sizeof(task_name), "sio_%.27s", id);
    BaseType_t created = xTaskCreateWithCaps(sio_connection_task, task_name, 6144, conn, 5,
                                            &conn->task_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        conn->allocated = false;vSemaphoreDelete(conn->pending_mutex);conn->pending_mutex = NULL;
    }
    xSemaphoreGive(s_sio_ctx.mutex);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ts_source_sio_connect(const char *id)
{
    return stream_connect(id, TS_AUTO_SRC_SOCKETIO);
}

esp_err_t ts_source_sio_disconnect(const char *id)
{
    if (!id || !s_sio_ctx.mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    sio_connection_t *conn = sio_find_connection(id);
    if (conn) { conn->should_stop = true;if (conn->task_handle) xTaskNotifyGive(conn->task_handle); }
    xSemaphoreGive(s_sio_ctx.mutex);
    return conn ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool ts_source_sio_is_connected(const char *id)
{
    if (!id || !s_sio_ctx.mutex) return false;
    xSemaphoreTake(s_sio_ctx.mutex, portMAX_DELAY);
    sio_connection_t *conn = sio_find_connection(id);
    bool connected = conn && !conn->should_stop && conn->upgraded;
    xSemaphoreGive(s_sio_ctx.mutex);
    return connected;
}

/*===========================================================================*/
/*                              REST 源                                       */
/*===========================================================================*/

esp_err_t ts_source_rest_fetch(const char *id)
{
    return ts_source_poll(id);
}

/*===========================================================================*/
/*                              枚举                                          */
/*===========================================================================*/

/*===========================================================================*/
/*                              统计                                          */
/*===========================================================================*/

esp_err_t ts_source_manager_get_stats(ts_source_manager_stats_t *stats)
{
    if (!stats || !ts_source_runtime.initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    *stats = ts_source_runtime.stats;
    xSemaphoreGive(ts_source_runtime.mutex);

    return ESP_OK;
}
