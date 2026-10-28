#include "ts_action_codec.h"
#include "ts_action_filter.h"
#include "ts_rule_codec.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

bool ts_source_config_same(const ts_auto_source_t *a, const ts_auto_source_t *b)
{
    return !strcmp(a->id, b->id) && !strcmp(a->label, b->label) && a->type == b->type &&
        a->poll_interval_ms == b->poll_interval_ms && a->enabled == b->enabled &&
        a->auto_discover == b->auto_discover && a->mapping_count == b->mapping_count &&
        !memcmp(&a->builtin, &b->builtin, offsetof(ts_auto_source_t, mappings) - offsetof(ts_auto_source_t, builtin)) &&
        !memcmp(a->mappings, b->mappings, a->mapping_count * sizeof(*a->mappings));
}
#include <math.h>

/* Values must survive a save/load unchanged; never narrow or truncate them. */
#define READ_NUMBER(target, minimum, maximum) do { \
    double number = item->valuedouble; \
    if (!cJSON_IsNumber(item) || !isfinite(number) || number < (minimum) || \
        number > (maximum) || trunc(number) != number) goto invalid; \
    target = number; \
} while (0)
#define READ_BOOL(target) do { \
    if (!cJSON_IsBool(item)) goto invalid; \
    target = cJSON_IsTrue(item); \
} while (0)

static bool copy_string(char *out, size_t capacity, const char *value)
{
    if (strlen(value) >= capacity) return false;
    strcpy(out, value);
    return true;
}

static const char *action_type_to_str(ts_auto_action_type_t type)
{
    switch (type) {
        case TS_AUTO_ACT_CLI: return "cli";
        case TS_AUTO_ACT_LED: return "led";
        case TS_AUTO_ACT_SSH_CMD: return "ssh_cmd";
        case TS_AUTO_ACT_SSH_CMD_REF: return "ssh_cmd_ref";
        case TS_AUTO_ACT_GPIO: return "gpio";
        case TS_AUTO_ACT_WEBHOOK: return "webhook";
        case TS_AUTO_ACT_LOG: return "log";
        case TS_AUTO_ACT_SET_VAR: return "set_var";
        case TS_AUTO_ACT_DEVICE_CTRL: return "device_ctrl";
        default: return "unknown";
    }
}

static ts_auto_action_type_t str_to_action_type(const char *str)
{
    if (!str) return TS_AUTO_ACT_LOG;
    if (strcmp(str, "cli") == 0) return TS_AUTO_ACT_CLI;
    if (strcmp(str, "led") == 0) return TS_AUTO_ACT_LED;
    if (strcmp(str, "ssh_cmd") == 0) return TS_AUTO_ACT_SSH_CMD;
    if (strcmp(str, "ssh_cmd_ref") == 0) return TS_AUTO_ACT_SSH_CMD_REF;
    if (strcmp(str, "gpio") == 0) return TS_AUTO_ACT_GPIO;
    if (strcmp(str, "webhook") == 0) return TS_AUTO_ACT_WEBHOOK;
    if (strcmp(str, "log") == 0) return TS_AUTO_ACT_LOG;
    if (strcmp(str, "set_var") == 0) return TS_AUTO_ACT_SET_VAR;
    if (strcmp(str, "device_ctrl") == 0) return TS_AUTO_ACT_DEVICE_CTRL;
    return TS_AUTO_ACT_LOG;
}

