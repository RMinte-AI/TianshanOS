/**
 * @file ts_temp_source.c
 * @brief Temperature Source Management Implementation
 * 
 * 温度源管理服务实现
 * - 多 provider 支持（AGX/本地传感器/手动）
 * - 优先级选择
 * - 事件发布
 */

#include "ts_temp_source.h"
#include "ts_event.h"
#include "ts_log.h"
#include "ts_variable.h"
#include "ts_config_module.h"
#include "ts_config_pack.h"
#include "ts_storage.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define TAG "ts_temp_source"

/** NVS 存储键 */
#define NVS_NAMESPACE       "ts_temp"
#define NVS_KEY_PREFERRED   "preferred"
#define NVS_KEY_BOUND_VAR   "bound_var"
#define NVS_KEY_BOUND_VARS  "bound_vars"

/*===========================================================================*/
/*                          Internal Types                                    */
/*===========================================================================*/

typedef struct {
    ts_temp_source_type_t type;
    const char *name;
    int16_t value;
    int64_t last_update_ms;
    uint32_t update_count;
    bool registered;
    bool active;
} provider_t;

typedef struct {
    bool initialized;
    bool manual_mode;
    int16_t manual_temp;
    int16_t current_temp;
    uint32_t identity_revision;
    bool variable_valid;
    int64_t variable_last_update_ms;
    int16_t variable_guard_temp;
    bool variable_guard_valid;
    bool variable_partial_stale;
    uint8_t variable_valid_count;
    uint8_t variable_total_count;
    float variable_valid_weight;
    float variable_total_weight;
    ts_temp_source_type_t active_source;
    ts_temp_source_type_t preferred_source;  /**< 用户首选源（0=自动）*/
    char bound_variable[TS_TEMP_MAX_VARNAME_LEN];  /**< 向后兼容：第一个绑定变量名 */
    ts_temp_bound_var_t bound_vars[TS_TEMP_MAX_BOUND_VARS]; /**< 加权绑定变量 */
    uint8_t bound_var_count;                /**< 绑定变量数量 */
    provider_t providers[TS_TEMP_SOURCE_MAX];
    SemaphoreHandle_t mutex;
} temp_source_state_t;

typedef struct {
    bool valid;
    int16_t temp;
    int64_t timestamp_ms;
    int16_t guard_temp;
    bool guard_valid;
    bool partial_stale;
    uint8_t valid_count;
    uint8_t total_count;
    float valid_weight;
    float total_weight;
} variable_temp_snapshot_t;

/*===========================================================================*/
/*                          Static Variables                                  */
/*===========================================================================*/

static temp_source_state_t s_state = {0};

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static int64_t get_current_ms(void);
static void evaluate_active_source(void);
static void evaluate_active_source_with_lock_mode(bool try_variable_lock);
static void publish_temp_event(int16_t new_temp, ts_temp_source_type_t new_source,
                               int16_t prev_temp, ts_temp_source_type_t prev_source);
static void sync_bound_variable_compat(void);
static esp_err_t save_bound_vars_to_nvs(void);
static esp_err_t load_preferred_source_from_nvs(void);
static esp_err_t save_preferred_source_to_nvs(ts_temp_source_type_t type);
static void export_temp_config_to_sdcard(void);
static float sanitize_bound_weight(float weight);
static bool has_positive_bound_weight(const ts_temp_bound_var_t *vars, uint8_t count);
static bool read_fresh_variable_float(const char *name, int64_t now, double *value,
                                      int64_t *last_update_ms, bool try_lock);
static bool read_variable_temp_snapshot(variable_temp_snapshot_t *snapshot, int64_t now,
                                        bool try_lock);

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

static int64_t get_current_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static float sanitize_bound_weight(float weight)
{
    if (weight < 0.0f) return 0.0f;
    if (weight > 1.0f) return 1.0f;
    return weight;
}

static bool has_positive_bound_weight(const ts_temp_bound_var_t *vars, uint8_t count)
{
    if (!vars || count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (sanitize_bound_weight(vars[i].weight) > 0.001f) {
            return true;
        }
    }

    return false;
}

static void store_variable_snapshot(const variable_temp_snapshot_t *snapshot)
{
    if (!snapshot) {
        s_state.variable_valid = false;
        s_state.variable_last_update_ms = 0;
        s_state.variable_guard_temp = TS_TEMP_DEFAULT_VALUE;
        s_state.variable_guard_valid = false;
        s_state.variable_partial_stale = false;
        s_state.variable_valid_count = 0;
        s_state.variable_total_count = 0;
        s_state.variable_valid_weight = 0.0f;
        s_state.variable_total_weight = 0.0f;
        return;
    }

    s_state.variable_valid = snapshot->valid;
    s_state.variable_last_update_ms = snapshot->timestamp_ms;
    s_state.variable_guard_temp = snapshot->guard_temp;
    s_state.variable_guard_valid = snapshot->guard_valid;
    s_state.variable_partial_stale = snapshot->partial_stale;
    s_state.variable_valid_count = snapshot->valid_count;
    s_state.variable_total_count = snapshot->total_count;
    s_state.variable_valid_weight = snapshot->valid_weight;
    s_state.variable_total_weight = snapshot->total_weight;
}

static bool read_fresh_variable_float(const char *name, int64_t now, double *value,
                                      int64_t *last_update_ms, bool try_lock)
{
    ts_variable_info_t var = {0};
    double temp_c = 0.0;
    esp_err_t ret;

    if (!name || !value) {
        return false;
    }

    ret = try_lock ? ts_variable_get_info_try(name, &var)
                   : ts_variable_get_info(name, &var);
    if (ret != ESP_OK) {
        return false;
    }

    if (var.last_update_ms <= 0) {
        return false;
    }

    int64_t age = now - var.last_update_ms;
    if (age < 0 || age > TS_TEMP_DATA_TIMEOUT_MS) {
        return false;
    }
    if (last_update_ms) {
        *last_update_ms = var.last_update_ms;
    }

    switch (var.value.type) {
        case TS_AUTO_VAL_FLOAT:
            temp_c = var.value.float_val;
            break;
        case TS_AUTO_VAL_INT:
            temp_c = (double)var.value.int_val;
            break;
        default:
            return false;
    }

    if (temp_c < (TS_TEMP_MIN_VALID / 10.0) ||
        temp_c > (TS_TEMP_MAX_VALID / 10.0)) {
        return false;
    }

    *value = temp_c;
    return true;
}

