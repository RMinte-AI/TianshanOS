/**
 * @file ts_fan.c
 * @brief Fan Control Driver Implementation
 * 
 * 移植自 robOS fan_controller，增强功能：
 * - 温度曲线线性插值
 * - 温度迟滞控制（防止频繁调速）
 * - 完整配置持久化
 * - 自动订阅温度事件（ts_temp_source 集成）
 */

#include "ts_fan.h"
#include "ts_temp_source.h"
#include "ts_event.h"
#include "ts_hal_pwm.h"
#include "ts_hal_gpio.h"
#include "ts_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define TAG "ts_fan"
#define FAN_NVS_NAMESPACE "fan_config"
#define FAN_CONFIG_VERSION 2
#define FAN_AUTO_GUARD_TEMP            950     /* 95.0°C */
#define FAN_AUTO_GUARD_RELEASE_TEMP    900     /* 90.0°C */
#define FAN_AUTO_GUARD_RELEASE_MS      30000
#define FAN_AUTO_PREDICT_WINDOW_SEC    45.0f
#define FAN_AUTO_SLOPE_WINDOW_MS       30000
#define FAN_AUTO_MIN_SAMPLE_MS         10000
#define FAN_AUTO_RESPONSE_WINDOW_MS    30000
#define FAN_AUTO_RESPONSE_DUTY_DELTA   5
#define FAN_AUTO_GAIN_DEFAULT          0.75f
#define FAN_AUTO_GAIN_MIN              0.50f
#define FAN_AUTO_GAIN_MAX              2.00f
#define FAN_AUTO_RISE_RATE_PER_SEC     12.0f
#define FAN_AUTO_FALL_RATE_PER_SEC     0.30f
#define FAN_AUTO_STALE_DUTY            25
#define FAN_AUTO_HISTORY_SIZE          32

/*===========================================================================*/
/*                          Internal Types                                    */
/*===========================================================================*/

typedef struct {
    int16_t temp;
    int64_t timestamp_ms;
    bool valid;
} fan_temp_sample_t;

typedef struct {
    bool initialized;
    bool enabled;
    ts_fan_config_t config;
    ts_pwm_handle_t pwm;
    ts_gpio_handle_t tach_gpio;
    ts_fan_mode_t mode;
    uint8_t current_duty;       /* Last successful logical PWM output. */
    bool duty_valid;
    uint8_t target_duty;
    uint16_t rpm;
    int16_t temperature;
    int16_t last_stable_temp;
    uint32_t last_speed_change_time;
    volatile uint32_t tach_count;
    int64_t last_tach_time;
    bool fault;
    int16_t control_temperature;
    int16_t guard_temperature;
    int16_t predicted_temperature;
    float slope_c_per_min;
    float controller_gain;
    float cooling_response;
    ts_fan_auto_state_t auto_state;
    bool guard_active;
    bool temp_stale;
    int64_t guard_release_since_ms;
    int64_t guard_last_observation_ms;
    int64_t guard_last_report_ms;
    ts_temp_source_type_t guard_last_source;
    uint32_t guard_last_identity;
    bool response_observing;
    int64_t response_until_ms;
    float response_slope_before;
    fan_temp_sample_t temp_history[FAN_AUTO_HISTORY_SIZE];
    uint8_t temp_history_next;
    uint8_t temp_history_count;
    int64_t last_auto_update_ms;
    float auto_fall_credit;
} fan_instance_t;

/*===========================================================================*/
/*                          Static Variables                                  */
/*===========================================================================*/

static fan_instance_t s_fans[TS_FAN_MAX];
static esp_timer_handle_t s_update_timer = NULL;
static bool s_initialized = false;
static ts_event_handler_handle_t s_temp_event_handle = NULL;
static bool s_auto_temp_enabled = true;

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static void tach_isr_callback(ts_gpio_handle_t handle, void *arg);
static void fan_update_callback(void *arg);
static uint8_t calc_duty_from_curve(fan_instance_t *fan, int16_t temp);
static esp_err_t apply_curve_with_hysteresis(fan_instance_t *fan, uint8_t fan_id);
static esp_err_t apply_adaptive_auto(fan_instance_t *fan, const ts_temp_data_t *temp_data);
static esp_err_t update_pwm(fan_instance_t *fan, uint8_t duty);
static void temp_event_handler(const ts_event_t *event, void *user_data);

/*===========================================================================*/
/*                          Temperature Event Handler                         */
/*===========================================================================*/

/**
 * @brief 温度事件处理器
 * 
 * 自动接收 ts_temp_source 发布的温度更新事件，
 * 并同步到所有风扇实例
 */
static void temp_event_handler(const ts_event_t *event, void *user_data)
{
    (void)user_data;
    
    if (!s_auto_temp_enabled || !s_initialized) return;
    if (event == NULL || event->id != TS_EVT_TEMP_UPDATED) return;
    
    const ts_temp_event_data_t *temp_evt = (const ts_temp_event_data_t *)event->data;
    if (temp_evt == NULL) return;
    if (temp_evt->source == TS_TEMP_SOURCE_DEFAULT ||
        temp_evt->temp < TS_TEMP_MIN_VALID ||
        temp_evt->temp > TS_TEMP_MAX_VALID) {
        return;
    }
    
    /* AUTO 模式只由 fan_update_callback() 推进，事件仅服务旧的 CURVE 快照路径 */
    for (int i = 0; i < TS_FAN_MAX; i++) {
        if (s_fans[i].initialized && s_fans[i].mode == TS_FAN_MODE_CURVE) {
            s_fans[i].temperature = temp_evt->temp;
            TS_LOGD(TAG, "Fan %d temp updated: %d.%d°C (source=%d)", 
                    i, temp_evt->temp / 10, 
                    abs(temp_evt->temp % 10),
                    temp_evt->source);
        }
    }
}

