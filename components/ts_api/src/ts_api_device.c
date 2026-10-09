/**
 * @file ts_api_device.c
 * @brief Device Control API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_log.h"
#include "ts_device_ctrl.h"
#include "ts_fan.h"
#include "ts_power.h"
#include "ts_usb_mux.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"

#define TAG "api_device"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const char *device_state_to_str(ts_device_state_t state)
{
    switch (state) {
        case TS_DEVICE_STATE_OFF:     return "off";
        case TS_DEVICE_STATE_STANDBY: return "standby";
        case TS_DEVICE_STATE_ON:      return "on";
        case TS_DEVICE_STATE_BOOTING: return "booting";
        case TS_DEVICE_STATE_ERROR:   return "error";
        default: return "unknown";
    }
}

static const char *fan_mode_to_str(ts_fan_mode_t mode)
{
    switch (mode) {
        case TS_FAN_MODE_OFF:    return "off";
        case TS_FAN_MODE_MANUAL: return "manual";
        case TS_FAN_MODE_AUTO:   return "auto";
        case TS_FAN_MODE_CURVE:  return "curve";
        default: return "unknown";
    }
}

static const char *fan_auto_state_to_str(ts_fan_auto_state_t state)
{
    switch (state) {
        case TS_FAN_AUTO_STATE_IDLE:     return "idle";
        case TS_FAN_AUTO_STATE_BASELINE: return "baseline";
        case TS_FAN_AUTO_STATE_ACTIVE:   return "active";
        case TS_FAN_AUTO_STATE_GUARD:    return "guard";
        case TS_FAN_AUTO_STATE_STALE:    return "stale";
        default: return "unknown";
    }
}

static const char *usb_target_to_str(ts_usb_mux_target_t target)
{
    switch (target) {
        case TS_USB_MUX_ESP32:      return "esp32";
        case TS_USB_MUX_AGX:        return "agx";
        case TS_USB_MUX_LPMU:       return "lpmu";
        case TS_USB_MUX_DISCONNECT: return "disconnected";
        default: return "unknown";
    }
}

/*===========================================================================*/
/*                          Device Control APIs                               */
/*===========================================================================*/

/**
 * @brief device.status - Get device status
 * @param device: device name ("agx", "lpmu")
 */