static bool read_variable_temp_snapshot(variable_temp_snapshot_t *snapshot, int64_t now,
                                        bool try_lock)
{
    double weighted_sum = 0.0;
    int16_t max_bound = TS_TEMP_MIN_VALID;

    if (!snapshot) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->guard_temp = TS_TEMP_DEFAULT_VALUE;
    if (s_state.bound_var_count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < s_state.bound_var_count; i++) {
        float w = sanitize_bound_weight(s_state.bound_vars[i].weight);
        if (w <= 0.001f) {
            continue;
        }

        snapshot->total_count++;
        snapshot->total_weight += w;

        double value = 0.0;
        int64_t last_update_ms = 0;
        if (!read_fresh_variable_float(s_state.bound_vars[i].name, now, &value,
                                       &last_update_ms, try_lock)) {
            TS_LOGD(TAG, "Variable '%s' unavailable or stale",
                    s_state.bound_vars[i].name);
            continue;
        }

        weighted_sum += value * w;
        snapshot->valid_weight += w;
        snapshot->valid_count++;
        int16_t temp_01c = (int16_t)(value * 10.0);
        if (temp_01c > max_bound) {
            max_bound = temp_01c;
        }
        if (snapshot->timestamp_ms == 0 || last_update_ms < snapshot->timestamp_ms) {
            snapshot->timestamp_ms = last_update_ms;
        }
    }

    snapshot->partial_stale = (snapshot->valid_count < snapshot->total_count);
    if (snapshot->valid_count == 0 || snapshot->valid_weight <= 0.001f) {
        return false;
    }

    snapshot->guard_temp = max_bound;
    snapshot->guard_valid = true;

    if (snapshot->partial_stale) {
        return false;
    }

    double effective_temp = weighted_sum / snapshot->valid_weight;
    int16_t temp_01c = (int16_t)(effective_temp * 10.0);
    if (temp_01c < TS_TEMP_MIN_VALID || temp_01c > TS_TEMP_MAX_VALID) {
        TS_LOGW(TAG, "Weighted temp out of range: %.1f°C", effective_temp);
        return false;
    }

    snapshot->temp = temp_01c;
    snapshot->valid = true;
    return true;
}

const char *ts_temp_source_type_to_str(ts_temp_source_type_t type)
{
    switch (type) {
        case TS_TEMP_SOURCE_DEFAULT:      return "default";
        case TS_TEMP_SOURCE_SENSOR_LOCAL: return "sensor";
        case TS_TEMP_SOURCE_AGX_AUTO:     return "agx";
        case TS_TEMP_SOURCE_VARIABLE:     return "variable";
        case TS_TEMP_SOURCE_MANUAL:       return "manual";
        default:                          return "unknown";
    }
}

/*===========================================================================*/
/*                          Core Logic                                        */
/*===========================================================================*/

/**
 * @brief 检查 provider 是否可用（已注册、激活且数据未过期）
 */
static bool is_provider_valid(ts_temp_source_type_t type, int64_t now, bool try_variable_lock)
{
    if (type >= TS_TEMP_SOURCE_MAX) return false;
    
    if (type == TS_TEMP_SOURCE_VARIABLE) {
        variable_temp_snapshot_t snapshot = {0};
        bool valid = read_variable_temp_snapshot(&snapshot, now, try_variable_lock);
        store_variable_snapshot(&snapshot);
        return valid;
    }
    
    provider_t *p = &s_state.providers[type];
    if (!p->registered || !p->active) return false;
    
    if (type == TS_TEMP_SOURCE_DEFAULT) return false;
    if (type == TS_TEMP_SOURCE_MANUAL) return p->registered && p->active;
    
    int64_t age = now - p->last_update_ms;
    return (age >= 0 && age < TS_TEMP_DATA_TIMEOUT_MS);
}

/**
 * @brief 同步 bound_variable 兼容字段
 */
static void sync_bound_variable_compat(void)
{
    if (s_state.bound_var_count > 0) {
        strncpy(s_state.bound_variable, s_state.bound_vars[0].name,
                sizeof(s_state.bound_variable) - 1);
        s_state.bound_variable[sizeof(s_state.bound_variable) - 1] = '\0';
    } else {
        s_state.bound_variable[0] = '\0';
    }
}

/**
 * @brief 根据优先级和数据有效性选择活动温度源
 * 
 * 选择逻辑：
 * 1. 手动模式最高优先级（无视 preferred_source）
 * 2. 如果设置了 preferred_source 且该源可用，使用它
 * 3. 否则按默认优先级（VARIABLE > AGX > SENSOR > DEFAULT）
 */
