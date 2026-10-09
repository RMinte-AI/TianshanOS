#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ts_fan.h"
#include "ts_temp_source.h"

typedef void *ts_pwm_handle_t;
typedef void *ts_gpio_handle_t;
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define portTICK_PERIOD_MS 1
#define CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS 1000
#include "fan_types.inc"
static fan_instance_t s_fans[TS_FAN_MAX];
static bool s_auto_temp_enabled = true;
static int64_t now_ms;
static ts_temp_data_t input;
static uint8_t observed_demand;
static float pwm_output;
static esp_err_t hal_result;
static unsigned hal_calls;

static int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static uint32_t xTaskGetTickCount(void) { return (uint32_t)now_ms; }
int16_t ts_temp_get_effective(ts_temp_data_t *data) { *data = input; return data->value; }
int16_t ts_temp_get_effective_nonblocking(ts_temp_data_t *data) { return ts_temp_get_effective(data); }
static esp_err_t ts_pwm_set_duty(ts_pwm_handle_t pwm, float duty) {
    ++hal_calls;
    if (hal_result == ESP_OK) pwm_output = duty;
    return hal_result;
}
static esp_err_t update_pwm(fan_instance_t *fan, uint8_t duty);
#include "fan_core.inc"

static void setup(int current, bool invert) {
    memset(s_fans, 0, sizeof(s_fans));
    now_ms = 100000;
    hal_result = ESP_OK;
    hal_calls = 0;
    fan_instance_t *f = &s_fans[0];
    f->initialized = f->enabled = true;
    f->mode = TS_FAN_MODE_AUTO;
    f->pwm = (void *)1;
    f->current_duty = current;
    f->temperature = 490;
    f->config.min_duty = 10;
    f->config.max_duty = 100;
    f->config.hysteresis = 20;
    f->config.min_interval = 5000;
    f->config.invert_pwm = invert;
    const ts_fan_curve_point_t curve[] = {
        {300,0}, {400,10}, {500,15}, {550,25}, {600,35},
        {650,40}, {700,45}, {800,50}, {900,60}
    };
    f->config.curve_points = 9;
    memcpy(f->config.curve, curve, sizeof(curve));
    reset_adaptive_auto_state(f);
    pwm_output = invert ? 100 - current : current;
    input = (ts_temp_data_t){.value = 490, .guard_value = 490,
        .source = TS_TEMP_SOURCE_VARIABLE, .valid = true, .guard_valid = true,
        .bound_total_count = 1, .bound_valid_count = 1, .timestamp_ms = now_ms};
    observed_demand = current;
}

static void tick(const char *scenario, int second, int temperature) {
    fan_instance_t *f = &s_fans[0];
    now_ms = (second + 100) * 1000LL;
    input.value = input.guard_value = temperature;
    input.timestamp_ms = now_ms; /* Each fixture row is an explicitly supplied fresh report. */
    uint8_t previous = f->current_duty;
    fan_update_callback(NULL);
    ts_fan_status_t status;
    assert(ts_fan_get_status(0, &status) == ESP_OK);
    assert(status.duty_percent == f->current_duty);
    if (hal_result == ESP_OK) {
        assert(pwm_output == (f->config.invert_pwm ? 100 - status.duty_percent : status.duty_percent));
        assert(status.target_duty == status.duty_percent || f->mode != TS_FAN_MODE_AUTO);
    } else {
        assert(f->current_duty == previous);
    }
    if (f->mode == TS_FAN_MODE_AUTO && !f->guard_active && !f->temp_stale) {
        assert(f->target_duty <= f->config.max_duty);
        assert(f->target_duty == 0 || f->target_duty >= f->config.min_duty);
        assert(f->target_duty >= observed_demand || f->target_duty > previous);
        if (f->slope_c_per_min > 0 && !f->temp_stale) assert(f->target_duty >= previous);
    }
    printf("%s,%d,%.1f,%.1f,%.3f,%u,%u,%u,%.0f,%d,%d,%d,%.3f\n", scenario, second,
        temperature / 10.0, f->predicted_temperature / 10.0, f->slope_c_per_min,
        observed_demand, status.target_duty, status.duty_percent, pwm_output,
        hal_result, status.guard_active, status.temp_stale, f->controller_gain);
}

static void seeded_cooling(const char *scenario, int current, int temperature, int delta_30s) {
    setup(current, false);
    fan_instance_t *f = &s_fans[0];
    for (int s = -30; s <= -1; s++) record_auto_temp_sample(f, temperature + delta_30s * (-s) / 30, (s + 100) * 1000LL);
    f->last_auto_update_ms = 0;
    for (int s = 0; s <= 260; s++) tick(scenario, s, temperature);
}

/* Offline test plant only. These parameters are assumptions, not identified RM-01 values.
 * Hotspot and aluminum body are separate nodes; successful HAL PWM drives airflow. */
