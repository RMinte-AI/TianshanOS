#include "ts_rule_codec.h"
#include "esp_heap_caps.h"
#include "ts_action_manager.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
/* Descriptors cover the actual C fields, not raw struct bytes or padding. */
typedef enum { STRING, BOOL, U8, U16, U32, I16, ENUM } field_type;
typedef struct {
    const char *name;
    size_t offset, size;
    field_type type;
    int max;
} field;
#define F(T, m, k, t, lim) {k, offsetof(T, m), sizeof(((T *)0)->m), t, lim}
#define A(m, k, t, lim) F(ts_auto_action_t, m, k, t, lim)
#define R(m, k, t, lim) F(ts_auto_rule_t, m, k, t, lim)
static const field common[] = {A(delay_ms, "delay_ms", U16, 65535), A(async, "async", BOOL, 1),
                               A(repeat_count, "repeat_count", U8, 255),
                               A(repeat_interval_ms, "repeat_interval_ms", U16, 65535),
                               A(template_id, "template_id", STRING, 0)};
static const field rule_fields[] = {R(id, "id", STRING, 0),
                                    R(name, "name", STRING, 0),
                                    R(icon, "icon", STRING, 0),
                                    R(enabled, "enabled", BOOL, 1),
                                    R(manual_trigger, "manual_trigger", BOOL, 1),
                                    R(show_on_dashboard, "show_on_dashboard", BOOL, 1),
                                    R(allow_manual_trigger, "allow_manual_trigger", BOOL, 1),
                                    R(cooldown_ms, "cooldown_ms", U32, 0),
                                    R(revision, "revision", U32, 0)};
static const field led_fields[] = {
    A(led.device, "device", STRING, 0),
    A(led.ctrl_type, "ctrl_type", ENUM, 9),
    A(led.index, "index", U8, 255),
    A(led.r, "r", U8, 255),
    A(led.g, "g", U8, 255),
    A(led.b, "b", U8, 255),
    A(led.brightness, "brightness", U8, 255),
    A(led.effect, "effect", STRING, 0),
    A(led.duration_ms, "duration_ms", U16, 65535),
    A(led.speed, "speed", U8, 255),
    A(led.text, "text", STRING, 0),
    A(led.font, "font", STRING, 0),
    A(led.image_path, "image_path", STRING, 0),
    A(led.qr_text, "qr_text", STRING, 0),
    A(led.qr_ecc, "qr_ecc", U8, 255),
    A(led.filter, "filter", STRING, 0),
    A(led.center, "center", BOOL, 1),
    A(led.loop, "loop", BOOL, 1),
    A(led.scroll, "scroll", STRING, 0),
    A(led.align, "align", STRING, 0),
    A(led.x, "x", I16, 32767),
    A(led.y, "y", I16, 32767),
};
static const field ssh_fields[] = {
    A(ssh.host_ref, "host_ref", STRING, 0),
    A(ssh.command, "command", STRING, 0),
    A(ssh.async, "ssh_async", BOOL, 1),
    A(ssh.timeout_ms, "timeout_ms", U32, 0),
};
static const field gpio_fields[] = {
    A(gpio.pin, "pin", U8, 255),
    A(gpio.level, "level", BOOL, 1),
    A(gpio.pulse_ms, "pulse_ms", U32, 0),
};
static const field webhook_fields[] = {
    A(webhook.url, "url", STRING, 0),
    A(webhook.method, "method", STRING, 0),
    A(webhook.body_template, "body_template", STRING, 0),
};
static const field log_fields[] = {
    A(log.level, "level", U8, 255),
    A(log.message, "message", STRING, 0),
};
static const field set_var_fields[] = {
    A(set_var.variable, "variable", STRING, 0),
};
static const field device_fields[] = {
    A(device.device, "device", STRING, 0),
    A(device.action, "action", STRING, 0),
};
static const field ssh_ref_fields[] = {
    A(ssh_ref.cmd_id, "cmd_id", STRING, 0),
};
static const field cli_fields[] = {
    A(cli.command, "command", STRING, 0),
    A(cli.var_name, "var_name", STRING, 0),
    A(cli.timeout_ms, "timeout_ms", U32, 0),
};
static const struct {
    const field *fields;
    size_t count;
} by_type[] = {
    {led_fields, sizeof(led_fields) / sizeof(field)},
    {ssh_fields, sizeof(ssh_fields) / sizeof(field)},
    {gpio_fields, sizeof(gpio_fields) / sizeof(field)},
    {webhook_fields, sizeof(webhook_fields) / sizeof(field)},
    {log_fields, sizeof(log_fields) / sizeof(field)},
    {set_var_fields, sizeof(set_var_fields) / sizeof(field)},
    {device_fields, sizeof(device_fields) / sizeof(field)},
    {ssh_ref_fields, sizeof(ssh_ref_fields) / sizeof(field)},
    {cli_fields, sizeof(cli_fields) / sizeof(field)},
};
static const char *types[] = {"led",     "ssh",         "gpio",        "webhook", "log",
                              "set_var", "device_ctrl", "ssh_cmd_ref", "cli"};