static void evaluate_active_source_with_lock_mode(bool try_variable_lock)
{
    int64_t now = get_current_ms();
    ts_temp_source_type_t best_source = TS_TEMP_SOURCE_DEFAULT;
    int16_t best_temp = TS_TEMP_DEFAULT_VALUE;
    variable_temp_snapshot_t variable_snapshot = {0};
    bool variable_snapshot_valid = read_variable_temp_snapshot(&variable_snapshot, now,
                                                               try_variable_lock);
    store_variable_snapshot(&variable_snapshot);
    
    // 1. 手动模式最高优先级
    if (s_state.manual_mode) {
        provider_t *p = &s_state.providers[TS_TEMP_SOURCE_MANUAL];
        if (p->registered && p->active) {
            best_source = TS_TEMP_SOURCE_MANUAL;
            best_temp = p->value;
            goto done;
        }
    }
    
    // 2. 检查用户首选源
    if (s_state.preferred_source != TS_TEMP_SOURCE_DEFAULT && 
        s_state.preferred_source != TS_TEMP_SOURCE_MANUAL) {
        
        // 变量绑定特殊处理
        if (s_state.preferred_source == TS_TEMP_SOURCE_VARIABLE) {
            if (variable_snapshot_valid) {
                best_source = TS_TEMP_SOURCE_VARIABLE;
                best_temp = variable_snapshot.temp;
                goto done;
            }
        } else if (is_provider_valid(s_state.preferred_source, now, try_variable_lock)) {
            best_source = s_state.preferred_source;
            best_temp = s_state.providers[s_state.preferred_source].value;
            goto done;
        }
        // 首选源不可用，降级到自动选择
        TS_LOGD(TAG, "Preferred source %s unavailable, falling back",
                ts_temp_source_type_to_str(s_state.preferred_source));
    }
    
    // 3. 默认优先级：VARIABLE > AGX > SENSOR > DEFAULT
    if (variable_snapshot_valid) {
        best_source = TS_TEMP_SOURCE_VARIABLE;
        best_temp = variable_snapshot.temp;
        goto done;
    }
    
    if (is_provider_valid(TS_TEMP_SOURCE_AGX_AUTO, now, try_variable_lock)) {
        best_source = TS_TEMP_SOURCE_AGX_AUTO;
        best_temp = s_state.providers[TS_TEMP_SOURCE_AGX_AUTO].value;
        goto done;
    }
    
    if (is_provider_valid(TS_TEMP_SOURCE_SENSOR_LOCAL, now, try_variable_lock)) {
        best_source = TS_TEMP_SOURCE_SENSOR_LOCAL;
        best_temp = s_state.providers[TS_TEMP_SOURCE_SENSOR_LOCAL].value;
        goto done;
    }
    
    // 4. 默认值
    best_source = TS_TEMP_SOURCE_DEFAULT;
    best_temp = TS_TEMP_DEFAULT_VALUE;

done:
    // 检查是否需要发布事件
    if (best_temp != s_state.current_temp || best_source != s_state.active_source) {
        int16_t prev_temp = s_state.current_temp;
        ts_temp_source_type_t prev_source = s_state.active_source;
        
        s_state.current_temp = best_temp;
        if (best_source != prev_source) s_state.identity_revision++;
        s_state.active_source = best_source;
        
        publish_temp_event(best_temp, best_source, prev_temp, prev_source);
    }
}

static void evaluate_active_source(void)
{
    evaluate_active_source_with_lock_mode(false);
}

static bool is_cached_active_source_valid_locked(int64_t now)
{
    ts_temp_source_type_t source = s_state.active_source;

    if (source == TS_TEMP_SOURCE_VARIABLE) {
        if (!s_state.variable_valid ||
            s_state.variable_last_update_ms <= 0 ||
            s_state.variable_partial_stale ||
            s_state.variable_total_count == 0 ||
            s_state.variable_valid_count < s_state.variable_total_count) {
            return false;
        }

        int64_t age = now - s_state.variable_last_update_ms;
        return age >= 0 && age < TS_TEMP_DATA_TIMEOUT_MS;
    }
    if (source == TS_TEMP_SOURCE_DEFAULT || source >= TS_TEMP_SOURCE_MAX) {
        return false;
    }
    if (source == TS_TEMP_SOURCE_MANUAL) {
        return s_state.providers[source].registered &&
               s_state.providers[source].active;
    }

    provider_t *p = &s_state.providers[source];
    if (!p->registered || !p->active || p->last_update_ms <= 0) {
        return false;
    }

    int64_t age = now - p->last_update_ms;
    return age >= 0 && age < TS_TEMP_DATA_TIMEOUT_MS;
}

/**
 * @brief 发布温度更新事件
 */
static void publish_temp_event(int16_t new_temp, ts_temp_source_type_t new_source,
                               int16_t prev_temp, ts_temp_source_type_t prev_source)
{
    ts_temp_event_data_t evt_data = {
        .temp = new_temp,
        .source = new_source,
        .prev_temp = prev_temp,
        .prev_source = prev_source,
    };
    
    ts_event_post(TS_EVENT_BASE_TEMP, TS_EVT_TEMP_UPDATED,
                  &evt_data, sizeof(evt_data), 0);
    
    TS_LOGD(TAG, "Temp: %.1f°C (%s) -> %.1f°C (%s)",
            prev_temp / 10.0f, ts_temp_source_type_to_str(prev_source),
            new_temp / 10.0f, ts_temp_source_type_to_str(new_source));
}

/*===========================================================================*/
/*                          Public API - Init                                 */
/*===========================================================================*/

esp_err_t ts_temp_source_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }
    
    memset(&s_state, 0, sizeof(s_state));
    
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        TS_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化默认 provider
    s_state.providers[TS_TEMP_SOURCE_DEFAULT].type = TS_TEMP_SOURCE_DEFAULT;
    s_state.providers[TS_TEMP_SOURCE_DEFAULT].name = "default";
    s_state.providers[TS_TEMP_SOURCE_DEFAULT].value = TS_TEMP_DEFAULT_VALUE;
    s_state.providers[TS_TEMP_SOURCE_DEFAULT].registered = true;
    s_state.providers[TS_TEMP_SOURCE_DEFAULT].active = true;
    
    s_state.current_temp = TS_TEMP_DEFAULT_VALUE;
    s_state.active_source = TS_TEMP_SOURCE_DEFAULT;
    s_state.manual_temp = TS_TEMP_DEFAULT_VALUE;
    s_state.preferred_source = TS_TEMP_SOURCE_DEFAULT;  // 默认自动选择
    
    // 从 NVS 加载首选温度源
    load_preferred_source_from_nvs();
    
    s_state.initialized = true;
    
    TS_LOGI(TAG, "Temperature source manager initialized (v%s), preferred: %s", 
            TS_TEMP_SOURCE_VERSION, 
            s_state.preferred_source == TS_TEMP_SOURCE_DEFAULT ? "auto" : 
            ts_temp_source_type_to_str(s_state.preferred_source));
    return ESP_OK;
}

