/**
 * @file ts_led_driver.c
 * @brief WS2812 LED Driver using RMT/SPI
 * 
 * 对于小型设备（≤256 LEDs）使用 RMT 后端
 * 对于大型设备（>256 LEDs）使用 SPI 后端以避免 RMT 内存限制
 */

#include "ts_led_private.h"
#include "ts_led_color_correction.h"
#include "ts_log.h"
#include "led_strip.h"
#include "driver/spi_master.h"
#include <string.h>

#define TAG "led_driver"

esp_err_t ts_led_driver_init(ts_led_device_impl_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    
    int gpio = dev->config.gpio_pin;
    int count = dev->config.led_count;
    
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };
    
    led_strip_handle_t strip = NULL;
    esp_err_t ret;
    
    // 对于大型 LED 设备（超过 256 个），使用 SPI 后端
    // SPI 没有 RMT 内存块限制，更适合大型矩阵
    if (count > 256) {
        TS_LOGI(TAG, "Using SPI backend for %d LEDs on GPIO %d", count, gpio);
        led_strip_spi_config_t spi_config = {
            .clk_src = SPI_CLK_SRC_DEFAULT,
            .spi_bus = SPI2_HOST,
            .flags = {
                .with_dma = true,
            }
        };
        ret = led_strip_new_spi_device(&strip_config, &spi_config, &strip);
    } else {
        // 小型设备使用 RMT
        TS_LOGI(TAG, "Using RMT backend for %d LEDs on GPIO %d", count, gpio);
        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .flags.with_dma = dev->config.use_dma && (count > 64),
            .mem_block_symbols = 64,  // 增加内存块大小
        };
        ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    }
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to create LED strip for '%s': %s", dev->name, esp_err_to_name(ret));
        return ret;
    }
    
    dev->strip_handle = strip;
    led_strip_clear(strip);
    
    TS_LOGI(TAG, "LED driver initialized: '%s' - GPIO %d, %d LEDs", dev->name, gpio, count);
    return ESP_OK;
}

esp_err_t ts_led_driver_send(ts_led_device_impl_t *dev)
{
    if (!dev || !dev->strip_handle) return ESP_ERR_INVALID_STATE;
    
    led_strip_handle_t strip = dev->strip_handle;
    uint8_t brightness = dev->enabled ? dev->brightness : 0;
    
    /* Check if color correction is enabled */
    bool cc_enabled = ts_led_cc_is_enabled();
    
    for (int i = 0; i < dev->config.led_count; i++) {
        ts_led_rgb_t px = dev->framebuffer[i];
        
        /* Apply color correction if enabled */
        if (cc_enabled) {
            ts_led_rgb_t corrected;
            if (ts_led_cc_apply_pixel(&px, &corrected) == ESP_OK) {
                px = corrected;
            }
        }
        
        /* Apply device brightness */
        uint8_t r = (px.r * brightness) >> 8;
        uint8_t g = (px.g * brightness) >> 8;
        uint8_t b = (px.b * brightness) >> 8;
        led_strip_set_pixel(strip, i, r, g, b);
    }
    
    return led_strip_refresh(strip);
}

void ts_led_driver_deinit(ts_led_device_impl_t *dev)
{
    if (dev && dev->strip_handle) {
        led_strip_del(dev->strip_handle);
        dev->strip_handle = NULL;
    }
}