char *ts_action_template_encode(const ts_action_template_t *tpl)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    if (!cJSON_AddStringToObject(root, "id", tpl->id)) goto failed;
    if (!cJSON_AddStringToObject(root, "name", tpl->name)) goto failed;
    if (!cJSON_AddStringToObject(root, "description", tpl->description)) goto failed;
    if (!cJSON_AddBoolToObject(root, "enabled", tpl->enabled)) goto failed;
    if (!cJSON_AddBoolToObject(root, "async", tpl->async)) goto failed;
    if (!cJSON_AddStringToObject(root, "type", action_type_to_str(tpl->action.type))) goto failed;
    if (!cJSON_AddNumberToObject(root, "delay_ms", tpl->action.delay_ms)) goto failed;
    if (!cJSON_AddNumberToObject(root, "created_at", tpl->created_at)) goto failed;

    /* Serialize type-specific data */
    switch (tpl->action.type) {
        case TS_AUTO_ACT_SSH_CMD: {
            cJSON *ssh = cJSON_AddObjectToObject(root, "ssh");
            if (!ssh || !cJSON_AddStringToObject(ssh, "host_ref", tpl->action.ssh.host_ref) ||
                !cJSON_AddStringToObject(ssh, "command", tpl->action.ssh.command) ||
                !cJSON_AddNumberToObject(ssh, "timeout_ms", tpl->action.ssh.timeout_ms) ||
                !cJSON_AddBoolToObject(ssh, "async", tpl->action.ssh.async)) goto failed;
            break;
        }
        case TS_AUTO_ACT_DEVICE_CTRL: {
            cJSON *device = cJSON_AddObjectToObject(root, "device");
            if (!device || !cJSON_AddStringToObject(device, "device", tpl->action.device.device) ||
                !cJSON_AddStringToObject(device, "action", tpl->action.device.action)) goto failed;
            break;
        }
        case TS_AUTO_ACT_CLI: {
            cJSON *cli = cJSON_AddObjectToObject(root, "cli");
            if (!cli) goto failed;
            if (!cJSON_AddStringToObject(cli, "command", tpl->action.cli.command)) goto failed;
            if (!cJSON_AddStringToObject(cli, "var_name", tpl->action.cli.var_name)) goto failed;
            if (!cJSON_AddNumberToObject(cli, "timeout_ms", tpl->action.cli.timeout_ms)) goto failed;
            break;
        }
        case TS_AUTO_ACT_SSH_CMD_REF: {
            cJSON *ssh_ref = cJSON_AddObjectToObject(root, "ssh_ref");
            if (!ssh_ref) goto failed;
            if (!cJSON_AddStringToObject(ssh_ref, "cmd_id", tpl->action.ssh_ref.cmd_id)) goto failed;
            break;
        }
        case TS_AUTO_ACT_LED: {
            cJSON *led = cJSON_AddObjectToObject(root, "led");
            if (!led) goto failed;
            if (!cJSON_AddStringToObject(led, "device", tpl->action.led.device)) goto failed;
            /* 控制类型 */
            const char *ctrl_type_str = "fill";
            switch (tpl->action.led.ctrl_type) {
                case TS_LED_CTRL_EFFECT: ctrl_type_str = "effect"; break;
                case TS_LED_CTRL_BRIGHTNESS: ctrl_type_str = "brightness"; break;
                case TS_LED_CTRL_OFF: ctrl_type_str = "off"; break;
                case TS_LED_CTRL_TEXT: ctrl_type_str = "text"; break;
                case TS_LED_CTRL_IMAGE: ctrl_type_str = "image"; break;
                case TS_LED_CTRL_QRCODE: ctrl_type_str = "qrcode"; break;
                case TS_LED_CTRL_FILTER: ctrl_type_str = "filter"; break;
                case TS_LED_CTRL_FILTER_STOP: ctrl_type_str = "filter_stop"; break;
                case TS_LED_CTRL_TEXT_STOP: ctrl_type_str = "text_stop"; break;
                default: ctrl_type_str = "fill"; break;
            }
            if (!cJSON_AddStringToObject(led, "ctrl_type", ctrl_type_str)) goto failed;
            if (!cJSON_AddNumberToObject(led, "index", tpl->action.led.index)) goto failed;
            /* 颜色输出为 hex */
            char color_hex[8];
            snprintf(color_hex, sizeof(color_hex), "#%02X%02X%02X",
                     tpl->action.led.r, tpl->action.led.g, tpl->action.led.b);
            if (!cJSON_AddStringToObject(led, "color", color_hex)) goto failed;
            if (!cJSON_AddNumberToObject(led, "brightness", tpl->action.led.brightness)) goto failed;
            if (tpl->action.led.effect[0]) {
                if (!cJSON_AddStringToObject(led, "effect", tpl->action.led.effect)) goto failed;
            }
            if (!cJSON_AddNumberToObject(led, "speed", tpl->action.led.speed)) goto failed;
            if (!cJSON_AddNumberToObject(led, "duration_ms", tpl->action.led.duration_ms)) goto failed;
            /* Matrix 高级功能 */
            if (tpl->action.led.text[0]) {
                if (!cJSON_AddStringToObject(led, "text", tpl->action.led.text)) goto failed;
            }
            if (tpl->action.led.font[0]) {
                if (!cJSON_AddStringToObject(led, "font", tpl->action.led.font)) goto failed;
            }
            if (tpl->action.led.image_path[0]) {
                if (!cJSON_AddStringToObject(led, "image_path", tpl->action.led.image_path)) goto failed;
            }
            if (tpl->action.led.qr_text[0]) {
                if (!cJSON_AddStringToObject(led, "qr_text", tpl->action.led.qr_text)) goto failed;
            }
            if (tpl->action.led.qr_ecc) {
                char ecc_str[2] = { tpl->action.led.qr_ecc, '\0' };
                if (!cJSON_AddStringToObject(led, "qr_ecc", ecc_str)) goto failed;
            }
            if (tpl->action.led.filter[0]) {
                if (!cJSON_AddStringToObject(led, "filter", tpl->action.led.filter)) goto failed;
            }
            if (!ts_action_filter_encode(&tpl->action.led.filter_params, led)) goto failed;
            if (!cJSON_AddBoolToObject(led, "center", tpl->action.led.center)) goto failed;
            if (!cJSON_AddBoolToObject(led, "loop", tpl->action.led.loop)) goto failed;
            if (tpl->action.led.scroll[0]) {
                if (!cJSON_AddStringToObject(led, "scroll", tpl->action.led.scroll)) goto failed;
            }
            if (tpl->action.led.align[0]) {
                if (!cJSON_AddStringToObject(led, "align", tpl->action.led.align)) goto failed;
            }
            if (!cJSON_AddNumberToObject(led, "x", tpl->action.led.x)) goto failed;
            if (!cJSON_AddNumberToObject(led, "y", tpl->action.led.y)) goto failed;
            break;
        }
        case TS_AUTO_ACT_LOG: {
            cJSON *log_obj = cJSON_AddObjectToObject(root, "log");
            if (!log_obj) goto failed;
            if (!cJSON_AddNumberToObject(log_obj, "level", tpl->action.log.level)) goto failed;
            if (!cJSON_AddStringToObject(log_obj, "message", tpl->action.log.message)) goto failed;
            break;
        }
        case TS_AUTO_ACT_SET_VAR: {
            cJSON *set_var = cJSON_AddObjectToObject(root, "set_var");
            if (!set_var) goto failed;
            if (!cJSON_AddStringToObject(set_var, "variable", tpl->action.set_var.variable)) goto failed;
            if (!cJSON_AddNumberToObject(set_var, "value_type", tpl->action.set_var.value.type)) goto failed;
            cJSON *value = ts_auto_value_encode(&tpl->action.set_var.value);
            if (!value) goto failed;
            if (!cJSON_AddItemToObject(set_var, "value", value)) { cJSON_Delete(value); goto failed; }
            break;
        }
        case TS_AUTO_ACT_WEBHOOK: {
            cJSON *webhook = cJSON_AddObjectToObject(root, "webhook");
            if (!webhook) goto failed;
            if (!cJSON_AddStringToObject(webhook, "url", tpl->action.webhook.url)) goto failed;
            if (!cJSON_AddStringToObject(webhook, "method", tpl->action.webhook.method)) goto failed;
            if (!cJSON_AddStringToObject(webhook, "body_template", tpl->action.webhook.body_template)) goto failed;
            break;
        }
        default:
            break;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
failed:
    cJSON_Delete(root);
    return NULL;
}