esp_err_t ts_temp_source_deinit(void)
{
    if (!s_state.initialized) {
        return ESP_OK;
    }
    
    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }
    
    s_state.initialized = false;
    return ESP_OK;
}

bool ts_temp_source_is_initialized(void)
{
    return s_state.initialized;
}

/*===========================================================================*/
/*                          Public API - Provider                             */
/*===========================================================================*/

esp_err_t ts_temp_provider_register(ts_temp_source_type_t type, const char *name)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (type >= TS_TEMP_SOURCE_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    provider_t *p = &s_state.providers[type];
    p->type = type;
    p->name = name ? name : ts_temp_source_type_to_str(type);
    p->value = TS_TEMP_DEFAULT_VALUE;
    p->last_update_ms = 0;
    p->update_count = 0;
    p->registered = true;
    p->active = false;
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Provider registered: %s (%s)", p->name, ts_temp_source_type_to_str(type));
    return ESP_OK;
}

esp_err_t ts_temp_provider_unregister(ts_temp_source_type_t type)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (type >= TS_TEMP_SOURCE_MAX) return ESP_ERR_INVALID_ARG;
    if (type == TS_TEMP_SOURCE_DEFAULT) return ESP_ERR_NOT_SUPPORTED;  // 不能注销默认
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    s_state.providers[type].registered = false;
    s_state.providers[type].active = false;
    
    evaluate_active_source();
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Provider unregistered: %s", ts_temp_source_type_to_str(type));
    return ESP_OK;
}