/*===========================================================================*/
/*                          ISR Callback                                      */
/*===========================================================================*/

static void tach_isr_callback(ts_gpio_handle_t handle, void *arg)
{
    fan_instance_t *fan = (fan_instance_t *)arg;
    fan->tach_count++;
}

/*===========================================================================*/
/*                          Curve Calculation                                 */
/*===========================================================================*/

/**
 * @brief 线性插值计算曲线占空比
 */
static uint8_t calc_duty_from_curve(fan_instance_t *fan, int16_t temp)
{
    if (fan->config.curve_points == 0) {
        return fan->config.max_duty;
    }
    
    const ts_fan_curve_point_t *curve = fan->config.curve;
    uint8_t points = fan->config.curve_points;
    
    // 低于最低点
    if (temp <= curve[0].temp) {
        return curve[0].duty;
    }
    
    // 高于最高点
    if (temp >= curve[points - 1].temp) {
        return curve[points - 1].duty;
    }
    
    // 线性插值
    for (int i = 0; i < points - 1; i++) {
        if (temp >= curve[i].temp && temp < curve[i + 1].temp) {
            int32_t t_range = curve[i + 1].temp - curve[i].temp;
            int32_t d_range = (int32_t)curve[i + 1].duty - (int32_t)curve[i].duty;
            int32_t t_offset = temp - curve[i].temp;
            return curve[i].duty + (uint8_t)((t_offset * d_range) / t_range);
        }
    }
    
    return fan->config.max_duty;
}

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static uint8_t apply_duty_limits(fan_instance_t *fan, uint8_t target)
{
    if (target < fan->config.min_duty && target > 0) {
        target = fan->config.min_duty;
    }
    if (target > fan->config.max_duty) {
        target = fan->config.max_duty;
    }
    return target;
}

static bool is_temp_data_basic_valid(const ts_temp_data_t *data)
{
    if (!data ||
        !data->valid ||
        data->source == TS_TEMP_SOURCE_DEFAULT ||
        data->value < TS_TEMP_MIN_VALID ||
        data->value > TS_TEMP_MAX_VALID) {
        return false;
    }

    return true;
}

static bool is_auto_temp_data_usable(const ts_temp_data_t *data)
{
    if (!is_temp_data_basic_valid(data)) {
        return false;
    }

    if (data->bound_total_count > 0 &&
        (data->partial_stale || data->bound_valid_count < data->bound_total_count)) {
        return false;
    }

    return true;
}

static void reset_adaptive_auto_state(fan_instance_t *fan)
{
    fan->target_duty = fan->current_duty;
    fan->control_temperature = fan->temperature;
    fan->guard_temperature = fan->temperature;
    fan->predicted_temperature = fan->temperature;
    fan->slope_c_per_min = 0.0f;
    fan->controller_gain = FAN_AUTO_GAIN_DEFAULT;
    fan->cooling_response = 0.0f;
    fan->auto_state = TS_FAN_AUTO_STATE_IDLE;
    fan->guard_active = false;
    fan->temp_stale = false;
    fan->guard_release_since_ms = 0;
    fan->guard_last_observation_ms = 0;
    fan->guard_last_report_ms = 0;
    fan->response_observing = false;
    fan->response_until_ms = 0;
    fan->response_slope_before = 0.0f;
    memset(fan->temp_history, 0, sizeof(fan->temp_history));
    fan->temp_history_next = 0;
    fan->temp_history_count = 0;
    fan->last_auto_update_ms = 0;
    fan->auto_fall_credit = 0.0f;
}

static bool get_adaptive_temperatures(const ts_temp_data_t *temp_data,
                                      int16_t *control_temp,
                                      int16_t *guard_temp)
{
    if (!control_temp || !guard_temp || !is_auto_temp_data_usable(temp_data)) {
        return false;
    }

    *control_temp = temp_data->value;
    if (temp_data->guard_valid &&
        temp_data->guard_value >= TS_TEMP_MIN_VALID &&
        temp_data->guard_value <= TS_TEMP_MAX_VALID) {
        *guard_temp = temp_data->guard_value;
    } else {
        *guard_temp = temp_data->value;
    }

    return true;
}

static void record_auto_temp_sample(fan_instance_t *fan, int16_t temp, int64_t now_ms)
{
    fan->temp_history[fan->temp_history_next].temp = temp;
    fan->temp_history[fan->temp_history_next].timestamp_ms = now_ms;
    fan->temp_history[fan->temp_history_next].valid = true;
    fan->temp_history_next = (fan->temp_history_next + 1) % FAN_AUTO_HISTORY_SIZE;
    if (fan->temp_history_count < FAN_AUTO_HISTORY_SIZE) {
        fan->temp_history_count++;
    }
}

static bool calculate_auto_slope(fan_instance_t *fan, int16_t current_temp,
                                 int64_t now_ms, uint32_t min_span_ms,
                                 float *slope_c_per_min)
{
    const fan_temp_sample_t *oldest = NULL;
    int64_t oldest_age = 0;

    if (!fan || !slope_c_per_min || fan->temp_history_count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < fan->temp_history_count; i++) {
        const fan_temp_sample_t *sample = &fan->temp_history[i];
        if (!sample->valid) {
            continue;
        }
        int64_t age = now_ms - sample->timestamp_ms;
        if (age < 0 || age > FAN_AUTO_SLOPE_WINDOW_MS) {
            continue;
        }
        if (!oldest || age > oldest_age) {
            oldest = sample;
            oldest_age = age;
        }
    }

    if (!oldest || oldest_age < min_span_ms) {
        return false;
    }

    float delta_c = (current_temp - oldest->temp) / 10.0f;
    *slope_c_per_min = delta_c * 60000.0f / (float)oldest_age;
    return true;
}