esp_err_t ts_action_template_decode(const char *json, ts_action_template_t *tpl)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;
    cJSON *owner = root;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "type");
    bool envelope = cJSON_IsString(kind) && !strcmp(kind->valuestring, "action_template");
    if (envelope) {
        root = cJSON_GetObjectItemCaseSensitive(owner, "template");
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsObject(root) || !cJSON_IsString(id) || !id->valuestring[0] ||
        strlen(id->valuestring) >= sizeof(tpl->id) ||
        (envelope && !cJSON_IsString(type)) ||
        (type && (!cJSON_IsString(type) ||
         strcmp(action_type_to_str(str_to_action_type(type->valuestring)), type->valuestring))) ||
        (cJSON_IsString(type) && !strcmp(type->valuestring, "led") &&
         !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "led")))) {
        cJSON_Delete(owner);
        return ESP_ERR_INVALID_ARG;
    }

    memset(tpl, 0, sizeof(ts_action_template_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "id")) && cJSON_IsString(item)) {
        if (!copy_string(tpl->id, sizeof(tpl->id), item->valuestring)) goto invalid;
    }
    if ((item = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(item)) {
        if (!copy_string(tpl->name, sizeof(tpl->name), item->valuestring)) goto invalid;
    }
    if ((item = cJSON_GetObjectItem(root, "description")) && cJSON_IsString(item)) {
        if (!copy_string(tpl->description, sizeof(tpl->description), item->valuestring)) goto invalid;
    }
    if ((item = cJSON_GetObjectItem(root, "enabled"))) {
        READ_BOOL(tpl->enabled);
    } else {
        tpl->enabled = true;
    }
    if ((item = cJSON_GetObjectItem(root, "type")) && cJSON_IsString(item)) {
        tpl->action.type = str_to_action_type(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "async"))) {
        READ_BOOL(tpl->async);
    }
    if ((item = cJSON_GetObjectItem(root, "delay_ms"))) {
        READ_NUMBER(tpl->action.delay_ms, 0, UINT16_MAX);
    }
    if ((item = cJSON_GetObjectItem(root, "created_at"))) {
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
            item->valuedouble < 0 || item->valuedouble >= 0x1p63 ||
            trunc(item->valuedouble) != item->valuedouble) goto invalid;
        tpl->created_at = (int64_t)item->valuedouble;
    }

    /* Parse type-specific data */
    switch (tpl->action.type) {
        case TS_AUTO_ACT_SSH_CMD: {
            cJSON *ssh = cJSON_GetObjectItem(root, "ssh");
            if (ssh) {
                if ((item = cJSON_GetObjectItem(ssh, "host_ref")) && cJSON_IsString(item))
                    if (!copy_string(tpl->action.ssh.host_ref, sizeof(tpl->action.ssh.host_ref), item->valuestring)) goto invalid;
                if ((item = cJSON_GetObjectItem(ssh, "command")) && cJSON_IsString(item))
                    if (!copy_string(tpl->action.ssh.command, sizeof(tpl->action.ssh.command), item->valuestring)) goto invalid;
                if ((item = cJSON_GetObjectItem(ssh, "timeout_ms")))
                    READ_NUMBER(tpl->action.ssh.timeout_ms, 0, UINT32_MAX);
                if ((item = cJSON_GetObjectItem(ssh, "async")))
                    READ_BOOL(tpl->action.ssh.async);
            }
            break;
        }
        case TS_AUTO_ACT_DEVICE_CTRL: {
            cJSON *device = cJSON_GetObjectItem(root, "device");
            if (device) {
                if ((item = cJSON_GetObjectItem(device, "device")) && cJSON_IsString(item))
                    if (!copy_string(tpl->action.device.device, sizeof(tpl->action.device.device), item->valuestring)) goto invalid;
                if ((item = cJSON_GetObjectItem(device, "action")) && cJSON_IsString(item))
                    if (!copy_string(tpl->action.device.action, sizeof(tpl->action.device.action), item->valuestring)) goto invalid;
            }
            break;
        }
        case TS_AUTO_ACT_CLI: {
            cJSON *cli = cJSON_GetObjectItem(root, "cli");
            if (cli) {
                if ((item = cJSON_GetObjectItem(cli, "command")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.cli.command, sizeof(tpl->action.cli.command), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(cli, "var_name")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.cli.var_name, sizeof(tpl->action.cli.var_name), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(cli, "timeout_ms"))) {
                    READ_NUMBER(tpl->action.cli.timeout_ms, 0, UINT32_MAX);
                }
            }
            break;
        }
        case TS_AUTO_ACT_SSH_CMD_REF: {
            cJSON *ssh_ref = cJSON_GetObjectItem(root, "ssh_ref");
            if (ssh_ref) {
                if ((item = cJSON_GetObjectItem(ssh_ref, "cmd_id")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.ssh_ref.cmd_id, sizeof(tpl->action.ssh_ref.cmd_id), item->valuestring)) goto invalid;
                }
            }
            break;
        }
        case TS_AUTO_ACT_LED: {
            cJSON *led = cJSON_GetObjectItem(root, "led");
            if (led) {
                if ((item = cJSON_GetObjectItem(led, "device")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.device, sizeof(tpl->action.led.device), item->valuestring)) goto invalid;
                }
                /* 解析控制类型 */
                if ((item = cJSON_GetObjectItem(led, "ctrl_type")) && cJSON_IsString(item)) {
                    const char *ct = item->valuestring;
                    if (strcmp(ct, "fill") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_FILL;
                    else if (strcmp(ct, "effect") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_EFFECT;
                    else if (strcmp(ct, "brightness") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_BRIGHTNESS;
                    else if (strcmp(ct, "off") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_OFF;
                    else if (strcmp(ct, "text") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_TEXT;
                    else if (strcmp(ct, "image") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_IMAGE;
                    else if (strcmp(ct, "qrcode") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_QRCODE;
                    else if (strcmp(ct, "filter") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_FILTER;
                    else if (strcmp(ct, "filter_stop") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_FILTER_STOP;
                    else if (strcmp(ct, "text_stop") == 0) tpl->action.led.ctrl_type = TS_LED_CTRL_TEXT_STOP;
                    else goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "index"))) {
                    READ_NUMBER(tpl->action.led.index, 0, UINT8_MAX);
                } else {
                    tpl->action.led.index = 0xFF;  /* Default: fill all */
                }
                /* 解析颜色（支持 color 字符串或 r/g/b 数值） */
                if ((item = cJSON_GetObjectItem(led, "color")) && cJSON_IsString(item)) {
                    /* Parse hex color like "#FF0000" */
                    const char *hex = item->valuestring;
                    if (hex[0] == '#') hex++;
                    uint32_t color_val = strtoul(hex, NULL, 16);
                    tpl->action.led.r = (color_val >> 16) & 0xFF;
                    tpl->action.led.g = (color_val >> 8) & 0xFF;
                    tpl->action.led.b = color_val & 0xFF;
                } else {
                    if ((item = cJSON_GetObjectItem(led, "r"))) {
                        READ_NUMBER(tpl->action.led.r, 0, UINT8_MAX);
                    }
                    if ((item = cJSON_GetObjectItem(led, "g"))) {
                        READ_NUMBER(tpl->action.led.g, 0, UINT8_MAX);
                    }
                    if ((item = cJSON_GetObjectItem(led, "b"))) {
                        READ_NUMBER(tpl->action.led.b, 0, UINT8_MAX);
                    }
                }
                if ((item = cJSON_GetObjectItem(led, "brightness"))) {
                    READ_NUMBER(tpl->action.led.brightness, 0, UINT8_MAX);
                }
                if ((item = cJSON_GetObjectItem(led, "effect")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.effect, sizeof(tpl->action.led.effect), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "speed"))) {
                    READ_NUMBER(tpl->action.led.speed, 0, UINT8_MAX);
                }
                if ((item = cJSON_GetObjectItem(led, "duration_ms"))) {
                    READ_NUMBER(tpl->action.led.duration_ms, 0, UINT16_MAX);
                }
                /* Matrix 高级功能参数 */
                if ((item = cJSON_GetObjectItem(led, "text")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.text, sizeof(tpl->action.led.text), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "font")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.font, sizeof(tpl->action.led.font), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "image_path")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.image_path, sizeof(tpl->action.led.image_path), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "qr_text")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.qr_text, sizeof(tpl->action.led.qr_text), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "qr_ecc")) && cJSON_IsString(item)) {
                    tpl->action.led.qr_ecc = item->valuestring[0];  /* L/M/Q/H */
                }
                if ((item = cJSON_GetObjectItem(led, "filter")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.filter, sizeof(tpl->action.led.filter), item->valuestring)) goto invalid;
                }
                if (ts_action_filter_decode(cJSON_GetObjectItemCaseSensitive(led, "filter_params"),
                                            &tpl->action.led.filter_params) != ESP_OK) {
                    cJSON_Delete(owner);
                    return ESP_ERR_INVALID_ARG;
                }
                if ((item = cJSON_GetObjectItem(led, "center"))) {
                    READ_BOOL(tpl->action.led.center);
                }
                if ((item = cJSON_GetObjectItem(led, "loop"))) {
                    READ_BOOL(tpl->action.led.loop);
                }
                if ((item = cJSON_GetObjectItem(led, "scroll")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.scroll, sizeof(tpl->action.led.scroll), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "align")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.led.align, sizeof(tpl->action.led.align), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(led, "x"))) {
                    READ_NUMBER(tpl->action.led.x, INT16_MIN, INT16_MAX);
                }
                if ((item = cJSON_GetObjectItem(led, "y"))) {
                    READ_NUMBER(tpl->action.led.y, INT16_MIN, INT16_MAX);
                }
            }
            break;
        }
        case TS_AUTO_ACT_LOG: {
            cJSON *log_obj = cJSON_GetObjectItem(root, "log");
            if (log_obj) {
                if ((item = cJSON_GetObjectItem(log_obj, "level"))) {
                    READ_NUMBER(tpl->action.log.level, 0, UINT8_MAX);
                }
                if ((item = cJSON_GetObjectItem(log_obj, "message")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.log.message, sizeof(tpl->action.log.message), item->valuestring)) goto invalid;
                }
            }
            break;
        }
        case TS_AUTO_ACT_SET_VAR: {
            cJSON *set_var = cJSON_GetObjectItem(root, "set_var");
            if (set_var) {
                if ((item = cJSON_GetObjectItem(set_var, "variable")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.set_var.variable, sizeof(tpl->action.set_var.variable), item->valuestring)) goto invalid;
                }
                if (!ts_auto_value_decode(set_var, &tpl->action.set_var.value)) goto invalid;
            }
            break;
        }
        case TS_AUTO_ACT_WEBHOOK: {
            cJSON *webhook = cJSON_GetObjectItem(root, "webhook");
            if (webhook) {
                if ((item = cJSON_GetObjectItem(webhook, "url")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.webhook.url, sizeof(tpl->action.webhook.url), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(webhook, "method")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.webhook.method, sizeof(tpl->action.webhook.method), item->valuestring)) goto invalid;
                }
                if ((item = cJSON_GetObjectItem(webhook, "body_template")) && cJSON_IsString(item)) {
                    if (!copy_string(tpl->action.webhook.body_template, sizeof(tpl->action.webhook.body_template), item->valuestring)) goto invalid;
                }
            }
            break;
        }
        default:
            break;
    }

    cJSON_Delete(owner);
    return ESP_OK;