esp_err_t ts_temp_provider_update(ts_temp_source_type_t type, int16_t temp_01c)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (type >= TS_TEMP_SOURCE_MAX) return ESP_ERR_INVALID_ARG;
    
    // 温度范围检查
    if (temp_01c < TS_TEMP_MIN_VALID || temp_01c > TS_TEMP_MAX_VALID) {
        TS_LOGW(TAG, "Invalid temp from %s: %d", ts_temp_source_type_to_str(type), temp_01c);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    provider_t *p = &s_state.providers[type];
    if (!p->registered) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    p->value = temp_01c;
    p->last_update_ms = get_current_ms();
    p->update_count++;
    p->active = true;
    
    evaluate_active_source();
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API - Consumer                             */
/*===========================================================================*/

static void fill_invalid_temp_data(ts_temp_data_t *data)
{
    if (!data) {
        return;
    }

    memset(data, 0, sizeof(*data));
    data->value = TS_TEMP_DEFAULT_VALUE;
    data->source = TS_TEMP_SOURCE_DEFAULT;
    data->guard_value = TS_TEMP_DEFAULT_VALUE;
}

static void fill_effective_data_locked(ts_temp_data_t *data)
{
    if (!data) {
        return;
    }

    int16_t temp = s_state.current_temp;
    ts_temp_source_type_t source = s_state.active_source;
    int64_t now = get_current_ms();

    data->value = temp;
    data->source = source;
    data->identity_revision = s_state.identity_revision;
    data->timestamp_ms = (source == TS_TEMP_SOURCE_VARIABLE)
                         ? s_state.variable_last_update_ms
                         : s_state.providers[source].last_update_ms;
    data->valid = is_cached_active_source_valid_locked(now);
    data->guard_value = s_state.variable_guard_valid
                        ? s_state.variable_guard_temp : temp;
    data->guard_valid = s_state.variable_guard_valid ? true : data->valid;
    data->partial_stale = s_state.variable_partial_stale;
    data->bound_valid_count = s_state.variable_valid_count;
    data->bound_total_count = s_state.variable_total_count;
    data->bound_valid_weight = s_state.variable_valid_weight;
    data->bound_total_weight = s_state.variable_total_weight;
}

static int16_t ts_temp_get_effective_impl(ts_temp_data_t *data, bool try_lock)
{
    if (!s_state.initialized) {
        fill_invalid_temp_data(data);
        return TS_TEMP_DEFAULT_VALUE;
    }
    
    if (xSemaphoreTake(s_state.mutex, try_lock ? 0 : portMAX_DELAY) != pdTRUE) {
        fill_invalid_temp_data(data);
        return TS_TEMP_DEFAULT_VALUE;
    }
    
    /* 重新评估活动源并更新温度（确保获取最新值） */
    evaluate_active_source_with_lock_mode(try_lock);
    
    int16_t temp = s_state.current_temp;
    fill_effective_data_locked(data);
    
    xSemaphoreGive(s_state.mutex);
    
    return temp;
}

int16_t ts_temp_get_effective(ts_temp_data_t *data)
{
    return ts_temp_get_effective_impl(data, false);
}

int16_t ts_temp_get_effective_nonblocking(ts_temp_data_t *data)
{
    return ts_temp_get_effective_impl(data, true);
}

esp_err_t ts_temp_get_by_source(ts_temp_source_type_t type, ts_temp_data_t *data)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (type >= TS_TEMP_SOURCE_MAX || !data) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (type == TS_TEMP_SOURCE_VARIABLE) {
        variable_temp_snapshot_t snapshot = {0};
        bool valid = read_variable_temp_snapshot(&snapshot, get_current_ms(), false);
        store_variable_snapshot(&snapshot);
        data->value = valid ? snapshot.temp : TS_TEMP_DEFAULT_VALUE;
        data->source = TS_TEMP_SOURCE_VARIABLE;
        data->identity_revision = s_state.identity_revision;
        data->timestamp_ms = snapshot.timestamp_ms;
        data->valid = valid;
        data->guard_value = snapshot.guard_temp;
        data->guard_valid = snapshot.guard_valid;
        data->partial_stale = snapshot.partial_stale;
        data->bound_valid_count = snapshot.valid_count;
        data->bound_total_count = snapshot.total_count;
        data->bound_valid_weight = snapshot.valid_weight;
        data->bound_total_weight = snapshot.total_weight;
        xSemaphoreGive(s_state.mutex);
        return (snapshot.total_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    
    provider_t *p = &s_state.providers[type];
    if (!p->registered) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    data->value = p->value;
    data->source = p->type;
    data->identity_revision = s_state.identity_revision;
    data->timestamp_ms = p->last_update_ms;
    data->valid = (type != TS_TEMP_SOURCE_DEFAULT) &&
                  is_provider_valid(type, get_current_ms(), false);
    data->guard_value = p->value;
    data->guard_valid = data->valid;
    data->partial_stale = false;
    data->bound_valid_count = 0;
    data->bound_total_count = 0;
    data->bound_valid_weight = 0.0f;
    data->bound_total_weight = 0.0f;
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API - Manual Mode                          */
/*===========================================================================*/

esp_err_t ts_temp_set_manual(int16_t temp_01c)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (temp_01c < TS_TEMP_MIN_VALID || temp_01c > TS_TEMP_MAX_VALID) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 注册手动 provider（如果尚未注册）
    if (!s_state.providers[TS_TEMP_SOURCE_MANUAL].registered) {
        ts_temp_provider_register(TS_TEMP_SOURCE_MANUAL, "manual");
    }
    
    // 启用手动模式
    s_state.manual_mode = true;
    s_state.manual_temp = temp_01c;
    
    // 更新 provider
    return ts_temp_provider_update(TS_TEMP_SOURCE_MANUAL, temp_01c);
}

esp_err_t ts_temp_set_manual_mode(bool enable)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    s_state.manual_mode = enable;
    
    if (enable && !s_state.providers[TS_TEMP_SOURCE_MANUAL].registered) {
        // 注册手动 provider
        provider_t *p = &s_state.providers[TS_TEMP_SOURCE_MANUAL];
        p->type = TS_TEMP_SOURCE_MANUAL;
        p->name = "manual";
        p->value = s_state.manual_temp;
        p->last_update_ms = get_current_ms();
        p->registered = true;
        p->active = true;
    } else if (enable) {
        provider_t *p = &s_state.providers[TS_TEMP_SOURCE_MANUAL];
        p->active = true;
        p->last_update_ms = get_current_ms();
    }
    
    evaluate_active_source();
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Manual mode %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

bool ts_temp_is_manual_mode(void)
{
    return s_state.manual_mode;
}

/*===========================================================================*/
/*                          Public API - Status                               */
/*===========================================================================*/

esp_err_t ts_temp_get_status(ts_temp_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    
    memset(status, 0, sizeof(ts_temp_status_t));
    
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    int64_t now = get_current_ms();
    status->initialized = s_state.initialized;
    status->active_source = s_state.active_source;
    status->preferred_source = s_state.preferred_source;
    status->current_temp = s_state.current_temp;
    if (s_state.active_source == TS_TEMP_SOURCE_VARIABLE) {
        status->current_timestamp_ms = s_state.variable_last_update_ms;
    } else if (s_state.active_source < TS_TEMP_SOURCE_MAX) {
        status->current_timestamp_ms = s_state.providers[s_state.active_source].last_update_ms;
    }
    status->current_valid = is_cached_active_source_valid_locked(now);
    status->manual_mode = s_state.manual_mode;
    status->variable_partial_stale = s_state.variable_partial_stale;
    status->variable_valid_count = s_state.variable_valid_count;
    status->variable_total_count = s_state.variable_total_count;
    status->variable_valid_weight = s_state.variable_valid_weight;
    status->variable_total_weight = s_state.variable_total_weight;
    
    /* 复制绑定变量信息 */
    if (s_state.bound_variable[0] != '\0') {
        strncpy(status->bound_variable, s_state.bound_variable, sizeof(status->bound_variable) - 1);
        status->bound_variable[sizeof(status->bound_variable) - 1] = '\0';
    }
    status->bound_var_count = s_state.bound_var_count;
    if (s_state.bound_var_count > 0) {
        memcpy(status->bound_vars, s_state.bound_vars,
               s_state.bound_var_count * sizeof(ts_temp_bound_var_t));
    }
    
    uint32_t count = 0;
    for (int i = 0; i < TS_TEMP_SOURCE_MAX; i++) {
        provider_t *p = &s_state.providers[i];
        if (p->registered) {
            ts_temp_provider_info_t *info = &status->providers[count];
            info->type = p->type;
            info->name = p->name;
            info->last_value = p->value;
            info->last_update_ms = p->last_update_ms;
            info->update_count = p->update_count;
            info->active = p->active;
            count++;
        }
    }
    status->provider_count = count;
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

ts_temp_source_type_t ts_temp_get_active_source(void)
{
    return s_state.active_source;
}

/*===========================================================================*/
/*                          Public API - Preferred Source                     */
/*===========================================================================*/

esp_err_t ts_temp_set_preferred_source(ts_temp_source_type_t type)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    // 不能选择 MANUAL（手动模式通过专用 API）或无效值
    if (type == TS_TEMP_SOURCE_MANUAL || type >= TS_TEMP_SOURCE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_temp_source_type_t old_preferred = s_state.preferred_source;
    s_state.preferred_source = type;
    if (type != old_preferred) s_state.identity_revision++;
    
    // 切换到非手动源时，自动禁用手动模式
    if (s_state.manual_mode) {
        s_state.manual_mode = false;
        TS_LOGI(TAG, "Manual mode disabled (switching to %s)", ts_temp_source_type_to_str(type));
    }
    
    // 重新评估活动源
    evaluate_active_source();
    
    xSemaphoreGive(s_state.mutex);
    
    // 保存到 NVS + SD 卡
    save_preferred_source_to_nvs(type);
    export_temp_config_to_sdcard();
    
    TS_LOGI(TAG, "Preferred source: %s -> %s",
            old_preferred == TS_TEMP_SOURCE_DEFAULT ? "auto" : ts_temp_source_type_to_str(old_preferred),
            type == TS_TEMP_SOURCE_DEFAULT ? "auto" : ts_temp_source_type_to_str(type));
    
    return ESP_OK;
}

ts_temp_source_type_t ts_temp_get_preferred_source(void)
{
    return s_state.preferred_source;
}

esp_err_t ts_temp_clear_preferred_source(void)
{
    return ts_temp_set_preferred_source(TS_TEMP_SOURCE_DEFAULT);
}

/*===========================================================================*/
/*                          NVS Functions                                     */
/*===========================================================================*/

/* Forward declarations */
static esp_err_t load_temp_config_from_file(const char *filepath);
static esp_err_t save_preferred_source_to_nvs(ts_temp_source_type_t type);
static esp_err_t save_bound_variable_to_nvs(const char *var_name);

/**
 * @brief 加载温度源配置
 * 
 * Priority: SD card file > NVS > defaults (遵循系统配置优先级原则)
 */
static esp_err_t load_preferred_source_from_nvs(void)
{
    esp_err_t ret;
    
    /* 1. 优先从 SD 卡加载 */
    if (ts_storage_sd_mounted()) {
        ret = load_temp_config_from_file("/sdcard/config/temp.json");
        if (ret == ESP_OK) {
            TS_LOGI(TAG, "Loaded temp config from SD card");
            return ESP_OK;  /* SD 卡配置已自动保存到 NVS */
        }
    }
    
    /* 2. SD 卡无配置，从 NVS 加载 */
    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (ret != ESP_OK) {
        TS_LOGD(TAG, "No saved temp config found, using defaults");
        return ESP_OK;
    }
    
    s_state.identity_revision++;
    uint8_t preferred = 0;
    ret = nvs_get_u8(handle, NVS_KEY_PREFERRED, &preferred);
    
    if (ret == ESP_OK && preferred < TS_TEMP_SOURCE_MAX) {
        s_state.preferred_source = (ts_temp_source_type_t)preferred;
        TS_LOGI(TAG, "Loaded preferred source from NVS: %s",
                preferred == 0 ? "auto" : ts_temp_source_type_to_str(s_state.preferred_source));
        
        /* 加载绑定变量：优先新 blob 格式，回退旧 string 格式 */
        {
            typedef struct {
                uint8_t count;
                ts_temp_bound_var_t vars[TS_TEMP_MAX_BOUND_VARS];
            } bound_vars_blob_t;
            
            bound_vars_blob_t blob = {0};
            size_t blob_len = sizeof(blob);
            ret = nvs_get_blob(handle, NVS_KEY_BOUND_VARS, &blob, &blob_len);
            if (ret == ESP_OK && blob.count > 0 && blob.count <= TS_TEMP_MAX_BOUND_VARS) {
                s_state.bound_var_count = blob.count;
                memcpy(s_state.bound_vars, blob.vars, blob.count * sizeof(ts_temp_bound_var_t));
                for (uint8_t i = 0; i < s_state.bound_var_count; i++) {
                    s_state.bound_vars[i].weight =
                        sanitize_bound_weight(s_state.bound_vars[i].weight);
                }
                if (!has_positive_bound_weight(s_state.bound_vars, s_state.bound_var_count)) {
                    TS_LOGW(TAG, "Ignoring persisted bound vars with no positive weights");
                    s_state.bound_var_count = 0;
                    memset(s_state.bound_vars, 0, sizeof(s_state.bound_vars));
                }
                sync_bound_variable_compat();
                TS_LOGI(TAG, "Loaded %d weighted bound vars from NVS", s_state.bound_var_count);
            } else {
                size_t len = sizeof(s_state.bound_variable);
                ret = nvs_get_str(handle, NVS_KEY_BOUND_VAR, s_state.bound_variable, &len);
                if (ret == ESP_OK && s_state.bound_variable[0] != '\0') {
                    s_state.bound_var_count = 1;
                    strncpy(s_state.bound_vars[0].name, s_state.bound_variable,
                            sizeof(s_state.bound_vars[0].name) - 1);
                    s_state.bound_vars[0].name[sizeof(s_state.bound_vars[0].name) - 1] = '\0';
                    s_state.bound_vars[0].weight = 1.0f;
                    TS_LOGI(TAG, "Loaded legacy bound variable from NVS: %s", s_state.bound_variable);
                } else {
                    s_state.bound_variable[0] = '\0';
                    s_state.bound_var_count = 0;
                }
            }
        }
    } else {
        TS_LOGD(TAG, "No saved temp config found, using defaults");
    }
    
    nvs_close(handle);
    return ESP_OK;
}

/**
 * @brief Load temp config from SD card JSON file
 * 
 * 支持 .tscfg 加密配置优先加载
 */
static esp_err_t load_temp_config_from_file(const char *filepath)
{
    if (!filepath) return ESP_ERR_INVALID_ARG;
    
    /* 使用 .tscfg 优先加载 */
    char *content = NULL;
    size_t content_len = 0;
    bool used_tscfg = false;
    
    esp_err_t ret = ts_config_pack_load_with_priority(
        filepath, &content, &content_len, &used_tscfg);
    
    if (ret != ESP_OK) {
        TS_LOGD(TAG, "Cannot open file: %s", filepath);
        return ret;
    }
    
    if (used_tscfg) {
        TS_LOGI(TAG, "Loaded encrypted config from .tscfg");
    }
    
    cJSON *root = cJSON_Parse(content);
    free(content);
    
    if (!root) {
        TS_LOGW(TAG, "Failed to parse JSON: %s", filepath);
        return ESP_ERR_INVALID_ARG;
    }
    
    s_state.identity_revision++;
    /* Parse preferred_source */
    cJSON *pref = cJSON_GetObjectItem(root, "preferred_source");
    if (pref && cJSON_IsString(pref)) {
        const char *pref_str = pref->valuestring;
        if (strcmp(pref_str, "variable") == 0) {
            s_state.preferred_source = TS_TEMP_SOURCE_VARIABLE;
        } else if (strcmp(pref_str, "agx") == 0 || strcmp(pref_str, "agx_auto") == 0) {
            s_state.preferred_source = TS_TEMP_SOURCE_AGX_AUTO;
        } else if (strcmp(pref_str, "local") == 0 || strcmp(pref_str, "sensor_local") == 0) {
            s_state.preferred_source = TS_TEMP_SOURCE_SENSOR_LOCAL;
        } else if (strcmp(pref_str, "manual") == 0) {
            s_state.preferred_source = TS_TEMP_SOURCE_MANUAL;
        } else {
            s_state.preferred_source = TS_TEMP_SOURCE_DEFAULT;
        }
    }
    
    /* Parse bound_variables (new array format) or bound_variable (legacy string) */
    cJSON *bound_arr = cJSON_GetObjectItem(root, "bound_variables");
    if (bound_arr && cJSON_IsArray(bound_arr)) {
        int arr_size = cJSON_GetArraySize(bound_arr);
        if (arr_size > TS_TEMP_MAX_BOUND_VARS) arr_size = TS_TEMP_MAX_BOUND_VARS;
        s_state.bound_var_count = 0;
        for (int i = 0; i < arr_size; i++) {
            cJSON *item = cJSON_GetArrayItem(bound_arr, i);
            cJSON *jname = cJSON_GetObjectItem(item, "name");
            cJSON *jweight = cJSON_GetObjectItem(item, "weight");
            if (jname && cJSON_IsString(jname) && jname->valuestring[0] != '\0') {
                strncpy(s_state.bound_vars[s_state.bound_var_count].name,
                        jname->valuestring,
                        sizeof(s_state.bound_vars[0].name) - 1);
                s_state.bound_vars[s_state.bound_var_count].name[sizeof(s_state.bound_vars[0].name) - 1] = '\0';
                s_state.bound_vars[s_state.bound_var_count].weight =
                    sanitize_bound_weight((jweight && cJSON_IsNumber(jweight))
                                          ? (float)jweight->valuedouble : 1.0f);
                s_state.bound_var_count++;
            }
        }
        if (!has_positive_bound_weight(s_state.bound_vars, s_state.bound_var_count)) {
            TS_LOGW(TAG, "Ignoring SD card bound vars with no positive weights");
            s_state.bound_var_count = 0;
            memset(s_state.bound_vars, 0, sizeof(s_state.bound_vars));
        }
        sync_bound_variable_compat();
    } else {
        cJSON *bound = cJSON_GetObjectItem(root, "bound_variable");
        if (bound && cJSON_IsString(bound) && bound->valuestring[0] != '\0') {
            s_state.bound_var_count = 1;
            strncpy(s_state.bound_vars[0].name, bound->valuestring,
                    sizeof(s_state.bound_vars[0].name) - 1);
            s_state.bound_vars[0].name[sizeof(s_state.bound_vars[0].name) - 1] = '\0';
            s_state.bound_vars[0].weight = 1.0f;
            sync_bound_variable_compat();
        }
    }
    
    cJSON_Delete(root);
    
    TS_LOGI(TAG, "Loaded temp config from SD card: preferred=%s, bound_vars=%d",
            ts_temp_source_type_to_str(s_state.preferred_source),
            s_state.bound_var_count);
    
    /* Save to NVS for next boot */
    save_preferred_source_to_nvs(s_state.preferred_source);
    if (s_state.bound_var_count > 0) {
        save_bound_vars_to_nvs();
    }
    
    return ESP_OK;
}

static esp_err_t save_preferred_source_to_nvs(ts_temp_source_type_t type)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_u8(handle, NVS_KEY_PREFERRED, (uint8_t)type);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to write preferred source: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        TS_LOGD(TAG, "Saved preferred source to NVS: %s",
                type == 0 ? "auto" : ts_temp_source_type_to_str(type));
    }
    
    return ret;
}

static esp_err_t save_bound_variable_to_nvs(const char *var_name)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (var_name && var_name[0] != '\0') {
        ret = nvs_set_str(handle, NVS_KEY_BOUND_VAR, var_name);
    } else {
        ret = nvs_erase_key(handle, NVS_KEY_BOUND_VAR);
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    }
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to write bound variable: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        TS_LOGD(TAG, "Saved bound variable to NVS: %s", var_name ? var_name : "(none)");
    }
    
    return ret;
}