static uint8_t apply_auto_rate_limit(fan_instance_t *fan, uint8_t target,
                                     bool allow_down, int64_t now_ms)
{
    uint8_t current = fan->current_duty;
    float dt_sec = 1.0f;
    if (fan->last_auto_update_ms > 0) {
        dt_sec = (now_ms - fan->last_auto_update_ms) / 1000.0f;
    }
    if (dt_sec <= 0.0f) {
        dt_sec = 1.0f;
    }

    if (target > current) {
        fan->auto_fall_credit = 0.0f;
        uint8_t max_step = (uint8_t)(FAN_AUTO_RISE_RATE_PER_SEC * dt_sec + 0.5f);
        if (max_step < 1) max_step = 1;
        if ((uint16_t)target > (uint16_t)current + max_step) {
            target = current + max_step;
        }
        if (current == 0 && target > 0 &&
            target < fan->config.min_duty) {
            target = fan->config.min_duty;
        }
    } else if (target < current) {
        if (!allow_down) {
            fan->auto_fall_credit = 0.0f;
            target = current;
        } else {
            /* 已不需要的额外风量按温度趋势窗口平滑退出，避免峰值留下数分钟尾巴。
             * 接近当前需求时仍使用原来的慢速退档；升档和退档许可保持不变。 */
            float excess_rate = (current - target) / (FAN_AUTO_SLOPE_WINDOW_MS / 1000.0f);
            float fall_rate = excess_rate > FAN_AUTO_FALL_RATE_PER_SEC
                            ? excess_rate : FAN_AUTO_FALL_RATE_PER_SEC;
            fan->auto_fall_credit += fall_rate * dt_sec;
            uint8_t desired_step = current - target;
            uint8_t max_step = fan->auto_fall_credit >= desired_step
                             ? desired_step : (uint8_t)fan->auto_fall_credit;
            if (max_step == 0) {
                target = current;
            } else {
                target = current - max_step;
                /* Permission belongs to this outstanding demand, not a later one. */
                fan->auto_fall_credit = max_step == desired_step
                                      ? 0.0f : fan->auto_fall_credit - max_step;
            }
        }
    } else {
        fan->auto_fall_credit = 0.0f;
    }

    fan->last_auto_update_ms = now_ms;
    return target;
}

static void update_response_learning(fan_instance_t *fan, bool slope_valid,
                                     float slope, int64_t now_ms)
{
    if (!fan->response_observing || now_ms < fan->response_until_ms) {
        return;
    }

    fan->response_observing = false;
    if (!slope_valid) {
        return;
    }

    fan->cooling_response = fan->response_slope_before - slope;
    if (fan->cooling_response < 0.2f) {
        fan->controller_gain *= 1.15f;
    } else if (fan->cooling_response > 1.5f || slope < -0.5f) {
        fan->controller_gain *= 0.95f;
    }
    fan->controller_gain = clamp_float(fan->controller_gain,
                                       FAN_AUTO_GAIN_MIN,
                                       FAN_AUTO_GAIN_MAX);
}

static void update_guard_observation(fan_instance_t *fan, const ts_temp_data_t *data,
                                     int64_t now_ms)
{
    /* Valid cached readings count; a missed evaluation or changed source does not.
     * Two timer periods allow normal scheduling jitter, capped by input validity. */
    int64_t max_gap_ms = 2LL * CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS;
    if (max_gap_ms > TS_TEMP_DATA_TIMEOUT_MS) max_gap_ms = TS_TEMP_DATA_TIMEOUT_MS;
    bool broken = fan->guard_last_observation_ms == 0 ||
                  now_ms <= fan->guard_last_observation_ms ||
                  now_ms - fan->guard_last_observation_ms > max_gap_ms ||
                  data->source != fan->guard_last_source ||
                  data->identity_revision != fan->guard_last_identity ||
                  data->timestamp_ms < fan->guard_last_report_ms ||
                  data->timestamp_ms - fan->guard_last_report_ms > TS_TEMP_DATA_TIMEOUT_MS ||
                  data->timestamp_ms <= 0 || data->timestamp_ms > now_ms;
    if (broken) fan->guard_release_since_ms = 0;
    fan->guard_last_observation_ms = now_ms;
    fan->guard_last_report_ms = data->timestamp_ms;
    fan->guard_last_source = data->source;
    fan->guard_last_identity = data->identity_revision;
}