static const char *ops[] = {"eq", "ne", "lt", "le", "gt", "ge", "contains"};
static const char *repeats[] = {"once", "while_true", "count"};
static int choice(const cJSON *j, const char *const *names, size_t count, int def) {
    if (!j)
        return def;
    if (!cJSON_IsString(j))
        return -1;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(j->valuestring, names[i]))
            return i;
    return -1;
}
static bool fields_read(const cJSON *j, void *value, const field *fields, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const field *f = &fields[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, f->name);
        if (!v)
            continue;
        char *p = (char *)value + f->offset;
        if (f->type == STRING) {
            if (!cJSON_IsString(v) || strlen(v->valuestring) >= f->size)
                return false;
            strcpy(p, v->valuestring);
            continue;
        }
        if (f->type == BOOL) {
            if (!cJSON_IsBool(v))
                return false;
            *(bool *)p = cJSON_IsTrue(v);
            continue;
        }
        double d = v->valuedouble;
        double max = f->type == U32 ? UINT32_MAX : f->max;
        if (!cJSON_IsNumber(v) || !isfinite(d) || trunc(d) != d || d > (double)max ||
            d < (f->type == I16 ? INT16_MIN : 0))
            return false;
        switch (f->type) {
        case U8:
            *(uint8_t *)p = d;
            break;
        case U16:
            *(uint16_t *)p = d;
            break;
        case U32:
            *(uint32_t *)p = d;
            break;
        case I16:
            *(int16_t *)p = d;
            break;
        case ENUM:
            *(int *)p = d;
            break;
        default:
            return false;
        }
    }
    return true;
}
static bool fields_write(cJSON *j, const void *value, const field *fields, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const field *f = &fields[i];
        const char *p = (const char *)value + f->offset;
        cJSON *v = NULL;
        if (f->type == STRING) {
            if (!memchr(p, 0, f->size))
                return false;
            v = cJSON_CreateString(p);
        } else if (f->type == BOOL)
            v = cJSON_CreateBool(*(const bool *)p);
        else {
            double d = 0;
            switch (f->type) {
            case U8:
                d = *(const uint8_t *)p;
                break;
            case U16:
                d = *(const uint16_t *)p;
                break;
            case U32:
                d = *(const uint32_t *)p;
                break;
            case I16:
                d = *(const int16_t *)p;
                break;
            case ENUM:
                d = *(const int *)p;
                break;
            default:
                return false;
            }
            v = cJSON_CreateNumber(d);
        }
        if (!v)
            return false;
        if (!cJSON_AddItemToObject(j, f->name, v)) {
            cJSON_Delete(v);
            return false;
        }
    }
    return true;
}
static bool value_read(const cJSON *j, ts_auto_value_t *v) {
    if (!j || cJSON_IsNull(j)) {
        v->type = TS_AUTO_VAL_NULL;
        return true;
    }
    if (cJSON_IsBool(j)) {
        v->type = TS_AUTO_VAL_BOOL;
        v->bool_val = cJSON_IsTrue(j);
        return true;
    }
    if (cJSON_IsNumber(j) && isfinite(j->valuedouble)) {
        double d = j->valuedouble;
        if (d >= INT32_MIN && d <= INT32_MAX && trunc(d) == d) {
            v->type = TS_AUTO_VAL_INT;
            v->int_val = d;
        } else {
            v->type = TS_AUTO_VAL_FLOAT;
            v->float_val = d;
        }
        return true;
    }
    if (cJSON_IsString(j) && strlen(j->valuestring) < sizeof(v->str_val)) {
        v->type = TS_AUTO_VAL_STRING;
        strcpy(v->str_val, j->valuestring);
        return true;
    }
    return false;
}
static bool typed_value_read(const cJSON *object, ts_auto_value_t *v) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "value");
    if (!value_read(value, v))
        return false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(object, "value_type");
    if (!type)
        return true;
    if (!cJSON_IsNumber(type) || type->valuedouble != type->valueint)
        return false;
    if (type->valueint == TS_AUTO_VAL_FLOAT && cJSON_IsNumber(value)) {
        memset(v, 0, sizeof(*v));
        v->type = TS_AUTO_VAL_FLOAT;
        v->float_val = value->valuedouble;
        return true;
    }
    return type->valueint == v->type;
}
static cJSON *value_write(const ts_auto_value_t *v) {
    switch (v->type) {
    case TS_AUTO_VAL_NULL:
        return cJSON_CreateNull();
    case TS_AUTO_VAL_BOOL:
        return cJSON_CreateBool(v->bool_val);
    case TS_AUTO_VAL_INT:
        return cJSON_CreateNumber(v->int_val);
    case TS_AUTO_VAL_FLOAT:
        return isfinite(v->float_val) ? cJSON_CreateNumber(v->float_val) : NULL;
    case TS_AUTO_VAL_STRING:
        return memchr(v->str_val, 0, sizeof(v->str_val)) ? cJSON_CreateString(v->str_val) : NULL;
    default:
        return NULL;
    }
}
static bool condition_read(const cJSON *j, char *variable, int *op, ts_auto_value_t *v) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "variable");
    int o = choice(cJSON_GetObjectItemCaseSensitive(j, "operator"), ops, 7, 0);
    if (!cJSON_IsObject(j) || !cJSON_IsString(name) || !name->valuestring[0] ||
        strlen(name->valuestring) >= TS_AUTO_NAME_MAX_LEN || o < 0)
        return false;
    strcpy(variable, name->valuestring);
    *op = o;
    return typed_value_read(j, v);
}
static cJSON *condition_write(const char *variable, int op, const ts_auto_value_t *value) {
    if (op < 0 || op >= 7)
        return NULL;
    cJSON *j = cJSON_CreateObject(), *v = value_write(value);
    if (!j || !v) {
        cJSON_Delete(j);
        cJSON_Delete(v);
        return NULL;
    }
    if (!cJSON_AddItemToObject(j, "value", v)) {
        cJSON_Delete(v);
        cJSON_Delete(j);
        return NULL;
    }
    if (!cJSON_AddStringToObject(j, "variable", variable) ||
        !cJSON_AddStringToObject(j, "operator", ops[op]) ||
        !cJSON_AddNumberToObject(j, "value_type", value->type)) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
bool ts_rule_id_valid(const char *id) {
    if (!id || !id[0] || strlen(id) >= TS_AUTO_NAME_MAX_LEN || !strcmp(id, ".") ||
        !strcmp(id, ".."))
        return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (*p < 32 || *p == '/' || *p == '\\')
            return false;
    return true;
}
void ts_rule_dispose(ts_auto_rule_t *r) {
    if (!r)
        return;
    free(r->conditions.conditions);
    free(r->actions);
    memset(r, 0, sizeof(*r));
}
esp_err_t ts_rule_decode(const cJSON *j, ts_auto_rule_t *r) {
    if (!r || !cJSON_IsObject(j))
        return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof(*r));
    r->enabled = true;
    r->revision = 1;
    if (!fields_read(j, r, rule_fields, sizeof(rule_fields) / sizeof(field)) ||
        !ts_rule_id_valid(r->id) || !r->name[0])
        goto invalid;
    if (cJSON_HasObjectItem(j, "show_on_dashboard"))
        r->presentation_fields |= 1;
    if (cJSON_HasObjectItem(j, "allow_manual_trigger"))
        r->presentation_fields |= 2;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "conditions"), *items = c;
    const cJSON *logic = cJSON_GetObjectItemCaseSensitive(j, "logic");
    if (cJSON_IsObject(c)) {
        items = cJSON_GetObjectItemCaseSensitive(c, "items");
        logic = cJSON_GetObjectItemCaseSensitive(c, "logic");
    }
    const char *logics[] = {"and", "or"};
    int li = choice(logic, logics, 2, 0);
    if (li < 0)
        goto invalid;
    r->conditions.logic = li;
    if (items) {
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > UINT8_MAX)
            goto invalid;
        r->conditions.count = cJSON_GetArraySize(items);
    }
    if (r->conditions.count) {
        r->conditions.conditions = heap_caps_calloc(
            r->conditions.count, sizeof(ts_auto_condition_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->conditions.conditions)
            goto oom;
    }
    for (unsigned i = 0; i < r->conditions.count; ++i) {
        ts_auto_condition_t *cond = &r->conditions.conditions[i];
        int op;
        if (!condition_read(cJSON_GetArrayItem(items, i), cond->variable, &op, &cond->value))
            goto invalid;
        cond->op = op;
    }
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(j, "actions");
    if (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) > UINT8_MAX ||
        !cJSON_GetArraySize(actions))
        goto invalid;
    r->action_count = cJSON_GetArraySize(actions);
    r->actions = heap_caps_calloc(r->action_count, sizeof(ts_auto_action_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->actions)
        goto oom;
    for (unsigned i = 0; i < r->action_count; ++i) {
        const cJSON *a = cJSON_GetArrayItem(actions, i);
        ts_auto_action_t *out = &r->actions[i];
        if (!cJSON_IsObject(a))
            goto invalid;
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "type"),
                    *tpl = cJSON_GetObjectItemCaseSensitive(a, "template_id");
        if (!type && cJSON_IsString(tpl)) {
            ts_action_template_t *t =
                heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!t)
                goto oom;
            esp_err_t ret = ts_action_template_get(tpl->valuestring, t);
            if (ret == ESP_OK)
                *out = t->action;
            free(t);
            if (ret != ESP_OK)
                out->type = TS_AUTO_ACT_TEMPLATE_REF;
        } else {
            int ty = choice(type, types, 9, -1);
            if (cJSON_IsString(type) && !strcmp(type->valuestring, "device"))
                ty = TS_AUTO_ACT_DEVICE_CTRL;
            if (ty < 0)
                goto invalid;
            out->type = ty;
        }
        if (!fields_read(a, out, common, sizeof(common) / sizeof(field)) ||
            (out->type != TS_AUTO_ACT_TEMPLATE_REF &&
             !fields_read(a, out, by_type[out->type].fields, by_type[out->type].count)))
            goto invalid;
        int rep = choice(cJSON_GetObjectItemCaseSensitive(a, "repeat_mode"), repeats, 3,
                         out->repeat_mode);
        if (rep < 0)
            goto invalid;
        out->repeat_mode = rep;
        if (cJSON_HasObjectItem(a, "condition")) {
            const cJSON *ac = cJSON_GetObjectItemCaseSensitive(a, "condition");
            out->condition.has_condition = !cJSON_IsNull(ac);
            int op;
            if (out->condition.has_condition) {
                if (!condition_read(ac, out->condition.variable, &op, &out->condition.value))
                    goto invalid;
                out->condition.op = op;
            }
        }
        if (out->type == TS_AUTO_ACT_SET_VAR && !typed_value_read(a, &out->set_var.value))
            goto invalid;
    }
    return ESP_OK;