static esp_err_t save_bound_vars_to_nvs(void)
{
    nvs_handle_t handle;
    typedef struct {
        uint8_t count;
        ts_temp_bound_var_t vars[TS_TEMP_MAX_BOUND_VARS];
    } bound_vars_blob_t;

    bound_vars_blob_t blob = {0};
    char bound_variable[TS_TEMP_MAX_VARNAME_LEN] = {0};

    if (s_state.mutex) {
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);
        blob.count = s_state.bound_var_count;
        if (blob.count > 0) {
            memcpy(blob.vars, s_state.bound_vars,
                   blob.count * sizeof(ts_temp_bound_var_t));
        }
        strncpy(bound_variable, s_state.bound_variable, sizeof(bound_variable) - 1);
        xSemaphoreGive(s_state.mutex);
    }

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(ret));
        return ret;
    }

    if (blob.count > 0) {
        ret = nvs_set_blob(handle, NVS_KEY_BOUND_VARS, &blob, sizeof(blob));
    } else {
        ret = nvs_erase_key(handle, NVS_KEY_BOUND_VARS);
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    }
    
    /* 同步旧 key 以保持向后兼容 */
    if (ret == ESP_OK) {
        esp_err_t legacy_ret;
        if (bound_variable[0] != '\0') {
            legacy_ret = nvs_set_str(handle, NVS_KEY_BOUND_VAR, bound_variable);
        } else {
            legacy_ret = nvs_erase_key(handle, NVS_KEY_BOUND_VAR);
            if (legacy_ret == ESP_ERR_NVS_NOT_FOUND) legacy_ret = ESP_OK;
        }
        if (legacy_ret != ESP_OK) {
            ret = legacy_ret;
        }
    }
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to write bound vars blob: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        TS_LOGD(TAG, "Saved %d bound vars to NVS", blob.count);
    } else {
        TS_LOGE(TAG, "Failed to commit bound vars: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 导出温度配置到 SD 卡
 * 
 * 生成包含 preferred_source、bound_variable（兼容）和 bound_variables（数组）的 JSON
 */
static void export_temp_config_to_sdcard(void)
{
    ts_temp_source_type_t preferred_source = TS_TEMP_SOURCE_DEFAULT;
    char bound_variable[TS_TEMP_MAX_VARNAME_LEN] = {0};
    ts_temp_bound_var_t bound_vars[TS_TEMP_MAX_BOUND_VARS] = {0};
    uint8_t bound_var_count = 0;

    if (s_state.mutex) {
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    }
    preferred_source = s_state.preferred_source;
    strncpy(bound_variable, s_state.bound_variable, sizeof(bound_variable) - 1);
    bound_var_count = s_state.bound_var_count;
    if (bound_var_count > 0) {
        memcpy(bound_vars, s_state.bound_vars,
               bound_var_count * sizeof(ts_temp_bound_var_t));
    }
    if (s_state.mutex) {
        xSemaphoreGive(s_state.mutex);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        TS_LOGW(TAG, "Failed to create JSON for SD card export");
        return;
    }
    
    const char *preferred_str = preferred_source == TS_TEMP_SOURCE_DEFAULT
                                ? "auto" 
                                : ts_temp_source_type_to_str(preferred_source);
    cJSON_AddStringToObject(root, "preferred_source", preferred_str);
    
    /* 向后兼容：导出第一个变量名 */
    if (bound_variable[0] != '\0') {
        cJSON_AddStringToObject(root, "bound_variable", bound_variable);
    }
    
    /* 导出加权变量数组 */
    if (bound_var_count > 0) {
        cJSON *arr = cJSON_CreateArray();
        if (arr) {
            for (uint8_t i = 0; i < bound_var_count; i++) {
                cJSON *item = cJSON_CreateObject();
                if (item) {
                    cJSON_AddStringToObject(item, "name", bound_vars[i].name);
                    cJSON_AddNumberToObject(item, "weight", bound_vars[i].weight);
                    cJSON_AddItemToArray(arr, item);
                }
            }
            cJSON_AddItemToObject(root, "bound_variables", arr);
        }
    }
    
    esp_err_t ret = ts_config_module_export_custom_json(TS_CONFIG_MODULE_TEMP, root);
    cJSON_Delete(root);
    
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_SD_NOT_MOUNTED) {
        TS_LOGW(TAG, "Failed to export temp config to SD card: %s", esp_err_to_name(ret));
    }
}

/*===========================================================================*/
/*                      Public API - Variable Binding                         */
/*===========================================================================*/

esp_err_t ts_temp_bind_variable(const char *var_name)
{
    if (!var_name || var_name[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(var_name) >= TS_TEMP_MAX_VARNAME_LEN) return ESP_ERR_INVALID_SIZE;
    
    ts_temp_bound_var_t single = {0};
    strncpy(single.name, var_name, sizeof(single.name) - 1);
    single.weight = 1.0f;
    return ts_temp_bind_variables(&single, 1);
}

esp_err_t ts_temp_bind_variables(const ts_temp_bound_var_t *vars, uint8_t count)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (!vars || count == 0 || count > TS_TEMP_MAX_BOUND_VARS) return ESP_ERR_INVALID_ARG;

    ts_temp_bound_var_t validated_vars[TS_TEMP_MAX_BOUND_VARS] = {0};
    double total_weight = 0.0;

    for (uint8_t i = 0; i < count; i++) {
        if (vars[i].name[0] == '\0') return ESP_ERR_INVALID_ARG;
        if (strlen(vars[i].name) >= TS_TEMP_MAX_VARNAME_LEN) return ESP_ERR_INVALID_SIZE;
        if (!ts_variable_exists(vars[i].name)) {
            TS_LOGW(TAG, "Variable does not exist: %s (will bind anyway)", vars[i].name);
        }

        validated_vars[i] = vars[i];
        validated_vars[i].weight = sanitize_bound_weight(vars[i].weight);
        total_weight += validated_vars[i].weight;
    }

    if (total_weight <= 0.001) {
        TS_LOGW(TAG, "Rejected weighted binding with non-positive total weight");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    bool changed = s_state.bound_var_count != count;
    for (uint8_t i = 0; !changed && i < count; i++) {
        changed = strcmp(s_state.bound_vars[i].name, validated_vars[i].name) != 0 ||
                  s_state.bound_vars[i].weight != validated_vars[i].weight;
    }
    if (changed) s_state.identity_revision++;
    s_state.bound_var_count = count;
    memcpy(s_state.bound_vars, validated_vars, count * sizeof(ts_temp_bound_var_t));
    sync_bound_variable_compat();

    evaluate_active_source();

    xSemaphoreGive(s_state.mutex);

    save_bound_vars_to_nvs();
    export_temp_config_to_sdcard();

    TS_LOGI(TAG, "Temperature bound to %d weighted variable(s)", count);

    return ESP_OK;
}

esp_err_t ts_temp_get_bound_variable(char *var_name, size_t len)
{
    if (!var_name || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    if (s_state.bound_var_count == 0) {
        xSemaphoreGive(s_state.mutex);
        var_name[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    
    strncpy(var_name, s_state.bound_variable, len - 1);
    var_name[len - 1] = '\0';
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_temp_get_bound_variables(ts_temp_bound_var_t *vars, uint8_t *count)
{
    if (!vars || !count) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    if (s_state.bound_var_count == 0) {
        xSemaphoreGive(s_state.mutex);
        *count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    *count = s_state.bound_var_count;
    memcpy(vars, s_state.bound_vars, s_state.bound_var_count * sizeof(ts_temp_bound_var_t));
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_temp_unbind_variable(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    bool was_bound = (s_state.bound_var_count > 0);
    if (was_bound) s_state.identity_revision++;
    s_state.bound_var_count = 0;
    memset(s_state.bound_vars, 0, sizeof(s_state.bound_vars));
    s_state.bound_variable[0] = '\0';
    
    evaluate_active_source();
    
    xSemaphoreGive(s_state.mutex);
    
    if (was_bound) {
        save_bound_vars_to_nvs();
        save_bound_variable_to_nvs(NULL);
        export_temp_config_to_sdcard();
        TS_LOGI(TAG, "Temperature variable binding removed");
    }
    
    return ESP_OK;
}
