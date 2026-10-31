/**
 * @file ts_led.c
 * @brief TianShanOS LED Core Implementation
 */

#include "ts_led_private.h"
#include "ts_led_color_correction.h"
#include "ts_log.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

#define TAG "ts_led"

static ts_led_state_t s_led = {0};

ts_led_state_t *ts_led_get_state(void)
{
    return &s_led;
}

static void render_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(1000 / 60);
    uint32_t tick_ms = 0;
    
    while (s_led.render_running) {
        tick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        for (int i = 0; i < CONFIG_TS_LED_MAX_DEVICES; i++) {
            ts_led_device_impl_t *dev = &s_led.devices[i];
            if (!dev->used) continue;
            
            // 处理每个 layer 的特效
            for (int j = 0; j < dev->layer_count; j++) {
                ts_led_layer_impl_t *layer = (ts_led_layer_impl_t *)dev->layers[j];
                if (layer) {
                    xSemaphoreTake(dev->mutex, portMAX_DELAY);
                    if (layer->effect_fn && layer->visible &&
                        tick_ms - layer->effect_last_time >= layer->effect_interval) {
                        layer->effect_fn(layer, tick_ms, layer->effect_data);
                        layer->effect_last_time = tick_ms;
                        layer->dirty = true;
                    }
                    xSemaphoreGive(dev->mutex);
                }
            }
            
            // 合成 layers 到 framebuffer
            if (dev->layer_count > 0) {
                // 清空 framebuffer
                memset(dev->framebuffer, 0, dev->config.led_count * sizeof(ts_led_rgb_t));
                
                // 叠加所有可见 layer
                for (int j = 0; j < dev->layer_count; j++) {
                    ts_led_layer_impl_t *layer = (ts_led_layer_impl_t *)dev->layers[j];
                    if (layer && layer->visible && layer->buffer) {
                        for (uint16_t k = 0; k < dev->config.led_count; k++) {
                            // 简单叠加 (后续可添加混合模式)
                            ts_led_rgb_t *dst = &dev->framebuffer[k];
                            ts_led_rgb_t *src = &layer->buffer[k];
                            if (src->r || src->g || src->b) {
                                dst->r = src->r;
                                dst->g = src->g;
                                dst->b = src->b;
                            }
                        }
                        
                        // 应用后处理效果（如果有）
                        if (layer->post_effect.type != TS_LED_EFFECT_NONE) {
                            ts_led_effect_process(layer, 
                                                  dev->framebuffer,
                                                  dev->config.led_count,
                                                  dev->config.width,
                                                  dev->config.height,
                                                  tick_ms - layer->effect_start_time);
                        }
                    }
                }
            }
            
            ts_led_device_refresh(dev);
        }
        vTaskDelayUntil(&last_wake, interval);
    }
    vTaskDelete(NULL);
}

esp_err_t ts_led_init(void)
{
    if (s_led.initialized) return ESP_ERR_INVALID_STATE;
    
    s_led.mutex = xSemaphoreCreateMutex();
    if (!s_led.mutex) return ESP_ERR_NO_MEM;
    
    memset(s_led.devices, 0, sizeof(s_led.devices));
    s_led.initialized = true;
    
    s_led.render_running = true;
    xTaskCreate(render_task, "led_render", 2560, NULL, 5, &s_led.render_task);
    
    /* 初始化色彩校正子系统 */
    esp_err_t cc_ret = ts_led_cc_init();
    if (cc_ret != ESP_OK) {
        TS_LOGW(TAG, "Color correction init failed: %s (non-fatal)", esp_err_to_name(cc_ret));
    }
    
    TS_LOGI(TAG, "LED subsystem initialized");
    return ESP_OK;
}

bool ts_led_is_initialized(void)
{
    return s_led.initialized;
}

esp_err_t ts_led_deinit(void)
{
    if (!s_led.initialized) return ESP_ERR_INVALID_STATE;
    
    s_led.render_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    
    for (int i = 0; i < CONFIG_TS_LED_MAX_DEVICES; i++) {
        if (s_led.devices[i].used) {
            ts_led_device_destroy(&s_led.devices[i]);
        }
    }
    
    vSemaphoreDelete(s_led.mutex);
    s_led.initialized = false;
    return ESP_OK;
}