oom:
    ts_rule_dispose(r);
    return ESP_ERR_NO_MEM;
invalid:
    ts_rule_dispose(r);
    return ESP_ERR_INVALID_ARG;
}
cJSON *ts_rule_encode(const ts_auto_rule_t *r) {
    if (!r || (r->conditions.count && !r->conditions.conditions) ||
        (r->action_count && !r->actions) ||
        (r->conditions.logic != TS_AUTO_LOGIC_AND && r->conditions.logic != TS_AUTO_LOGIC_OR))
        return NULL;
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return NULL;
    if (!fields_write(j, r, rule_fields, sizeof(rule_fields) / sizeof(field)))
        goto fail;
    if (!(r->presentation_fields & 1))
        cJSON_DeleteItemFromObject(j, "show_on_dashboard");
    if (!(r->presentation_fields & 2))
        cJSON_DeleteItemFromObject(j, "allow_manual_trigger");
    if (!cJSON_AddStringToObject(j, "logic",
                                 r->conditions.logic == TS_AUTO_LOGIC_OR ? "or" : "and"))
        goto fail;
    cJSON *conditions = cJSON_AddArrayToObject(j, "conditions"),
          *actions = cJSON_AddArrayToObject(j, "actions");
    if (!conditions || !actions)
        goto fail;
    for (unsigned i = 0; i < r->conditions.count; ++i) {
        const ts_auto_condition_t *c = &r->conditions.conditions[i];
        cJSON *cj = condition_write(c->variable, c->op, &c->value);
        if (!cj)
            goto fail;
        if (!cJSON_AddItemToArray(conditions, cj)) {
            cJSON_Delete(cj);
            goto fail;
        }
    }
    for (unsigned i = 0; i < r->action_count; ++i) {
        const ts_auto_action_t *a = &r->actions[i];
        if ((a->type == TS_AUTO_ACT_TEMPLATE_REF && !a->template_id[0]) || a->type < 0 ||
            a->type > TS_AUTO_ACT_TEMPLATE_REF || a->repeat_mode < 0 ||
            a->repeat_mode > TS_AUTO_REPEAT_COUNT)
            goto fail;
        cJSON *aj = cJSON_CreateObject();
        if (!aj)
            goto fail;
        if (!cJSON_AddItemToArray(actions, aj)) {
            cJSON_Delete(aj);
            goto fail;
        }
        if ((a->type != TS_AUTO_ACT_TEMPLATE_REF &&
             !cJSON_AddStringToObject(aj, "type", types[a->type])) ||
            !fields_write(aj, a, common, sizeof(common) / sizeof(field)) ||
            (a->type != TS_AUTO_ACT_TEMPLATE_REF &&
             !fields_write(aj, a, by_type[a->type].fields, by_type[a->type].count)) ||
            !cJSON_AddStringToObject(aj, "repeat_mode", repeats[a->repeat_mode]))
            goto fail;
        if (a->condition.has_condition) {
            cJSON *cj =
                condition_write(a->condition.variable, a->condition.op, &a->condition.value);
            if (!cj)
                goto fail;
            if (!cJSON_AddItemToObject(aj, "condition", cj)) {
                cJSON_Delete(cj);
                goto fail;
            }
        }
        if (a->type == TS_AUTO_ACT_SET_VAR) {
            if (!cJSON_AddNumberToObject(aj, "value_type", a->set_var.value.type))
                goto fail;
            cJSON *v = value_write(&a->set_var.value);
            if (!v)
                goto fail;
            if (!cJSON_AddItemToObject(aj, "value", v)) {
                cJSON_Delete(v);
                goto fail;
            }
        }
    }
    return j;
fail:
    cJSON_Delete(j);
    return NULL;
}