static void plant(const char *name, double core_capacity, double bulk_capacity,
                  double core_conductance, int pattern) {
    setup(14, false);
    double bulk = 30.0 + 55.0 / (1.5 + 0.18 * 14);
    double core = bulk + 55.0 / core_conductance;
    for (int s = 0; s <= 600; s++) {
        double power = 55.0;
        if (pattern == 0 && s >= 40 && s < 48) power = 320;
        if (pattern == 1 && s >= 40 && s < 280 && (s - 40) % 60 < 8) power = 320;
        if (pattern == 2 && s >= 40) power = 300;
        int noise = pattern == 3 ? (s % 7 == 0 ? 1 : s % 7 == 3 ? -1 : 0) : 0;
        tick(name, s, (int)round(core * 10) + noise);
        for (int k = 0; k < 10; k++) {
            double transfer = core_conductance * (core - bulk);
            double ambient_loss = (1.5 + 0.18 * s_fans[0].current_duty) * (bulk - 30.0);
            core += 0.1 * (power - transfer) / core_capacity;
            bulk += 0.1 * (transfer - ambient_loss) / bulk_capacity;
        }
    }
}

int main(void) {
    puts("scenario,second,temp_c,predicted_c,slope,demand_pct,request_pct,applied_pct,hal_pwm_pct,hal_result,guard,stale,gain");
    /* Analytical controller-state reconstruction, NOT recovered hidden video samples. */
    seeded_cooling("video_like_87_to_14", 87, 488, 70);
    seeded_cooling("video_like_82_to_14", 82, 488, 70);
    seeded_cooling("video_like_69_to_13", 69, 466, 12);
    setup(14, false);
    for (int s = 0; s <= 200; s++) {
        int t = s < 31 ? 490 : s <= 35 ? 490 + (s - 30) * 35 : s <= 50 ? 665 - (s - 35) * 11 : 490;
        tick("short_load", s, t);
    }
    setup(14, false);
    for (int s = 0; s <= 280; s++) {
        int n = (s - 31) % 60;
        int t = s < 31 || s > 185 ? 490 : n < 5 ? 490 + (n + 1) * 35 : n < 21 ? 665 - (n - 4) * 10 : 490;
        tick("repeated_short_load", s, t);
    }
    setup(14, true);
    for (int s = 0; s <= 220; s++) tick("sustained_warming", s, s < 31 ? 490 : 490 + (s - 30) * 2);
    setup(87, false);
    for (int s = 0; s <= 160; s++) tick("cooling_then_reheat", s, s < 80 ? 488 : 488 + (s - 79) * 4);
    setup(14, false);
    for (int s = 0; s <= 90; s++) {
        tick("hard_guard", s, s < 31 ? 950 : s == 45 ? 910 : 900);
        if (s <= 75) assert(s_fans[0].guard_active && s_fans[0].current_duty == 100);
        if (s == 76) assert(!s_fans[0].guard_active);
    }
    setup(87, false);
    for (int s = 0; s <= 80; s++) {
        input.valid = s < 20 || s >= 30;
        tick("stale_and_recovery", s, 488);
        if (s >= 20 && s < 30) assert(s_fans[0].current_duty == 25);
    }
    setup(87, true);
    for (int s = 0; s <= 100; s++) {
        hal_result = s >= 15 && s < 20 ? ESP_ERR_INVALID_STATE : ESP_OK;
        tick("failed_pwm", s, 488);
        if (s == 19) assert(s_fans[0].target_duty <= s_fans[0].current_duty);
    }
    setup(87, false);
    for (int s = 0; s <= 100; s++) {
        if (s == 20) assert(ts_fan_set_mode(0, TS_FAN_MODE_CURVE) == ESP_OK);
        if (s == 30) assert(ts_fan_set_mode(0, TS_FAN_MODE_AUTO) == ESP_OK);
        if (s == 60) assert(ts_fan_set_mode(0, TS_FAN_MODE_MANUAL) == ESP_OK);
        if (s == 70) assert(ts_fan_set_mode(0, TS_FAN_MODE_AUTO) == ESP_OK);
        if (s == 80) assert(ts_fan_set_mode(0, TS_FAN_MODE_OFF) == ESP_OK);
        tick("mode_switch", s, 488);
        if (s == 30 || s == 70) assert(s_fans[0].temp_history_count <= 2);
        if (s >= 80) assert(s_fans[0].current_duty == 0);
    }
    /* Small surplus retains the old slow tail; no undershoot/hidden credit on a rise. */
    setup(20, false);
    for (int s = 0; s <= 40; s++) tick("small_surplus", s, 490);
    setup(10, false);
    s_fans[0].config.max_duty = 45;
    for (int s = 0; s <= 80; s++) tick("limited_output", s, s < 30 ? 490 : 850);
    setup(14, false);
    input.partial_stale = true;
    tick("partial_stale", 0, 490);
    assert(s_fans[0].current_duty == 25);
    setup(87, false);
    s_fans[0].last_auto_update_ms = 100000;
    tick("delayed_tick", 200, 488);
    assert(s_fans[0].current_duty >= 14 && s_fans[0].current_duty < 87);
    const double capacities[][3] = {{12,1200,10}, {8,600,8}, {24,1800,16}};
    for (int p = 0; p < 3; p++) {
        for (int pattern = 0; pattern < 4; pattern++) {
            char name[40];
            snprintf(name, sizeof(name), "plant_%d_%s", p,
                pattern == 0 ? "short" : pattern == 1 ? "repeated" : pattern == 2 ? "sustained" : "noise");
            plant(name, capacities[p][0], capacities[p][1], capacities[p][2], pattern);
        }
    }
    return 0;
}