esp_err_t ts_led_device_create(const ts_led_config_t *config, ts_led_device_t *device)
{
    if (!config || !device || config->led_count == 0) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_led.mutex, portMAX_DELAY);
    
    ts_led_device_impl_t *dev = NULL;
    for (int i = 0; i < CONFIG_TS_LED_MAX_DEVICES; i++) {
        if (!s_led.devices[i].used) {
            dev = &s_led.devices[i];
            break;
        }
    }
    
    if (!dev) {
        xSemaphoreGive(s_led.mutex);
        return ESP_ERR_NO_MEM;
    }
    
    memset(dev, 0, sizeof(*dev));
    dev->config = *config;
    strncpy(dev->name, config->name ? config->name : "led", TS_LED_MAX_NAME - 1);
    dev->brightness = config->brightness;
    
    /* 优先使用 PSRAM，如果没有则退回 DMA 内存 */
    dev->framebuffer = heap_caps_calloc(config->led_count, sizeof(ts_led_rgb_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dev->framebuffer) {
        /* PSRAM 不可用，使用 DMA 内存 */
        dev->framebuffer = heap_caps_calloc(config->led_count, sizeof(ts_led_rgb_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!dev->framebuffer) {
        xSemaphoreGive(s_led.mutex);
        return ESP_ERR_NO_MEM;
    }
    
    dev->mutex = xSemaphoreCreateMutex();
    esp_err_t ret = ts_led_driver_init(dev);
    if (ret != ESP_OK) {
        free(dev->framebuffer);
        vSemaphoreDelete(dev->mutex);
        xSemaphoreGive(s_led.mutex);
        return ret;
    }
    
    dev->used = true;
    *device = dev;
    
    xSemaphoreGive(s_led.mutex);
    TS_LOGI(TAG, "Created LED device '%s' with %d LEDs", dev->name, config->led_count);
    return ESP_OK;
}

esp_err_t ts_led_device_destroy(ts_led_device_t device)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ts_led_device_impl_t *dev = (ts_led_device_impl_t *)device;
    
    ts_led_driver_deinit(dev);
    free(dev->framebuffer);
    vSemaphoreDelete(dev->mutex);
    memset(dev, 0, sizeof(*dev));
    return ESP_OK;
}

ts_led_device_t ts_led_device_get(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < CONFIG_TS_LED_MAX_DEVICES; i++) {
        if (s_led.devices[i].used && strcmp(s_led.devices[i].name, name) == 0) {
            return &s_led.devices[i];
        }
    }
    return NULL;
}

esp_err_t ts_led_device_set_brightness(ts_led_device_t device, uint8_t brightness)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ((ts_led_device_impl_t *)device)->brightness = brightness;
    return ESP_OK;
}

uint8_t ts_led_device_get_brightness(ts_led_device_t device)
{
    return device ? ((ts_led_device_impl_t *)device)->brightness : 0;
}

uint16_t ts_led_device_get_count(ts_led_device_t device)
{
    return device ? ((ts_led_device_impl_t *)device)->config.led_count : 0;
}

ts_led_layout_t ts_led_device_get_layout(ts_led_device_t device)
{
    return device ? ((ts_led_device_impl_t *)device)->config.layout : TS_LED_LAYOUT_STRIP;
}

esp_err_t ts_led_device_refresh(ts_led_device_t device)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ts_led_device_impl_t *dev = (ts_led_device_impl_t *)device;
    
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    esp_err_t ret = ts_led_driver_send(dev);
    xSemaphoreGive(dev->mutex);
    return ret;
}

esp_err_t ts_led_device_clear(ts_led_device_t device)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ts_led_device_impl_t *dev = (ts_led_device_impl_t *)device;
    
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    memset(dev->framebuffer, 0, dev->config.led_count * sizeof(ts_led_rgb_t));
    xSemaphoreGive(dev->mutex);
    return ESP_OK;
}

esp_err_t ts_led_device_fill(ts_led_device_t device, ts_led_rgb_t color)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ts_led_device_impl_t *dev = (ts_led_device_impl_t *)device;
    
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < dev->config.led_count; i++) {
        dev->framebuffer[i] = color;
    }
    xSemaphoreGive(dev->mutex);
    return ESP_OK;
}

esp_err_t ts_led_device_set_pixel(ts_led_device_t device, uint16_t index, ts_led_rgb_t color)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    ts_led_device_impl_t *dev = (ts_led_device_impl_t *)device;
    
    if (index >= dev->config.led_count) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    dev->framebuffer[index] = color;
    xSemaphoreGive(dev->mutex);
    return ESP_OK;
}