invalid:
    cJSON_Delete(owner);
    return ESP_ERR_INVALID_ARG;
}

char *ts_source_config_encode(const ts_auto_source_t *src)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    if (!cJSON_AddStringToObject(root, "id", src->id)) goto failed;
    if (!cJSON_AddStringToObject(root, "label", src->label)) goto failed;
    if (!cJSON_AddNumberToObject(root, "type", src->type)) goto failed;
    if (!cJSON_AddBoolToObject(root, "enabled", src->enabled)) goto failed;
    if (!cJSON_AddBoolToObject(root, "auto_discover", src->auto_discover)) goto failed;
    if (!cJSON_AddNumberToObject(root, "poll_interval_ms", src->poll_interval_ms)) goto failed;

    switch (src->type) {
        case TS_AUTO_SRC_BUILTIN: {
            cJSON *builtin = cJSON_AddObjectToObject(root, "builtin");
            if (!builtin || !cJSON_AddNumberToObject(builtin, "type", src->builtin.type)) goto failed;
            if (src->builtin.type == TS_AUTO_BUILTIN_GPIO) {
                if (!cJSON_AddNumberToObject(builtin, "pin", src->builtin.gpio.pin)) goto failed;
            } else if (src->builtin.type == TS_AUTO_BUILTIN_ADC) {
                if (!cJSON_AddNumberToObject(builtin, "channel", src->builtin.adc.channel) ||
                    !cJSON_AddNumberToObject(builtin, "atten", src->builtin.adc.atten)) goto failed;
            } else if (src->builtin.type == TS_AUTO_BUILTIN_AGX_STATUS ||
                       src->builtin.type == TS_AUTO_BUILTIN_LPMU_STATUS) {
                if (!cJSON_AddNumberToObject(builtin, "device_index", src->builtin.device.device_index)) goto failed;
            }
            break;
        }
        case TS_AUTO_SRC_WEBSOCKET:
            if (!cJSON_AddStringToObject(root, "ws_uri", src->websocket.uri)) goto failed;
            if (!cJSON_AddStringToObject(root, "ws_path", src->websocket.path)) goto failed;
            if (!cJSON_AddNumberToObject(root, "reconnect_ms", src->websocket.reconnect_ms)) goto failed;
            break;
        case TS_AUTO_SRC_SOCKETIO:
            if (!cJSON_AddStringToObject(root, "sio_url", src->socketio.url)) goto failed;
            if (!cJSON_AddStringToObject(root, "sio_event", src->socketio.event)) goto failed;
            if (!cJSON_AddNumberToObject(root, "reconnect_ms", src->socketio.reconnect_ms)) goto failed;
            break;
        case TS_AUTO_SRC_REST:
            if (!cJSON_AddStringToObject(root, "rest_url", src->rest.url)) goto failed;
            if (!cJSON_AddStringToObject(root, "rest_path", src->rest.path)) goto failed;
            if (!cJSON_AddStringToObject(root, "rest_method", src->rest.method)) goto failed;
            if (!cJSON_AddStringToObject(root, "rest_auth", src->rest.auth_header)) goto failed;
            break;
        case TS_AUTO_SRC_VARIABLE:
            if (!cJSON_AddStringToObject(root, "var_name", src->variable.var_name)) goto failed;
            if (!cJSON_AddStringToObject(root, "var_prefix", src->variable.var_prefix)) goto failed;
            if (!cJSON_AddBoolToObject(root, "var_watch_all", src->variable.watch_all)) goto failed;
            // SSH 命令模式相关字段
            if (src->variable.ssh_host_id[0] != '\0') {
                if (!cJSON_AddStringToObject(root, "ssh_host_id", src->variable.ssh_host_id)) goto failed;
            }
            if (src->variable.ssh_command[0] != '\0') {
                if (!cJSON_AddStringToObject(root, "ssh_command", src->variable.ssh_command)) goto failed;
            }
            if (src->variable.expect_pattern[0] != '\0') {
                if (!cJSON_AddStringToObject(root, "expect_pattern", src->variable.expect_pattern)) goto failed;
            }
            if (src->variable.fail_pattern[0] != '\0') {
                if (!cJSON_AddStringToObject(root, "fail_pattern", src->variable.fail_pattern)) goto failed;
            }
            if (src->variable.extract_pattern[0] != '\0') {
                if (!cJSON_AddStringToObject(root, "extract_pattern", src->variable.extract_pattern)) goto failed;
            }
            if (src->variable.timeout_sec > 0) {
                if (!cJSON_AddNumberToObject(root, "timeout_sec", src->variable.timeout_sec)) goto failed;
            }
            break;
        default:
            break;
    }

    // 序列化映射
    if (src->mapping_count > 0) {
        cJSON *mappings = cJSON_AddArrayToObject(root, "mappings");
        if (!mappings) goto failed;
        for (uint8_t i = 0; i < src->mapping_count && i < TS_AUTO_MAX_MAPPINGS; i++) {
            cJSON *mapping = cJSON_CreateObject();
            if (!mapping) goto failed;
            if (!cJSON_AddItemToArray(mappings, mapping)) { cJSON_Delete(mapping); goto failed; }
            if (!cJSON_AddStringToObject(mapping, "path", src->mappings[i].json_path)) goto failed;
            if (!cJSON_AddStringToObject(mapping, "var", src->mappings[i].var_name)) goto failed;
            if (src->mappings[i].transform[0] != '\0') {
                if (!cJSON_AddStringToObject(mapping, "transform", src->mappings[i].transform)) goto failed;
            }
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
failed:
    cJSON_Delete(root);
    return NULL;
}

esp_err_t ts_source_config_decode(const char *json_str, ts_auto_source_t *src)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return ESP_ERR_INVALID_ARG;
    cJSON *owner = root;
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(format) && !strcmp(format->valuestring, "automation_source"))
        root = cJSON_GetObjectItemCaseSensitive(owner, "source");
    if (!cJSON_IsObject(root)) goto invalid;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        const char *names[] = {"builtin", "websocket", "socketio", "rest", "variable"};
        unsigned kind = 0;
        while (kind < sizeof(names) / sizeof(names[0]) && strcmp(type->valuestring, names[kind])) ++kind;
        if (kind == sizeof(names) / sizeof(names[0])) goto invalid;
        cJSON *number = cJSON_CreateNumber(kind);
        if (!number) goto no_memory;
        if (!cJSON_ReplaceItemInObjectCaseSensitive(root, "type", number)) { cJSON_Delete(number);goto no_memory; }
        const char *fields[4][2] = {{0}};
        if (kind == TS_AUTO_SRC_WEBSOCKET) {
            fields[0][0] = "ws_uri";fields[0][1] = "uri";
            fields[1][0] = "ws_path";fields[1][1] = "json_path";
        } else if (kind == TS_AUTO_SRC_SOCKETIO) {
            fields[0][0] = "sio_url";fields[0][1] = "url";
            fields[1][0] = "sio_event";fields[1][1] = "event";
        } else if (kind == TS_AUTO_SRC_REST) {
            fields[0][0] = "rest_url";fields[0][1] = "url";
            fields[1][0] = "rest_method";fields[1][1] = "method";
            fields[2][0] = "rest_auth";fields[2][1] = "auth_header";
            fields[3][0] = "rest_path";fields[3][1] = "json_path";
        }
        for (unsigned i = 0; i < 4 && fields[i][0]; ++i) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, fields[i][1]);
            if (!value || cJSON_HasObjectItem(root, fields[i][0])) continue;
            cJSON *copy = cJSON_Duplicate(value, true);
            if (!copy) goto no_memory;
            if (!cJSON_AddItemToObject(root, fields[i][0], copy)) { cJSON_Delete(copy);goto no_memory; }
        }
    }

    memset(src, 0, sizeof(ts_auto_source_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "id"))) {
        if (!cJSON_IsString(item) || !copy_string(src->id, sizeof(src->id), item->valuestring)) goto invalid;
    }
    if ((item = cJSON_GetObjectItem(root, "label"))) {
        if (!cJSON_IsString(item) || !copy_string(src->label, sizeof(src->label), item->valuestring)) goto invalid;
    }
    if ((item = cJSON_GetObjectItem(root, "type"))) {
        READ_NUMBER(src->type, TS_AUTO_SRC_BUILTIN, TS_AUTO_SRC_VARIABLE);
    }
    if ((item = cJSON_GetObjectItem(root, "enabled"))) {
        READ_BOOL(src->enabled);
    }
    if ((item = cJSON_GetObjectItem(root, "auto_discover"))) {
        READ_BOOL(src->auto_discover);
    } else {
        src->auto_discover = true;  // 默认启用自动发现（兼容旧配置）
    }
    if ((item = cJSON_GetObjectItem(root, "poll_interval_ms"))) {
        READ_NUMBER(src->poll_interval_ms, 0, UINT32_MAX);
    }

    switch (src->type) {
        case TS_AUTO_SRC_BUILTIN: {
            cJSON *builtin = cJSON_GetObjectItem(root, "builtin");
            if (builtin) {
                if (!cJSON_IsObject(builtin)) goto invalid;
                if ((item = cJSON_GetObjectItem(builtin, "type")))
                    READ_NUMBER(src->builtin.type, 0, TS_AUTO_BUILTIN_UPTIME);
                if (src->builtin.type == TS_AUTO_BUILTIN_GPIO) {
                    if ((item = cJSON_GetObjectItem(builtin, "pin")))
                        READ_NUMBER(src->builtin.gpio.pin, 0, UINT8_MAX);
                } else if (src->builtin.type == TS_AUTO_BUILTIN_ADC) {
                    if ((item = cJSON_GetObjectItem(builtin, "channel")))
                        READ_NUMBER(src->builtin.adc.channel, 0, UINT8_MAX);
                    if ((item = cJSON_GetObjectItem(builtin, "atten")))
                        READ_NUMBER(src->builtin.adc.atten, 0, UINT8_MAX);
                } else if (src->builtin.type == TS_AUTO_BUILTIN_AGX_STATUS ||
                           src->builtin.type == TS_AUTO_BUILTIN_LPMU_STATUS) {
                    if ((item = cJSON_GetObjectItem(builtin, "device_index")))
                        READ_NUMBER(src->builtin.device.device_index, 0, UINT8_MAX);
                }
            }
            break;
        }
        case TS_AUTO_SRC_WEBSOCKET:
            if ((item = cJSON_GetObjectItem(root, "ws_uri"))) {
                if (!cJSON_IsString(item) || !copy_string(src->websocket.uri, sizeof(src->websocket.uri), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "ws_path"))) {
                if (!cJSON_IsString(item) || !copy_string(src->websocket.path, sizeof(src->websocket.path), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "reconnect_ms"))) {
                READ_NUMBER(src->websocket.reconnect_ms, 0, UINT16_MAX);
            }
            break;
        case TS_AUTO_SRC_SOCKETIO:
            if ((item = cJSON_GetObjectItem(root, "sio_url"))) {
                if (!cJSON_IsString(item) || !copy_string(src->socketio.url, sizeof(src->socketio.url), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "sio_event"))) {
                if (!cJSON_IsString(item) || !copy_string(src->socketio.event, sizeof(src->socketio.event), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "reconnect_ms"))) {
                READ_NUMBER(src->socketio.reconnect_ms, 0, UINT16_MAX);
            }
            break;
        case TS_AUTO_SRC_REST:
            if ((item = cJSON_GetObjectItem(root, "rest_url"))) {
                if (!cJSON_IsString(item) || !copy_string(src->rest.url, sizeof(src->rest.url), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "rest_path"))) {
                if (!cJSON_IsString(item) || !copy_string(src->rest.path, sizeof(src->rest.path), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "rest_method"))) {
                if (!cJSON_IsString(item) || !copy_string(src->rest.method, sizeof(src->rest.method), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "rest_auth"))) {
                if (!cJSON_IsString(item) || !copy_string(src->rest.auth_header, sizeof(src->rest.auth_header), item->valuestring)) goto invalid;
            }
            break;
        case TS_AUTO_SRC_VARIABLE:
            if ((item = cJSON_GetObjectItem(root, "var_name"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.var_name, sizeof(src->variable.var_name), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "var_prefix"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.var_prefix, sizeof(src->variable.var_prefix), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "var_watch_all"))) {
                READ_BOOL(src->variable.watch_all);
            }
            // SSH 命令模式相关字段
            if ((item = cJSON_GetObjectItem(root, "ssh_host_id"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.ssh_host_id, sizeof(src->variable.ssh_host_id), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "ssh_command"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.ssh_command, sizeof(src->variable.ssh_command), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "expect_pattern"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.expect_pattern, sizeof(src->variable.expect_pattern), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "fail_pattern"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.fail_pattern, sizeof(src->variable.fail_pattern), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "extract_pattern"))) {
                if (!cJSON_IsString(item) || !copy_string(src->variable.extract_pattern, sizeof(src->variable.extract_pattern), item->valuestring)) goto invalid;
            }
            if ((item = cJSON_GetObjectItem(root, "timeout_sec"))) {
                READ_NUMBER(src->variable.timeout_sec, 0, UINT16_MAX);
            }
            break;
        default:
            break;
    }

    // 反序列化映射
    cJSON *mappings = cJSON_GetObjectItem(root, "mappings");
    if (mappings && !cJSON_IsArray(mappings)) goto invalid;
    if (mappings) {
        int count = cJSON_GetArraySize(mappings);
        if (count > TS_AUTO_MAX_MAPPINGS) goto invalid;
        src->mapping_count = count;

        for (int i = 0; i < src->mapping_count; i++) {
            cJSON *mapping = cJSON_GetArrayItem(mappings, i);
            if (!cJSON_IsObject(mapping)) goto invalid;

            cJSON *path_item = cJSON_GetObjectItem(mapping, "path");
            cJSON *var_item = cJSON_GetObjectItem(mapping, "var");
            if (!path_item) path_item = cJSON_GetObjectItem(mapping, "json_path");
            if (!var_item) var_item = cJSON_GetObjectItem(mapping, "var_name");
            cJSON *transform_item = cJSON_GetObjectItem(mapping, "transform");

            if (!cJSON_IsString(path_item) || !path_item->valuestring[0] ||
                !copy_string(src->mappings[i].json_path, sizeof(src->mappings[i].json_path), path_item->valuestring) ||
                !cJSON_IsString(var_item) || !var_item->valuestring[0] ||
                !copy_string(src->mappings[i].var_name, sizeof(src->mappings[i].var_name), var_item->valuestring)) goto invalid;
            if (transform_item && (!cJSON_IsString(transform_item) ||
                !copy_string(src->mappings[i].transform, sizeof(src->mappings[i].transform), transform_item->valuestring))) goto invalid;
        }
    }

    cJSON_Delete(owner);
    return ESP_OK;
no_memory:
    cJSON_Delete(owner);
    return ESP_ERR_NO_MEM;
invalid:
    cJSON_Delete(owner);
    return ESP_ERR_INVALID_ARG;
}

#undef READ_NUMBER
#undef READ_BOOL
