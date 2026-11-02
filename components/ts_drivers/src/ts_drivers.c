/**
 * @file ts_drivers.c
 * @brief TianShanOS Device Drivers - Main Init
 */

#include "ts_drivers.h"
#include "ts_fan.h"
#include "ts_power.h"
#include "ts_device_ctrl.h"
#include "ts_usb_mux.h"
#include "ts_temp_source.h"
#include "ts_compute_monitor.h"
#include "ts_log.h"
#include "sdkconfig.h"
#include "driver/gpio.h"

#define TAG "ts_drivers"

// Default fan GPIO pin (from pins.json: FAN_PWM_0=41)
#ifndef CONFIG_TS_DRIVERS_FAN0_PWM_GPIO
#define CONFIG_TS_DRIVERS_FAN0_PWM_GPIO 41
#endif

/* GPIO 映射 (from robOS device_controller.h) */
#define GPIO_AGX_POWER          3   // LOW=ON, HIGH=OFF (inverted logic)
#define GPIO_AGX_RESET          1
#define GPIO_AGX_FORCE_RECOVERY 40
#define GPIO_LPMU_POWER         46
#define GPIO_LPMU_RESET         2
#define GPIO_USB_MUX_SEL0       8
#define GPIO_USB_MUX_SEL1       48

esp_err_t ts_drivers_init(void)
{
    esp_err_t ret;
    
    TS_LOGI(TAG, "Initializing device drivers");
    
    /* 1. 初始化温度源管理（供风扇、AGX 监控使用） */
    ret = ts_temp_source_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Temperature source init failed: %s", esp_err_to_name(ret));
        /* 非致命错误，继续 */
    } else {
        TS_LOGI(TAG, "Temperature source manager initialized");
    }
    
    /* 2. GPIO3 测试已完成 - 正式初始化设备驱动 */
    
#ifdef CONFIG_TS_DRIVERS_FAN_ENABLE
    ret = ts_fan_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Fan driver init failed: %s", esp_err_to_name(ret));
    } else {
        // Configure fan 0 only (only one fan GPIO on this board)
        ts_fan_config_t fan0_cfg = {
            .gpio_pwm = CONFIG_TS_DRIVERS_FAN0_PWM_GPIO,
            .gpio_tach = -1,  // No tach for now
            .min_duty = 20,
            .max_duty = 100,
            .curve_points = 0
        };
        ret = ts_fan_configure(TS_FAN_1, &fan0_cfg);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "Fan 0 configure failed: %s", esp_err_to_name(ret));
        } else {
            TS_LOGI(TAG, "Fan 0 configured on GPIO %d", fan0_cfg.gpio_pwm);
        }
    }
#endif

#ifdef CONFIG_TS_DRIVERS_POWER_ENABLE
    ret = ts_power_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Power monitor init failed: %s", esp_err_to_name(ret));
    }
#endif

#ifdef CONFIG_TS_DRIVERS_DEVICE_ENABLE
    ret = ts_device_ctrl_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Device control init failed: %s", esp_err_to_name(ret));
    } else {
        // Configure AGX
        ts_agx_pins_t agx_pins = {
            .gpio_power_en = GPIO_AGX_POWER,
            .gpio_reset = GPIO_AGX_RESET,
            .gpio_force_recovery = GPIO_AGX_FORCE_RECOVERY,
            .gpio_sys_rst = -1,          // Not used on this board
            .gpio_power_good = -1,       // Not used on this board
            .gpio_carrier_pwr_on = -1,   // Not used on this board
            .gpio_shutdown_req = -1,     // Not used on this board
            .gpio_sleep_wake = -1        // Not used on this board
        };
        ret = ts_device_configure_agx(&agx_pins);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "AGX configure failed: %s", esp_err_to_name(ret));
        } else {
            TS_LOGI(TAG, "AGX configured (pwr=%d, rst=%d, rcv=%d)", 
                    GPIO_AGX_POWER, GPIO_AGX_RESET, GPIO_AGX_FORCE_RECOVERY);
        }
        
        // Configure LPMU
        ts_lpmu_pins_t lpmu_pins = {
            .gpio_power_btn = GPIO_LPMU_POWER,
            .gpio_reset = GPIO_LPMU_RESET
        };
        ret = ts_device_configure_lpmu(&lpmu_pins);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "LPMU configure failed: %s", esp_err_to_name(ret));
        } else {
            TS_LOGI(TAG, "LPMU configured (pwr=%d, rst=%d)", 
                    GPIO_LPMU_POWER, GPIO_LPMU_RESET);
        }
    }
#endif

#ifdef CONFIG_TS_DRIVERS_USB_MUX_ENABLE
    ret = ts_usb_mux_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "USB MUX init failed: %s", esp_err_to_name(ret));
    } else {
        // Configure USB MUX
        ts_usb_mux_pins_t mux_pins = {
            .gpio_sel0 = GPIO_USB_MUX_SEL0,
            .gpio_sel1 = GPIO_USB_MUX_SEL1
        };
        ret = ts_usb_mux_configure(&mux_pins);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "USB MUX configure failed: %s", esp_err_to_name(ret));
        } else {
            TS_LOGI(TAG, "USB MUX configured (sel0=%d, sel1=%d)",
                    GPIO_USB_MUX_SEL0, GPIO_USB_MUX_SEL1);
        }
    }
#endif

    /* 初始化算力设备监控（仅初始化，不自动启动；通过 CLI 命令启动） */
    ret = ts_compute_monitor_init(NULL);  /* 使用默认配置 */
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Compute monitor init failed: %s", esp_err_to_name(ret));
    } else {
        TS_LOGI(TAG, "Compute monitor initialized (use 'compute --start' to connect)");
    }

    TS_LOGI(TAG, "Device drivers initialized");
    return ESP_OK;
}

esp_err_t ts_drivers_deinit(void)
{
    /* 停止算力设备监控 */
    esp_err_t ret = ts_compute_monitor_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    
#ifdef CONFIG_TS_DRIVERS_USB_MUX_ENABLE
    ts_usb_mux_deinit();
#endif
#ifdef CONFIG_TS_DRIVERS_DEVICE_ENABLE
    ts_device_ctrl_deinit();
#endif
#ifdef CONFIG_TS_DRIVERS_POWER_ENABLE
    ts_power_deinit();
#endif
#ifdef CONFIG_TS_DRIVERS_FAN_ENABLE
    ts_fan_deinit();
#endif
    
    /* 最后清理温度源 */
    ts_temp_source_deinit();
    
    return ESP_OK;
}