static esp_err_t apply_adaptive_auto(fan_instance_t *fan, const ts_temp_data_t *temp_data)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int16_t control_temp = TS_TEMP_DEFAULT_VALUE;
    int16_t guard_temp = TS_TEMP_DEFAULT_VALUE;
    uint8_t old_duty = fan->current_duty;
    bool recovered_from_stale = fan->temp_stale;

    if (!get_adaptive_temperatures(temp_data, &control_temp, &guard_temp)) {
        fan->temp_stale = true;
        fan->auto_state = TS_FAN_AUTO_STATE_STALE;
        fan->response_observing = false;
        fan->auto_fall_credit = 0.0f;
        fan->guard_active = false;
        fan->guard_release_since_ms = 0;
        fan->guard_last_observation_ms = 0;
        fan->target_duty = FAN_AUTO_STALE_DUTY;
        fan->predicted_temperature = fan->control_temperature;
        fan->last_auto_update_ms = now_ms;
        return update_pwm(fan, FAN_AUTO_STALE_DUTY);
    }

    fan->temp_stale = false;
    fan->temperature = control_temp;
    fan->control_temperature = control_temp;
    fan->guard_temperature = guard_temp;
    record_auto_temp_sample(fan, control_temp, now_ms);

    update_guard_observation(fan, temp_data, now_ms);
    if (guard_temp >= FAN_AUTO_GUARD_TEMP) {
        fan->guard_active = true;
        fan->guard_release_since_ms = 0;
    } else if (fan->guard_active) {
        if (guard_temp <= FAN_AUTO_GUARD_RELEASE_TEMP) {
            if (fan->guard_release_since_ms == 0) {
                fan->guard_release_since_ms = now_ms;
            } else if ((now_ms - fan->guard_release_since_ms) >= FAN_AUTO_GUARD_RELEASE_MS) {
                fan->guard_active = false;
                fan->guard_release_since_ms = 0;
            }
        } else {
            fan->guard_release_since_ms = 0;
        }
    }

    bool slope_valid = calculate_auto_slope(fan, control_temp, now_ms,
                                            FAN_AUTO_MIN_SAMPLE_MS,
                                            &fan->slope_c_per_min);
    if (!slope_valid) {
        fan->slope_c_per_min = 0.0f;
    }
    update_response_learning(fan, slope_valid, fan->slope_c_per_min, now_ms);

    if (fan->guard_active) {
        fan->auto_state = TS_FAN_AUTO_STATE_GUARD;
        fan->response_observing = false;
        fan->auto_fall_credit = 0.0f;
        fan->target_duty = 100;
        fan->predicted_temperature = guard_temp;
        fan->last_auto_update_ms = now_ms;
        return update_pwm(fan, 100);
    }

    uint8_t baseline = apply_duty_limits(fan, calc_duty_from_curve(fan, control_temp));
    uint8_t target = baseline;

    if (slope_valid) {
        float predicted = control_temp + (fan->slope_c_per_min * (FAN_AUTO_PREDICT_WINDOW_SEC / 60.0f) * 10.0f);
        if (predicted < TS_TEMP_MIN_VALID) predicted = TS_TEMP_MIN_VALID;
        if (predicted > TS_TEMP_MAX_VALID) predicted = TS_TEMP_MAX_VALID;
        fan->predicted_temperature = (int16_t)predicted;

        if (fan->predicted_temperature >= FAN_AUTO_GUARD_TEMP) {
            fan->auto_state = TS_FAN_AUTO_STATE_GUARD;
            target = fan->config.max_duty;
        } else {
            float boost = 0.0f;
            float margin_c = (FAN_AUTO_GUARD_TEMP - guard_temp) / 10.0f;
            if (margin_c < 25.0f) {
                boost += (25.0f - margin_c) * 1.2f;
            }
            if (fan->predicted_temperature > 850) {
                boost += ((fan->predicted_temperature - 850) / 10.0f) * 1.5f;
            }
            if (fan->slope_c_per_min > 0.0f) {
                boost += fan->slope_c_per_min * 2.0f;
            }

            float adaptive_target = baseline + boost * fan->controller_gain;
            if (adaptive_target > 100.0f) adaptive_target = 100.0f;
            target = apply_duty_limits(fan, (uint8_t)(adaptive_target + 0.5f));
            fan->auto_state = TS_FAN_AUTO_STATE_ACTIVE;
        }
    } else {
        fan->predicted_temperature = control_temp;
        fan->auto_state = TS_FAN_AUTO_STATE_BASELINE;
    }

    bool allow_down = (guard_temp <= FAN_AUTO_GUARD_RELEASE_TEMP) &&
                      (!slope_valid || fan->slope_c_per_min <= 0.0f);
    if (target < fan->current_duty && !allow_down && !recovered_from_stale) {
        target = fan->current_duty;
    }

    if (recovered_from_stale && target < fan->current_duty) {
        fan->auto_fall_credit = 0.0f;
        fan->last_auto_update_ms = now_ms;
    } else {
        target = apply_auto_rate_limit(fan, target, allow_down, now_ms);
    }
    fan->target_duty = target;
    esp_err_t ret = update_pwm(fan, target);

    if (ret == ESP_OK && !fan->response_observing && slope_valid &&
        target >= old_duty + FAN_AUTO_RESPONSE_DUTY_DELTA) {
        fan->response_observing = true;
        fan->response_until_ms = now_ms + FAN_AUTO_RESPONSE_WINDOW_MS;
        fan->response_slope_before = fan->slope_c_per_min;
    }

    return ret;
}

static esp_err_t apply_auto_immediate(fan_instance_t *fan)
{
    ts_temp_data_t temp_data = {0};
    const ts_temp_data_t *auto_temp = NULL;

    if (s_auto_temp_enabled) {
        ts_temp_get_effective(&temp_data);
        if (is_temp_data_basic_valid(&temp_data)) {
            fan->temperature = temp_data.value;
        }
        auto_temp = &temp_data;
    }

    reset_adaptive_auto_state(fan);
    return apply_adaptive_auto(fan, auto_temp);
}

/**
 * @brief 应用曲线（带迟滞控制）
 */
