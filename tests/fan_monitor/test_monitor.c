#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_fan.h"
#include "ts_temp_source.h"

typedef void *ts_pwm_handle_t;
typedef void *ts_gpio_handle_t;
typedef int SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY (-1)
#define portTICK_PERIOD_MS 1
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_AUTO_VAL_FLOAT 1
#define TS_AUTO_VAL_INT 2

#include "fan_types.inc"
#include "temp_types.inc"
static fan_instance_t s_fans[TS_FAN_MAX];
static temp_source_state_t s_state;
static bool s_auto_temp_enabled = true;
static int64_t now_ms;
static bool inside_timer;
static bool temp_mutex_busy;
static float pwm_output;
typedef struct {
    int64_t last_update_ms;
    struct { int type; double float_val; int int_val; } value;
} ts_variable_info_t;
static ts_variable_info_t variable;

static int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static uint32_t xTaskGetTickCount(void) { return (uint32_t)now_ms; }
static int xSemaphoreTake(int mutex, int ticks) {
    if (inside_timer) assert(ticks == 0);
    return !temp_mutex_busy;
}
static void xSemaphoreGive(int mutex) {}
static esp_err_t ts_variable_get_info(const char *name, ts_variable_info_t *info) {
    assert(!inside_timer); /* Timer must use the nonblocking variant. */
    *info = variable;
    return ESP_OK;
}
static esp_err_t ts_variable_get_info_try(const char *name, ts_variable_info_t *info) {
    *info = variable;
    return ESP_OK;
}
static void publish_temp_event(int16_t temp, ts_temp_source_type_t source,
                               int16_t previous, ts_temp_source_type_t previous_source) {}
static esp_err_t ts_pwm_set_duty(ts_pwm_handle_t pwm, float duty) {
    pwm_output = duty;
    return ESP_OK;
}
static esp_err_t update_pwm(fan_instance_t *fan, uint8_t duty);
#include "temp_core.inc"
#include "fan_core.inc"

static void setup(ts_fan_mode_t mode) {
    memset(&s_state, 0, sizeof(s_state));
    memset(s_fans, 0, sizeof(s_fans));
    s_state.initialized = true;
    s_state.mutex = 1;
    s_state.bound_var_count = 1;
    strcpy(s_state.bound_vars[0].name, "cpu.temperature");
    s_state.bound_vars[0].weight = 1.0f;
    s_auto_temp_enabled = true;
    temp_mutex_busy = false;
    now_ms = 1000;
    variable = (ts_variable_info_t){
        .last_update_ms = now_ms,
        .value = {.type = TS_AUTO_VAL_FLOAT, .float_val = 51.2}
    };
    fan_instance_t *f = &s_fans[0];
    f->initialized = true;
    f->enabled = true;
    f->pwm = (void *)1;
    f->mode = mode;
    f->current_duty = 37;
    f->config.min_duty = 20;
    f->config.max_duty = 100;
    f->config.hysteresis = 30;
    f->config.min_interval = 1000;
    f->config.curve_points = 4;
    ts_fan_curve_point_t points[] = {{300,20}, {500,40}, {700,80}, {800,100}};
    memcpy(f->config.curve, points, sizeof(points));
    reset_adaptive_auto_state(f);
    f->last_stable_temp = -1000;
    /* Simulate the last valid evaluation before changing fan mode. */
    ts_temp_data_t data;
    ts_temp_get_effective_nonblocking(&data);
    assert(data.valid && data.value == 512);
}

static void tick(int64_t ms, double temperature, bool report) {
    now_ms = ms;
    if (report) {
        variable.last_update_ms = now_ms;
        variable.value.float_val = temperature;
    }
    inside_timer = true;
    fan_update_callback(NULL);
    inside_timer = false;
}

static void assert_temperature(int temp, bool valid) {
    ts_temp_status_t global;
    ts_fan_status_t fan;
    assert(ts_temp_get_status(&global) == ESP_OK);
    assert(global.current_temp == temp);
    assert(global.current_valid == valid);
    if (valid) {
        assert(global.current_timestamp_ms == variable.last_update_ms);
        assert(ts_fan_get_status(0, &fan) == ESP_OK);
        assert(fan.temp == temp);
    }
}

int main(void) {
    setup(TS_FAN_MODE_MANUAL);
    for (int second = 2; second <= 25; second++) {
        tick(second * 1000LL, 49.5, true);
        assert_temperature(495, true);
        assert(s_fans[0].mode == TS_FAN_MODE_MANUAL);
        assert(s_fans[0].current_duty == 37 && pwm_output == 37);
        assert(s_fans[0].auto_state == TS_FAN_AUTO_STATE_IDLE);
        assert(!s_fans[0].response_observing);
    }
    puts("PASS manual: 51.2 -> 49.5C remains fresh beyond 10s; manual 37% unchanged, no AUTO learning");

    /* Newest sample ages naturally; timer/status queries must not renew its timestamp. */
    tick(36000, 49.5, false);
    assert_temperature(TS_TEMP_DEFAULT_VALUE, false);
    assert(s_fans[0].current_duty == 37);
    tick(37000, 48.7, true);
    assert_temperature(487, true);
    assert(s_fans[0].current_duty == 37);
    puts("PASS actual stale input: still invalid after 10s, recovers on new report; manual speed unchanged");

    setup(TS_FAN_MODE_OFF);
    for (int second = 2; second <= 25; second++) {
        tick(second * 1000LL, 49.5, true);
        assert_temperature(495, true);
        assert(s_fans[0].current_duty == 0 && pwm_output == 0);
    }
    puts("PASS OFF: temperature monitoring stays fresh; output remains zero");

    setup(TS_FAN_MODE_MANUAL);
    s_fans[0].enabled = false;
    tick(2000, 49.5, true);
    assert_temperature(495, true);
    assert(s_fans[0].current_duty == 0 && pwm_output == 0);
    puts("PASS disabled fan: temperature monitoring stays fresh; output remains zero");

    setup(TS_FAN_MODE_MANUAL);
    temp_mutex_busy = true;
    tick(2000, 49.5, true);
    assert(s_fans[0].current_duty == 37);
    temp_mutex_busy = false;
    tick(3000, 49.5, true);
    assert_temperature(495, true);
    puts("PASS busy temperature service: nonblocking timer, manual output preserved, refresh resumes");

    setup(TS_FAN_MODE_AUTO);
    for (int second = 2; second <= 25; second++) {
        tick(second * 1000LL, 60.0, true);
        assert_temperature(600, true);
    }
    assert(s_fans[0].auto_state == TS_FAN_AUTO_STATE_ACTIVE);
    assert(s_fans[0].current_duty == 60);
    tick(36000, 60.0, false);
    assert(s_fans[0].current_duty == 25 && s_fans[0].temp_stale);
    puts("PASS AUTO: control progresses once per timer; real stale input keeps approved 25% fallback");

    setup(TS_FAN_MODE_CURVE);
    tick(2000, 60.0, true);
    assert_temperature(600, true);
    assert(s_fans[0].current_duty == 60);
    puts("PASS CURVE: current temperature still drives configured curve");

    setup(TS_FAN_MODE_MANUAL);
    s_auto_temp_enabled = false;
    tick(2000, 49.5, true);
    assert(s_state.current_temp == 512);
    assert(s_fans[0].current_duty == 37);
    s_auto_temp_enabled = true;
    tick(3000, 49.5, true);
    assert_temperature(495, true);
    puts("PASS explicit auto-temperature disable/enable remains honored");
    return 0;
}
