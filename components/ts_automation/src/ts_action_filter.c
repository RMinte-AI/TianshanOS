#include "ts_action_filter.h"
#include <math.h>
#include <string.h>

static const struct { const char *name; int min, max; } fields[TS_AUTO_FILTER_PARAM_COUNT] = {
    {"speed", 1, 100}, {"intensity", 0, 255}, {"density", 0, 255},
    {"decay", 0, 255}, {"scale", 1, 100}, {"levels", 2, 16},
    {"amount", 0, 100}, {"saturation", 0, 100}, {"angle", 0, 360},
    {"width", 1, 16}, {"wavelength", 1, 32}, {"amplitude", 0, 255},
    {"frequency", 0, 100},
};

esp_err_t ts_action_filter_decode(const cJSON *json, ts_auto_filter_params_t *out)
{
    ts_auto_filter_params_t next = {0};
    if (!out) return ESP_ERR_INVALID_ARG;
    if (json) {
        if (!cJSON_IsObject(json)) return ESP_ERR_INVALID_ARG;
        const cJSON *item;
        cJSON_ArrayForEach(item, json) {
            size_t i;
            for (i = 0; i < TS_AUTO_FILTER_PARAM_COUNT; i++)
                if (item->string && !strcmp(item->string, fields[i].name)) break;
            if (i == TS_AUTO_FILTER_PARAM_COUNT || (next.present & (1u << i)) ||
                !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                trunc(item->valuedouble) != item->valuedouble ||
                item->valuedouble < fields[i].min || item->valuedouble > fields[i].max)
                return ESP_ERR_INVALID_ARG;
            next.present |= 1u << i;
            next.values[i] = (int16_t)item->valuedouble;
        }
    }
    *out = next;
    return ESP_OK;
}

static bool write(const ts_auto_filter_params_t *params, cJSON *json, bool api_units)
{
    if (params->present >> TS_AUTO_FILTER_PARAM_COUNT) return false;
    for (size_t i = 0; i < TS_AUTO_FILTER_PARAM_COUNT; i++) {
        if (!(params->present & (1u << i))) continue;
        int value = params->values[i];
        if (value < fields[i].min || value > fields[i].max) return false;
        if (api_units) {
            if (!strcmp(fields[i].name, "saturation") || !strcmp(fields[i].name, "frequency"))
                value = (int)round(value * 2.55);
            else if (!strcmp(fields[i].name, "amount")) value -= 50;
        }
        if (!cJSON_AddNumberToObject(json, fields[i].name, value)) return false;
    }
    return true;
}

bool ts_action_filter_encode(const ts_auto_filter_params_t *params, cJSON *led)
{
    if (!params->present) return true;
    cJSON *json = cJSON_AddObjectToObject(led, "filter_params");
    return json && write(params, json, false);
}

bool ts_action_filter_api_params(const ts_auto_filter_params_t *params, cJSON *request)
{
    return write(params, request, true);
}