static esp_err_t apply_curve_with_hysteresis(fan_instance_t *fan, uint8_t fan_id)
{
    (void)fan_id;

    if (fan->config.curve_points == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 计算目标占空比
    uint8_t target = calc_duty_from_curve(fan, fan->temperature);
    
    // 应用最小/最大限制
    target = apply_duty_limits(fan, target);
    
    fan->target_duty = target;
    
    // 温度迟滞控制
    int16_t temp_diff = fan->temperature > fan->last_stable_temp ?
                        fan->temperature - fan->last_stable_temp :
                        fan->last_stable_temp - fan->temperature;
    
    bool significant_change = temp_diff >= fan->config.hysteresis;
    bool enough_time = (current_time - fan->last_speed_change_time) >= fan->config.min_interval;
    
    // 决定是否调速
    if (significant_change && enough_time) {
        fan->last_stable_temp = fan->temperature;
        fan->last_speed_change_time = current_time;
        
        if (target != fan->current_duty) {
            return update_pwm(fan, target);
        }
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          PWM Control                                       */
/*===========================================================================*/

static esp_err_t update_pwm(fan_instance_t *fan, uint8_t duty)
{
    if (!fan->pwm) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!fan->enabled) {
        duty = 0;
    }
    
    if (duty > 100) duty = 100;
    
    // PWM 反转
    uint8_t actual = fan->config.invert_pwm ? (100 - duty) : duty;
    
    esp_err_t ret = ts_pwm_set_duty(fan->pwm, (float)actual);
    if (ret == ESP_OK) {
        fan->current_duty = duty;
        fan->duty_valid = true;
        fan->fault = false;
    } else {
        fan->fault = true;
    }
    
    return ret;
}

/*===========================================================================*/
/*                          Timer Callback                                    */
/*===========================================================================*/

static void fan_update_callback(void *arg)
{
    int64_t now = esp_timer_get_time();
    static uint32_t s_log_counter = 0;
    ts_temp_data_t temp_data = {0};
    bool have_temp_data = false;
    bool basic_temp_data_valid = false;
    
    /* 温度监控独立于风扇模式，手动/关闭模式下也保持有效温度快照更新。 */
    if (s_auto_temp_enabled) {
        int16_t current_temp = ts_temp_get_effective_nonblocking(&temp_data);
        have_temp_data = true;
        basic_temp_data_valid = is_temp_data_basic_valid(&temp_data);
        
        /* 每 10 秒输出一次调试日志 */
        if (++s_log_counter >= 10) {
            s_log_counter = 0;
            TS_LOGD(TAG, "[FAN-CB] temp=%d.%d°C, source=%d, valid=%d",
                    current_temp / 10, current_temp % 10,
                    temp_data.source, temp_data.valid);
        }
        
        if (basic_temp_data_valid) {
            for (int i = 0; i < TS_FAN_MAX; i++) {
                if (s_fans[i].initialized) {
                    s_fans[i].temperature = current_temp;
                }
            }
        }
    }
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        fan_instance_t *fan = &s_fans[i];
        if (!fan->initialized) continue;
        
        // 计算 RPM
        if (fan->tach_gpio) {
            int64_t dt_us = now - fan->last_tach_time;
            if (dt_us > 0) {
                fan->rpm = (fan->tach_count * 60 * 1000000) / (dt_us * 2);
                fan->tach_count = 0;
                fan->last_tach_time = now;
            }
        }
        
        // 根据模式更新
        if (!fan->enabled) {
            update_pwm(fan, 0);
            continue;
        }
        
        switch (fan->mode) {
        case TS_FAN_MODE_MANUAL:
            // 手动模式：保持当前设定
            update_pwm(fan, fan->target_duty);
            break;
            
        case TS_FAN_MODE_AUTO:
            {
                const ts_temp_data_t *auto_temp = have_temp_data ? &temp_data : NULL;
                apply_adaptive_auto(fan, auto_temp);
            }
            break;
            
        case TS_FAN_MODE_CURVE:
            // 曲线模式：带迟滞控制
            apply_curve_with_hysteresis(fan, i);
            break;
            
        case TS_FAN_MODE_OFF:
        default:
            update_pwm(fan, 0);
            break;
        }
    }
}

/*===========================================================================*/
/*                          Public API - Init                                 */
/*===========================================================================*/

esp_err_t ts_fan_init(void)
{
    if (s_initialized) return ESP_OK;
    
    memset(s_fans, 0, sizeof(s_fans));
    
    // 默认曲线（30°C-20%, 50°C-40%, 70°C-80%, 80°C-100%）
    ts_fan_curve_point_t default_curve[] = {
        {300, 20}, {500, 40}, {700, 80}, {800, 100}
    };
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        s_fans[i].config.min_duty = 20;
        s_fans[i].config.max_duty = 100;
        s_fans[i].config.hysteresis = TS_FAN_DEFAULT_HYSTERESIS;
        s_fans[i].config.min_interval = TS_FAN_DEFAULT_MIN_INTERVAL;
        s_fans[i].config.invert_pwm = false;
        memcpy(s_fans[i].config.curve, default_curve, sizeof(default_curve));
        s_fans[i].config.curve_points = 4;
        s_fans[i].last_stable_temp = 250;  // 25.0°C
        s_fans[i].temperature = 250;
        s_fans[i].enabled = true;
        reset_adaptive_auto_state(&s_fans[i]);
    }
    
    // 启动更新定时器
    esp_timer_create_args_t timer_args = {
        .callback = fan_update_callback,
        .name = "fan_update"
    };
    esp_timer_create(&timer_args, &s_update_timer);
    
#ifdef CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS
    esp_timer_start_periodic(s_update_timer, CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS * 1000);
#else
    esp_timer_start_periodic(s_update_timer, 1000000);  // 1 秒
#endif
    
    /* 订阅温度更新事件 */
    esp_err_t ret = ts_event_register(
        TS_EVENT_BASE_TEMP,
        TS_EVT_TEMP_UPDATED,
        temp_event_handler,
        NULL,
        &s_temp_event_handle
    );
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Subscribed to temperature events");
    } else {
        TS_LOGW(TAG, "Failed to subscribe temp events: %s", esp_err_to_name(ret));
    }
    
    s_initialized = true;
    TS_LOGI(TAG, "Fan driver initialized");
    
    return ESP_OK;
}

esp_err_t ts_fan_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
    /* 注销温度事件订阅 */
    if (s_temp_event_handle != NULL) {
        ts_event_unregister(s_temp_event_handle);
        s_temp_event_handle = NULL;
    }
    
    if (s_update_timer) {
        esp_timer_stop(s_update_timer);
        esp_timer_delete(s_update_timer);
        s_update_timer = NULL;
    }
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        if (s_fans[i].pwm) {
            ts_pwm_destroy(s_fans[i].pwm);
            s_fans[i].pwm = NULL;
        }
        if (s_fans[i].tach_gpio) {
            ts_gpio_destroy(s_fans[i].tach_gpio);
            s_fans[i].tach_gpio = NULL;
        }
    }
    
    s_initialized = false;
    return ESP_OK;
}

bool ts_fan_is_initialized(void)
{
    return s_initialized;
}

/*===========================================================================*/
/*                          Public API - Configuration                        */
/*===========================================================================*/