static esp_err_t api_device_status(const cJSON *params, ts_api_result_t *result)
{
    ts_device_id_t device_id = TS_DEVICE_AGX;
    
    cJSON *device = cJSON_GetObjectItem(params, "device");
    if (device && cJSON_IsString(device)) {
        if (strcmp(device->valuestring, "lpmu") == 0) {
            device_id = TS_DEVICE_LPMU;
        }
    }
    
    ts_device_status_t status;
    esp_err_t ret = ts_device_get_status(device_id, &status);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get device status");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_id == TS_DEVICE_AGX ? "agx" : "lpmu");
    cJSON_AddStringToObject(data, "state", device_state_to_str(status.state));
    cJSON_AddBoolToObject(data, "power_good", status.power_good);
    cJSON_AddNumberToObject(data, "uptime_ms", status.uptime_ms);
    cJSON_AddNumberToObject(data, "boot_count", status.boot_count);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief device.power - Control device power
 * @param device: device name
 * @param action: "on", "off", "force_off"
 */
static esp_err_t api_device_power(const cJSON *params, ts_api_result_t *result)
{
    cJSON *action = cJSON_GetObjectItem(params, "action");
    if (!action || !cJSON_IsString(action)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'action' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_device_id_t device_id = TS_DEVICE_AGX;
    cJSON *device = cJSON_GetObjectItem(params, "device");
    if (device && cJSON_IsString(device)) {
        if (strcmp(device->valuestring, "lpmu") == 0) {
            device_id = TS_DEVICE_LPMU;
        }
    }
    
    esp_err_t ret = ESP_OK;
    const char *action_str = action->valuestring;
    
    if (strcmp(action_str, "on") == 0) {
        ret = ts_device_power_on(device_id);
    } else if (strcmp(action_str, "off") == 0) {
        ret = ts_device_power_off(device_id);
    } else if (strcmp(action_str, "toggle") == 0) {
        ret = ts_device_power_toggle(device_id);
    } else if (strcmp(action_str, "force_off") == 0) {
        ret = ts_device_force_off(device_id);
    } else {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid action");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Power control failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_id == TS_DEVICE_AGX ? "agx" : "lpmu");
    cJSON_AddStringToObject(data, "action", action_str);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Device %s: %s", device_id == TS_DEVICE_AGX ? "AGX" : "LPMU", action_str);
    return ESP_OK;
}

/**
 * @brief device.reset - Reset device
 * @param device: device name
 */
static esp_err_t api_device_reset(const cJSON *params, ts_api_result_t *result)
{
    ts_device_id_t device_id = TS_DEVICE_AGX;
    cJSON *device = cJSON_GetObjectItem(params, "device");
    if (device && cJSON_IsString(device)) {
        if (strcmp(device->valuestring, "lpmu") == 0) {
            device_id = TS_DEVICE_LPMU;
        }
    }
    
    esp_err_t ret = ts_device_reset(device_id);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Reset failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_id == TS_DEVICE_AGX ? "agx" : "lpmu");
    cJSON_AddBoolToObject(data, "reset", true);
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Device %s reset", device_id == TS_DEVICE_AGX ? "AGX" : "LPMU");
    return ESP_OK;
}

/*===========================================================================*/
/*                          Fan Control APIs                                  */
/*===========================================================================*/

/**
 * @brief device.fan.status - Get fan status
 * @param fan: fan id (0-3) or "all"
 */
static esp_err_t api_device_fan_status(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *fans = cJSON_AddArrayToObject(data, "fans");
    
    cJSON *fan_param = cJSON_GetObjectItem(params, "fan");
    int start = 0, end = TS_FAN_MAX;
    
    if (fan_param && cJSON_IsNumber(fan_param)) {
        int fan_id = (int)cJSON_GetNumberValue(fan_param);
        if (fan_id >= 0 && fan_id < TS_FAN_MAX) {
            start = fan_id;
            end = fan_id + 1;
        }
    }
    
    for (int i = start; i < end; i++) {
        ts_fan_status_t status;
        if (ts_fan_get_status(i, &status) == ESP_OK) {
            cJSON *fan = cJSON_CreateObject();
            cJSON_AddNumberToObject(fan, "id", i);
            cJSON_AddStringToObject(fan, "mode", fan_mode_to_str(status.mode));
            cJSON_AddBoolToObject(fan, "duty_valid", status.duty_valid);
            if (status.duty_valid) cJSON_AddNumberToObject(fan, "duty", status.duty_percent);
            else cJSON_AddNullToObject(fan, "duty");
            cJSON_AddNumberToObject(fan, "rpm", status.rpm);
            cJSON_AddNumberToObject(fan, "temp", status.temp / 10.0);
            if (status.duty_valid) cJSON_AddBoolToObject(fan, "running", status.is_running);
            else cJSON_AddNullToObject(fan, "running");
            cJSON_AddNumberToObject(fan, "control_temperature", status.control_temp / 10.0);
            cJSON_AddNumberToObject(fan, "guard_temperature", status.guard_temp / 10.0);
            cJSON_AddNumberToObject(fan, "predicted_temperature", status.predicted_temp / 10.0);
            cJSON_AddNumberToObject(fan, "slope_c_per_min", status.slope_c_per_min);
            cJSON_AddNumberToObject(fan, "controller_gain", status.controller_gain);
            cJSON_AddNumberToObject(fan, "cooling_response", status.cooling_response);
            cJSON_AddStringToObject(fan, "auto_state", fan_auto_state_to_str(status.auto_state));
            cJSON_AddBoolToObject(fan, "guard_active", status.guard_active);
            cJSON_AddBoolToObject(fan, "temp_stale", status.temp_stale);
            cJSON_AddItemToArray(fans, fan);
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief device.fan.set - Set fan parameters
 * @param fan: fan id
 * @param mode: "off", "manual", "auto", "curve"
 * @param duty: duty cycle (0-100) for manual mode
 */
static esp_err_t api_device_fan_set(const cJSON *params, ts_api_result_t *result)
{
    cJSON *fan_param = cJSON_GetObjectItem(params, "fan");
    if (!fan_param || !cJSON_IsNumber(fan_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'fan' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = (int)cJSON_GetNumberValue(fan_param);
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    
    cJSON *mode = cJSON_GetObjectItem(params, "mode");
    if (mode && cJSON_IsString(mode)) {
        ts_fan_mode_t fan_mode;
        if (strcmp(mode->valuestring, "off") == 0) {
            fan_mode = TS_FAN_MODE_OFF;
        } else if (strcmp(mode->valuestring, "manual") == 0) {
            fan_mode = TS_FAN_MODE_MANUAL;
        } else if (strcmp(mode->valuestring, "auto") == 0) {
            fan_mode = TS_FAN_MODE_AUTO;
        } else if (strcmp(mode->valuestring, "curve") == 0) {
            fan_mode = TS_FAN_MODE_CURVE;
        } else {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid mode");
            return ESP_ERR_INVALID_ARG;
        }
        ret = ts_fan_set_mode(fan_id, fan_mode);
    }
    
    cJSON *duty = cJSON_GetObjectItem(params, "duty");
    if (ret == ESP_OK && duty && cJSON_IsNumber(duty)) {
        int duty_val = (int)cJSON_GetNumberValue(duty);
        if ((!mode || !cJSON_IsString(mode) || strcmp(mode->valuestring, "manual") == 0) &&
            duty_val >= 0 && duty_val <= 100) {
            ret = ts_fan_set_duty(fan_id, duty_val);
        }
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Fan control failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "fan", fan_id);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Power Monitoring APIs                             */
/*===========================================================================*/

static const char *power_rail_names[] = {
    "vin", "5v", "3v3", "12v", "vbat"
};

/**
 * @brief device.power.status - Get power measurements
 * @param rail: rail name or "all"
 */
static esp_err_t api_device_power_status(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *rails = cJSON_AddArrayToObject(data, "rails");
    
    ts_power_data_t power_data[TS_POWER_RAIL_MAX];
    ts_power_read_all(power_data);
    
    for (int i = 0; i < TS_POWER_RAIL_MAX; i++) {
        cJSON *rail = cJSON_CreateObject();
        cJSON_AddStringToObject(rail, "name", power_rail_names[i]);
        cJSON_AddNumberToObject(rail, "voltage_mv", power_data[i].voltage_mv);
        if (power_data[i].current_ma >= 0) {
            cJSON_AddNumberToObject(rail, "current_ma", power_data[i].current_ma);
        }
        if (power_data[i].power_mw >= 0) {
            cJSON_AddNumberToObject(rail, "power_mw", power_data[i].power_mw);
        }
        cJSON_AddItemToArray(rails, rail);
    }
    
    int32_t total_mw;
    if (ts_power_get_total(&total_mw) == ESP_OK && total_mw >= 0) {
        cJSON_AddNumberToObject(data, "total_power_mw", total_mw);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          USB MUX APIs                                      */
/*===========================================================================*/

/**
 * @brief device.usb.status - Get USB MUX status
 */
static esp_err_t api_device_usb_status(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    
    if (!ts_usb_mux_is_configured()) {
        cJSON_AddBoolToObject(data, "configured", false);
    } else {
        cJSON_AddBoolToObject(data, "configured", true);
        cJSON_AddStringToObject(data, "target", usb_target_to_str(ts_usb_mux_get_target()));
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief device.usb.set - Set USB MUX target
 * @param target: "esp32", "agx", "lpmu", "disconnect"
 */
static esp_err_t api_device_usb_set(const cJSON *params, ts_api_result_t *result)
{
    cJSON *target_param = cJSON_GetObjectItem(params, "target");
    
    if (!target_param || !cJSON_IsString(target_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'target' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!ts_usb_mux_is_configured()) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "USB MUX not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_usb_mux_target_t target;
    const char *target_str = target_param->valuestring;
    if (strcmp(target_str, "esp32") == 0) {
        target = TS_USB_MUX_ESP32;
    } else if (strcmp(target_str, "agx") == 0) {
        target = TS_USB_MUX_AGX;
    } else if (strcmp(target_str, "lpmu") == 0) {
        target = TS_USB_MUX_LPMU;
    } else if (strcmp(target_str, "disconnect") == 0) {
        target = TS_USB_MUX_DISCONNECT;
    } else {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid target (use: esp32, agx, lpmu, disconnect)");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_usb_mux_set_target(target);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "USB MUX control failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "target", target_str);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "USB MUX set to %s", target_str);
    return ESP_OK;
}

/**
 * @brief device.ping - Test network connectivity using ICMP ping
 * @param host: IP address (default: "10.10.99.99" for LPMU)
 * @param timeout: timeout in ms (default: 1000)
 */
static esp_err_t api_device_ping(const cJSON *params, ts_api_result_t *result)
{
    const char *host = "10.10.99.99";
    int timeout_ms = 1000;
    
    cJSON *host_json = cJSON_GetObjectItem(params, "host");
    if (host_json && cJSON_IsString(host_json)) {
        host = host_json->valuestring;
    }
    
    cJSON *timeout_json = cJSON_GetObjectItem(params, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json)) {
        timeout_ms = timeout_json->valueint;
    }
    
    // 创建 ICMP raw socket
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "host", host);
        cJSON_AddBoolToObject(data, "reachable", false);
        cJSON_AddStringToObject(data, "error", "socket creation failed");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, host, &addr.sin_addr);
    
    // 构建 ICMP echo request
    struct {
        uint8_t type;
        uint8_t code;
        uint16_t checksum;
        uint16_t id;
        uint16_t seq;
        uint8_t data[32];
    } icmp_pkt;
    
    memset(&icmp_pkt, 0, sizeof(icmp_pkt));
    icmp_pkt.type = 8;  // ICMP_ECHO
    icmp_pkt.code = 0;
    icmp_pkt.id = htons(0x1234);
    icmp_pkt.seq = htons(1);
    memset(icmp_pkt.data, 0xAB, sizeof(icmp_pkt.data));
    
    // 计算校验和
    icmp_pkt.checksum = 0;
    icmp_pkt.checksum = inet_chksum(&icmp_pkt, sizeof(icmp_pkt));
    
    int64_t start_time = esp_timer_get_time();
    
    // 发送 ICMP echo request
    int sent = sendto(sock, &icmp_pkt, sizeof(icmp_pkt), 0, 
                      (struct sockaddr *)&addr, sizeof(addr));
    
    bool reachable = false;
    int latency_ms = 0;
    const char *error_msg = NULL;
    
    if (sent > 0) {
        // 等待 ICMP echo reply
        char recv_buf[64];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        
        int recv_len = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr *)&from, &from_len);
        
        if (recv_len > 0) {
            // IP 头部通常是 20 字节，ICMP 回复在其后
            // 检查是否是 ICMP echo reply (type = 0)
            if (recv_len >= 28) {  // 20 (IP) + 8 (ICMP header)
                uint8_t icmp_type = recv_buf[20];
                if (icmp_type == 0) {  // ICMP_ECHOREPLY
                    reachable = true;
                    latency_ms = (int)((esp_timer_get_time() - start_time) / 1000);
                } else {
                    error_msg = "unexpected icmp type";
                }
            } else {
                error_msg = "short packet";
            }
        } else {
            error_msg = "timeout";
        }
    } else {
        error_msg = "send failed";
    }
    
    close(sock);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "host", host);
    cJSON_AddBoolToObject(data, "reachable", reachable);
    if (reachable) {
        cJSON_AddNumberToObject(data, "latency_ms", latency_ms);
    } else if (error_msg) {
        cJSON_AddStringToObject(data, "error", error_msg);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t device_endpoints[] = {
    {
        .name = "device.status",
        .description = "Get device power status",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_status,
        .requires_auth = false,
    },
    {
        .name = "device.power",
        .description = "Control device power (on/off/force_off)",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_power,
        .requires_auth = true,
        .permission = "device.control",
    },
    {
        .name = "device.reset",
        .description = "Reset device",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_reset,
        .requires_auth = true,
        .permission = "device.control",
    },
    {
        .name = "device.fan.status",
        .description = "Get fan status",
        .category = TS_API_CAT_FAN,
        .handler = api_device_fan_status,
        .requires_auth = false,
    },
    {
        .name = "device.fan.set",
        .description = "Set fan mode and duty",
        .category = TS_API_CAT_FAN,
        .handler = api_device_fan_set,
        .requires_auth = true,
        .permission = "device.control",
    },
    {
        .name = "device.power.status",
        .description = "Get power measurements",
        .category = TS_API_CAT_POWER,
        .handler = api_device_power_status,
        .requires_auth = false,
    },
    {
        .name = "device.usb.status",
        .description = "Get USB MUX status",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_usb_status,
        .requires_auth = false,
    },
    {
        .name = "device.usb.set",
        .description = "Set USB MUX target",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_usb_set,
        .requires_auth = true,
        .permission = "device.control",
    },
    {
        .name = "device.ping",
        .description = "Test network connectivity (TCP port check)",
        .category = TS_API_CAT_DEVICE,
        .handler = api_device_ping,
        .requires_auth = false,
    },
};

esp_err_t ts_api_device_register(void)
{
    TS_LOGI(TAG, "Registering device APIs");
    
    for (size_t i = 0; i < sizeof(device_endpoints) / sizeof(device_endpoints[0]); i++) {
        esp_err_t ret = ts_api_register(&device_endpoints[i]);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to register %s", device_endpoints[i].name);
            return ret;
        }
    }
    
    return ESP_OK;
}