void ts_fan_get_default_config(ts_fan_config_t *config)
{
    if (!config) return;
    
    memset(config, 0, sizeof(ts_fan_config_t));
    config->gpio_pwm = -1;
    config->gpio_tach = -1;
    config->min_duty = 20;
    config->max_duty = 100;
    config->hysteresis = TS_FAN_DEFAULT_HYSTERESIS;
    config->min_interval = TS_FAN_DEFAULT_MIN_INTERVAL;
    config->invert_pwm = false;
    
    // 默认曲线
    ts_fan_curve_point_t default_curve[] = {
        {300, 20}, {500, 40}, {700, 80}, {800, 100}
    };
    memcpy(config->curve, default_curve, sizeof(default_curve));
    config->curve_points = 4;
}

esp_err_t ts_fan_get_config(ts_fan_id_t fan, ts_fan_config_t *config)
{
    if (fan >= TS_FAN_MAX || !config) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    *config = s_fans[fan].config;
    return ESP_OK;
}

esp_err_t ts_fan_configure(ts_fan_id_t fan, const ts_fan_config_t *config)
{
    if (fan >= TS_FAN_MAX || !config) return ESP_ERR_INVALID_ARG;
    
    fan_instance_t *f = &s_fans[fan];
    f->config = *config;
    f->duty_valid = false;
    
    // 创建 PWM 句柄
    f->pwm = ts_pwm_create_raw(config->gpio_pwm, "fan");
    if (!f->pwm) {
        TS_LOGE(TAG, "Failed to create PWM for fan %d", fan);
        return ESP_FAIL;
    }
    
    // 配置 PWM (25kHz PC 风扇标准)
    ts_pwm_config_t pwm_cfg = {
        .frequency = 25000,
        .resolution_bits = 10,
        .timer = TS_PWM_TIMER_AUTO,
        .invert = config->invert_pwm,
        .initial_duty = 0.0f
    };
    esp_err_t ret = ts_pwm_configure(f->pwm, &pwm_cfg);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to configure PWM for fan %d", fan);
        ts_pwm_destroy(f->pwm);
        f->pwm = NULL;
        return ret;
    }
    
    // 配置转速计（如果有）
    if (config->gpio_tach >= 0) {
        f->tach_gpio = ts_gpio_create_raw(config->gpio_tach, "fan_tach");
        if (f->tach_gpio) {
            ts_gpio_config_t gpio_cfg = {
                .direction = TS_GPIO_DIR_INPUT,
                .pull_mode = TS_GPIO_PULL_UP,
                .intr_type = TS_GPIO_INTR_NEGEDGE,
                .drive = TS_GPIO_DRIVE_2,
                .invert = false,
                .initial_level = -1
            };
            ts_gpio_configure(f->tach_gpio, &gpio_cfg);
            ts_gpio_set_isr_callback(f->tach_gpio, tach_isr_callback, f);
            ts_gpio_intr_enable(f->tach_gpio);
            f->last_tach_time = esp_timer_get_time();
        }
    }
    
    f->initialized = true;
    f->enabled = true;
    f->mode = TS_FAN_MODE_MANUAL;
    f->current_duty = 0;
    f->fault = false;
    reset_adaptive_auto_state(f);
    f->target_duty = config->min_duty;
    ret = update_pwm(f, f->target_duty);
    
    TS_LOGI(TAG, "Fan %d configured: PWM=GPIO%d, TACH=%d, curve=%d points", 
            fan, config->gpio_pwm, config->gpio_tach, config->curve_points);
    
    return ret;
}

/*===========================================================================*/
/*                          Public API - Control                              */
/*===========================================================================*/

esp_err_t ts_fan_set_mode(ts_fan_id_t fan, ts_fan_mode_t mode)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    ts_fan_mode_t old_mode = s_fans[fan].mode;
    s_fans[fan].mode = mode;
    esp_err_t ret = ESP_OK;
    
    if (mode == TS_FAN_MODE_MANUAL && old_mode != mode) {
        s_fans[fan].target_duty = s_fans[fan].current_duty;
    }
    if (mode == TS_FAN_MODE_OFF) {
        s_fans[fan].target_duty = 0;
        ret = update_pwm(&s_fans[fan], 0);
    }
    
    /* 切换到曲线/自动模式时，重置迟滞状态以允许立即生效 */
    if ((mode == TS_FAN_MODE_CURVE || mode == TS_FAN_MODE_AUTO) && old_mode != mode) {
        /* 将 last_stable_temp 设为一个远离实际温度的值，确保首次评估通过迟滞检查 */
        s_fans[fan].last_stable_temp = -1000;   /* -100.0°C，远低于任何实际温度 */
        s_fans[fan].last_speed_change_time = 0;  /* 允许立即调速 */
        
        if (mode == TS_FAN_MODE_AUTO) {
            ret = apply_auto_immediate(&s_fans[fan]);
        } else if (s_auto_temp_enabled) {
            ts_temp_data_t temp_data;
            int16_t current_temp = ts_temp_get_effective(&temp_data);
            if (is_temp_data_basic_valid(&temp_data)) {
                s_fans[fan].temperature = current_temp;
            }
        }
        
        TS_LOGI(TAG, "Fan %d mode -> %d: hysteresis reset, temp=%d", 
                fan, mode, s_fans[fan].temperature);
    } else {
        TS_LOGI(TAG, "Fan %d mode set to %d", fan, mode);
    }
    
    return ret;
}

esp_err_t ts_fan_get_mode(ts_fan_id_t fan, ts_fan_mode_t *mode)
{
    if (fan >= TS_FAN_MAX || !mode) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    *mode = s_fans[fan].mode;
    return ESP_OK;
}

esp_err_t ts_fan_set_duty(ts_fan_id_t fan, uint8_t duty_percent)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    if (duty_percent > 100) duty_percent = 100;
    
    s_fans[fan].mode = TS_FAN_MODE_MANUAL;
    reset_adaptive_auto_state(&s_fans[fan]);
    s_fans[fan].target_duty = duty_percent;
    
    return update_pwm(&s_fans[fan], duty_percent);
}

esp_err_t ts_fan_enable(ts_fan_id_t fan, bool enable)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    s_fans[fan].enabled = enable;
    
    if (!enable) {
        return update_pwm(&s_fans[fan], 0);
    }
    
    TS_LOGI(TAG, "Fan %d %s", fan, enable ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t ts_fan_is_enabled(ts_fan_id_t fan, bool *enabled)
{
    if (fan >= TS_FAN_MAX || !enabled) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    *enabled = s_fans[fan].enabled;
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API - Temperature & Curve                  */
/*===========================================================================*/

esp_err_t ts_fan_set_temperature(ts_fan_id_t fan, int16_t temp_01c)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    if (temp_01c < TS_TEMP_MIN_VALID || temp_01c > TS_TEMP_MAX_VALID) {
        return ESP_ERR_INVALID_ARG;
    }
    s_fans[fan].temperature = temp_01c;
    return ESP_OK;
}

esp_err_t ts_fan_set_curve(ts_fan_id_t fan, const ts_fan_curve_point_t *curve, uint8_t points)
{
    if (fan >= TS_FAN_MAX || !curve || points > TS_FAN_MAX_CURVE_POINTS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 复制曲线点
    memcpy(s_fans[fan].config.curve, curve, points * sizeof(ts_fan_curve_point_t));
    s_fans[fan].config.curve_points = points;
    
    // 按温度排序（冒泡）
    for (int i = 0; i < points - 1; i++) {
        for (int j = 0; j < points - i - 1; j++) {
            if (s_fans[fan].config.curve[j].temp > s_fans[fan].config.curve[j + 1].temp) {
                ts_fan_curve_point_t tmp = s_fans[fan].config.curve[j];
                s_fans[fan].config.curve[j] = s_fans[fan].config.curve[j + 1];
                s_fans[fan].config.curve[j + 1] = tmp;
            }
        }
    }
    
    TS_LOGI(TAG, "Fan %d curve set with %d points", fan, points);
    return ESP_OK;
}

esp_err_t ts_fan_set_hysteresis(ts_fan_id_t fan, int16_t hysteresis, uint32_t min_interval)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    
    if (hysteresis < 0 || hysteresis > 200) {  // 0-20°C
        return ESP_ERR_INVALID_ARG;
    }
    if (min_interval < 100 || min_interval > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_fans[fan].config.hysteresis = hysteresis;
    s_fans[fan].config.min_interval = min_interval;
    
    TS_LOGI(TAG, "Fan %d hysteresis: %.1f°C, interval: %lums", 
            fan, hysteresis / 10.0f, (unsigned long)min_interval);
    
    return ESP_OK;
}

esp_err_t ts_fan_set_limits(ts_fan_id_t fan, uint8_t min_duty, uint8_t max_duty)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    
    if (min_duty > 100 || max_duty > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (min_duty > max_duty) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_fans[fan].config.min_duty = min_duty;
    s_fans[fan].config.max_duty = max_duty;
    
    TS_LOGI(TAG, "Fan %d limits: min=%d%%, max=%d%%", fan, min_duty, max_duty);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API - Status                               */
/*===========================================================================*/

esp_err_t ts_fan_get_status(ts_fan_id_t fan, ts_fan_status_t *status)
{
    if (fan >= TS_FAN_MAX || !status) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    fan_instance_t *f = &s_fans[fan];

    /* Status reads never mutate the requested setting or the last output. */
    status->mode = f->mode;
    status->duty_percent = f->current_duty;
    status->duty_valid = f->duty_valid;
    status->target_duty = f->target_duty;
    status->rpm = f->rpm;
    status->temp = f->temperature;
    status->last_stable_temp = f->last_stable_temp;
    status->is_running = f->duty_valid && f->current_duty > 0;
    status->enabled = f->enabled;
    status->fault = f->fault;
    status->control_temp = f->control_temperature;
    status->guard_temp = f->guard_temperature;
    status->predicted_temp = f->predicted_temperature;
    status->slope_c_per_min = f->slope_c_per_min;
    status->controller_gain = f->controller_gain;
    status->cooling_response = f->cooling_response;
    status->auto_state = f->auto_state;
    status->guard_active = f->guard_active;
    status->temp_stale = f->temp_stale;
    
    return ESP_OK;
}

esp_err_t ts_fan_get_all_status(ts_fan_status_t *status_array, uint8_t array_size)
{
    if (!status_array || array_size < TS_FAN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        ts_fan_get_status(i, &status_array[i]);
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API - Emergency                            */
/*===========================================================================*/

esp_err_t ts_fan_emergency_full(void)
{
    TS_LOGW(TAG, "Emergency: All fans to 100%%");
    
    esp_err_t result = ESP_OK;
    for (int i = 0; i < TS_FAN_MAX; i++) {
        if (s_fans[i].initialized) {
            s_fans[i].mode = TS_FAN_MODE_MANUAL;
            s_fans[i].enabled = true;
            reset_adaptive_auto_state(&s_fans[i]);
            s_fans[i].target_duty = 100;
            esp_err_t ret = update_pwm(&s_fans[i], 100);
            if (ret != ESP_OK && result == ESP_OK) result = ret;
        }
    }
    
    return result;
}

/*===========================================================================*/
/*                          Public API - Persistence                          */
/*===========================================================================*/

/**
 * @brief NVS 配置结构（包含完整运行时状态）
 */
typedef struct {
    uint32_t version;
    ts_fan_mode_t mode;
    uint8_t duty;
    bool enabled;
    uint8_t curve_points;
    ts_fan_curve_point_t curve[TS_FAN_MAX_CURVE_POINTS];
    int16_t hysteresis;
    uint32_t min_interval;
    uint8_t min_duty;
    uint8_t max_duty;
    bool invert_pwm;
} fan_nvs_config_t;

esp_err_t ts_fan_save_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(FAN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        if (!s_fans[i].initialized) continue;
        
        char key[16];
        snprintf(key, sizeof(key), "fan%d", i);
        
        fan_nvs_config_t cfg = {
            .version = FAN_CONFIG_VERSION,
            .mode = s_fans[i].mode,
            .duty = s_fans[i].mode == TS_FAN_MODE_MANUAL
                    ? s_fans[i].target_duty : s_fans[i].current_duty,
            .enabled = s_fans[i].enabled,
            .curve_points = s_fans[i].config.curve_points,
            .hysteresis = s_fans[i].config.hysteresis,
            .min_interval = s_fans[i].config.min_interval,
            .min_duty = s_fans[i].config.min_duty,
            .max_duty = s_fans[i].config.max_duty,
            .invert_pwm = s_fans[i].config.invert_pwm,
        };
        memcpy(cfg.curve, s_fans[i].config.curve, 
               s_fans[i].config.curve_points * sizeof(ts_fan_curve_point_t));
        
        ret = nvs_set_blob(nvs, key, &cfg, sizeof(cfg));
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to save fan %d: %s", i, esp_err_to_name(ret));
        } else {
            TS_LOGI(TAG, "Saved fan %d: mode=%d, duty=%d%%, curve=%d pts", 
                    i, cfg.mode, cfg.duty, cfg.curve_points);
        }
    }
    
    ret = nvs_commit(nvs);
    nvs_close(nvs);
    
    return ret;
}

esp_err_t ts_fan_save_full_config(ts_fan_id_t fan)
{
    if (fan >= TS_FAN_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_fans[fan].initialized) return ESP_ERR_INVALID_STATE;
    
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(FAN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    
    char key[16];
    snprintf(key, sizeof(key), "fan%d", fan);
    
    fan_nvs_config_t cfg = {
        .version = FAN_CONFIG_VERSION,
        .mode = s_fans[fan].mode,
        .duty = s_fans[fan].mode == TS_FAN_MODE_MANUAL
                    ? s_fans[fan].target_duty : s_fans[fan].current_duty,
        .enabled = s_fans[fan].enabled,
        .curve_points = s_fans[fan].config.curve_points,
        .hysteresis = s_fans[fan].config.hysteresis,
        .min_interval = s_fans[fan].config.min_interval,
        .min_duty = s_fans[fan].config.min_duty,
        .max_duty = s_fans[fan].config.max_duty,
        .invert_pwm = s_fans[fan].config.invert_pwm,
    };
    memcpy(cfg.curve, s_fans[fan].config.curve, 
           s_fans[fan].config.curve_points * sizeof(ts_fan_curve_point_t));
    
    ret = nvs_set_blob(nvs, key, &cfg, sizeof(cfg));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    
    nvs_close(nvs);
    return ret;
}

esp_err_t ts_fan_load_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(FAN_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        TS_LOGD(TAG, "No saved fan config found");
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t result = ESP_OK;
    for (int i = 0; i < TS_FAN_MAX; i++) {
        if (!s_fans[i].initialized) continue;
        
        char key[16];
        snprintf(key, sizeof(key), "fan%d", i);
        
        fan_nvs_config_t cfg;
        size_t len = sizeof(cfg);
        
        if (nvs_get_blob(nvs, key, &cfg, &len) != ESP_OK) {
            continue;
        }
        
        // 版本检查
        if (cfg.version != FAN_CONFIG_VERSION) {
            TS_LOGW(TAG, "Fan %d config version mismatch, skipping", i);
            continue;
        }
        
        // 恢复配置
        s_fans[i].mode = cfg.mode;
        reset_adaptive_auto_state(&s_fans[i]);
        s_fans[i].target_duty = cfg.mode == TS_FAN_MODE_OFF ? 0 : cfg.duty;
        s_fans[i].enabled = cfg.enabled;
        s_fans[i].config.curve_points = cfg.curve_points;
        s_fans[i].config.hysteresis = cfg.hysteresis;
        s_fans[i].config.min_interval = cfg.min_interval;
        s_fans[i].config.min_duty = cfg.min_duty;
        s_fans[i].config.max_duty = cfg.max_duty;
        s_fans[i].config.invert_pwm = cfg.invert_pwm;
        memcpy(s_fans[i].config.curve, cfg.curve, 
               cfg.curve_points * sizeof(ts_fan_curve_point_t));
        
        if (s_fans[i].mode == TS_FAN_MODE_AUTO && s_fans[i].pwm) {
            ret = apply_auto_immediate(&s_fans[i]);
        } else if (s_fans[i].mode == TS_FAN_MODE_MANUAL && s_fans[i].pwm) {
            ret = update_pwm(&s_fans[i], cfg.duty);
        } else if (s_fans[i].mode == TS_FAN_MODE_OFF && s_fans[i].pwm) {
            ret = update_pwm(&s_fans[i], 0);
        } else {
            ret = ESP_OK;
        }
        if (ret != ESP_OK && result == ESP_OK) result = ret;
        
        TS_LOGI(TAG, "Restored fan %d: mode=%d, duty=%d%%, curve=%d pts", 
                i, cfg.mode, cfg.duty, cfg.curve_points);
    }
    
    nvs_close(nvs);
    return result;
}
/*===========================================================================*/
/*                          Temperature Source Integration                    */
/*===========================================================================*/

esp_err_t ts_fan_set_auto_temp_enabled(bool enable)
{
    s_auto_temp_enabled = enable;
    TS_LOGI(TAG, "Auto temperature subscription %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

bool ts_fan_is_auto_temp_enabled(void)
{
    return s_auto_temp_enabled;
}
