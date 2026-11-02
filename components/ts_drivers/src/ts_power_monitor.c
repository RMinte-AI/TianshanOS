/**
 * @file ts_power_monitor.c
 * @brief Power Monitor Service Implementation
 * 
 * 移植自 robOS power_monitor 组件，提供：
 * - ADC 供电电压监控（GPIO18, ADC2_CH7, 分压比 11.4:1）
 * - UART 电源芯片数据接收（GPIO47, 9600 8N1, [0xFF][V][I][CRC]）
 * - 后台任务持续监控
 * - 阈值报警和事件回调
 */

#include "ts_power_monitor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdatomic.h>
#include <math.h>

static const char *TAG = "ts_power_monitor";

/*===========================================================================*/
/*                          Hardware Configuration                            */
/*===========================================================================*/

/* ADC 配置 - GPIO18 -> ADC2_CHANNEL_7 */
#define POWER_ADC_UNIT          ADC_UNIT_2
#define POWER_ADC_CHANNEL       ADC_CHANNEL_7
#define POWER_ADC_GPIO          18

/* UART 配置 - GPIO47 -> UART1_RX */
#define POWER_UART_NUM          UART_NUM_1
#define POWER_UART_RX_GPIO      47
#define POWER_UART_BAUD_RATE    9600
#define POWER_UART_BUF_SIZE     256

/*===========================================================================*/
/*                          Internal State                                    */
/*===========================================================================*/

typedef struct {
    bool initialized;
    atomic_bool running;
    atomic_bool control_busy;
    SemaphoreHandle_t exit_done;
    ts_power_monitor_config_t config;
    
    /* ADC */
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t adc_cali_handle;
    ts_power_voltage_data_t latest_voltage;
    float last_supply_voltage;
    float voltage_threshold;
    
    /* UART */
    ts_power_chip_data_t latest_power_data;
    QueueHandle_t uart_queue;
    
    /* Task */
    TaskHandle_t monitor_task_handle;
    
    /* Synchronization */
    SemaphoreHandle_t data_mutex;
    
    /* Statistics */
    ts_power_monitor_stats_t stats;
    uint64_t start_time_us;
    
    /* Callback */
    ts_power_event_callback_t callback;
    void *callback_user_data;
    
} power_monitor_state_t;

static power_monitor_state_t s_pm = {0};

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static void power_monitor_task(void *pvParameters);
static esp_err_t voltage_monitor_init(void);
static esp_err_t power_chip_init(void);
static esp_err_t read_voltage_sample(ts_power_voltage_data_t *data);
static esp_err_t read_power_chip_packet(ts_power_chip_data_t *data);
static void update_statistics(void);
static void trigger_event(ts_power_event_type_t event_type, void *event_data);

/*===========================================================================*/
/*                          Default Configuration                             */
/*===========================================================================*/

esp_err_t ts_power_monitor_get_default_config(ts_power_monitor_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(config, 0, sizeof(ts_power_monitor_config_t));
    
    /* 电压监控默认配置 */
    config->voltage_config.gpio_pin = POWER_ADC_GPIO;
    config->voltage_config.divider_ratio = TS_POWER_VOLTAGE_DIVIDER_RATIO;
    config->voltage_config.sample_interval_ms = 5000;  /* 5秒采样一次 */
    config->voltage_config.voltage_min_threshold = 10.0f;  /* 最小 10V */
    config->voltage_config.voltage_max_threshold = 30.0f;  /* 最大 30V */
    config->voltage_config.enable_threshold_alarm = true;
    
    /* 电源芯片通信默认配置 */
    config->power_chip_config.uart_num = POWER_UART_NUM;
    config->power_chip_config.rx_gpio_pin = POWER_UART_RX_GPIO;
    config->power_chip_config.baud_rate = POWER_UART_BAUD_RATE;
    config->power_chip_config.timeout_ms = 1000;
    config->power_chip_config.enable_protocol_debug = false;
    
    /* 任务配置 */
    config->auto_start_monitoring = true;
    config->task_stack_size = 4096;
    config->task_priority = 5;
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

esp_err_t ts_power_monitor_init(const ts_power_monitor_config_t *config)
{
    if (s_pm.initialized) {
        ESP_LOGW(TAG, "Power monitor already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing power monitor v%s", TS_POWER_MONITOR_VERSION);
    
    /* 使用提供的配置或默认配置 */
    if (config != NULL) {
        memcpy(&s_pm.config, config, sizeof(ts_power_monitor_config_t));
    } else {
        ts_power_monitor_get_default_config(&s_pm.config);
    }
    
    /* 创建互斥锁 */
    s_pm.data_mutex = xSemaphoreCreateMutex();
    if (s_pm.data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create data mutex");
        return ESP_ERR_NO_MEM;
    }
    
    s_pm.exit_done = xSemaphoreCreateBinary();
    if (!s_pm.exit_done) {
        vSemaphoreDelete(s_pm.data_mutex);
        s_pm.data_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 初始化电压监控 (ADC) */
    esp_err_t ret = voltage_monitor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize voltage monitor: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    
    /* 初始化电源芯片通信 (UART) */
    ret = power_chip_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize power chip communication: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    
    /* 初始化统计和启动时间 */
    s_pm.start_time_us = esp_timer_get_time();
    memset(&s_pm.stats, 0, sizeof(ts_power_monitor_stats_t));
    s_pm.voltage_threshold = 1.0f;  /* 默认 1V 变化阈值 */
    
    s_pm.initialized = true;
    
    /* 自动启动监控 */
    if (s_pm.config.auto_start_monitoring) {
        ret = ts_power_monitor_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to auto-start monitoring: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "Power monitor initialized successfully");
    return ESP_OK;
    
cleanup:
    if (s_pm.exit_done) {
        vSemaphoreDelete(s_pm.exit_done);
        s_pm.exit_done = NULL;
    }
    if (s_pm.data_mutex) {
        vSemaphoreDelete(s_pm.data_mutex);
        s_pm.data_mutex = NULL;
    }
    return ret;
}

static esp_err_t stop_pm_task(void);

esp_err_t ts_power_monitor_deinit(void)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Deinitializing power monitor");
    
    if (atomic_exchange(&s_pm.control_busy, true)) return ESP_ERR_NOT_FINISHED;
    esp_err_t ret = stop_pm_task();
    if (ret != ESP_OK) {
        atomic_store(&s_pm.control_busy, false);
        return ret;
    }
    vSemaphoreDelete(s_pm.exit_done);
    s_pm.exit_done = NULL;
    
    /* 清理 ADC 校准 */
    if (s_pm.adc_cali_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(s_pm.adc_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(s_pm.adc_cali_handle);
#endif
        s_pm.adc_cali_handle = NULL;
    }
    
    /* 清理 ADC */
    if (s_pm.adc_handle) {
        adc_oneshot_del_unit(s_pm.adc_handle);
        s_pm.adc_handle = NULL;
    }
    
    /* 清理 UART */
    uart_driver_delete(s_pm.config.power_chip_config.uart_num);
    
    /* 清理互斥锁 */
    if (s_pm.data_mutex) {
        vSemaphoreDelete(s_pm.data_mutex);
        s_pm.data_mutex = NULL;
    }
    
    /* 清理队列 */
    if (s_pm.uart_queue) {
        vQueueDelete(s_pm.uart_queue);
        s_pm.uart_queue = NULL;
    }
    
    s_pm.initialized = false;
    s_pm.running = false;
    atomic_store(&s_pm.control_busy, false);
    
    ESP_LOGI(TAG, "Power monitor deinitialized");
    return ESP_OK;
}

/*===========================================================================*/
/*                          ADC Voltage Monitor                               */
/*===========================================================================*/

static esp_err_t voltage_monitor_init(void)
{
    ESP_LOGI(TAG, "Initializing voltage monitor on GPIO %d (ADC2_CH%d)",
             s_pm.config.voltage_config.gpio_pin, POWER_ADC_CHANNEL);
    
    /* 配置 ADC 单元 */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = POWER_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &s_pm.adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 配置 ADC 通道 - 使用 11dB 衰减以支持更高电压范围 */
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,  /* 0-3.3V 范围 */
    };
    
    ret = adc_oneshot_config_channel(s_pm.adc_handle, POWER_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 初始化 ADC 校准 */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = POWER_ADC_UNIT,
        .chan = POWER_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &s_pm.adc_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = POWER_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &s_pm.adc_cali_handle);
#else
    ret = ESP_ERR_NOT_SUPPORTED;
#endif

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration initialized");
    } else {
        ESP_LOGW(TAG, "ADC calibration failed: %s, using linear conversion", esp_err_to_name(ret));
        s_pm.adc_cali_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Voltage monitor initialized (divider ratio: %.1f:1)", 
             s_pm.config.voltage_config.divider_ratio);
    return ESP_OK;
}

static esp_err_t read_voltage_sample(ts_power_voltage_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_pm.initialized || s_pm.adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int raw_adc;
    int voltage_mv;
    
    /* 读取 ADC 原始值 */
    esp_err_t ret = adc_oneshot_read(s_pm.adc_handle, POWER_ADC_CHANNEL, &raw_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ADC: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 校准转换为电压 */
    if (s_pm.adc_cali_handle != NULL) {
        ret = adc_cali_raw_to_voltage(s_pm.adc_cali_handle, raw_adc, &voltage_mv);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to calibrate voltage: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* 无校准时使用线性转换 */
        voltage_mv = (raw_adc * TS_POWER_ADC_REF_VOLTAGE_MV) / TS_POWER_ADC_MAX_VALUE;
    }
    
    /* 根据分压比计算实际电压 */
    float actual_voltage = (voltage_mv / 1000.0f) * s_pm.config.voltage_config.divider_ratio;
    
    data->supply_voltage = actual_voltage;
    data->raw_adc = raw_adc;
    data->voltage_mv = voltage_mv;
    data->timestamp = esp_log_timestamp();
    
    ESP_LOGD(TAG, "Voltage: raw=%d, mv=%d, actual=%.2fV", raw_adc, voltage_mv, actual_voltage);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          UART Power Chip Communication                     */
/*===========================================================================*/

static esp_err_t power_chip_init(void)
{
    ESP_LOGI(TAG, "Initializing power chip communication on GPIO %d (UART%d)",
             s_pm.config.power_chip_config.rx_gpio_pin,
             s_pm.config.power_chip_config.uart_num);
    
    /* 配置 UART */
    uart_config_t uart_config = {
        .baud_rate = s_pm.config.power_chip_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    int uart_num = s_pm.config.power_chip_config.uart_num;
    
    esp_err_t ret = uart_driver_install(uart_num, POWER_UART_BUF_SIZE, POWER_UART_BUF_SIZE, 
                                        10, &s_pm.uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_param_config(uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 只配置 RX 引脚（电源芯片是单向发送数据） */
    ret = uart_set_pin(uart_num, UART_PIN_NO_CHANGE, 
                       s_pm.config.power_chip_config.rx_gpio_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Power chip communication initialized (baud: %d)", 
             s_pm.config.power_chip_config.baud_rate);
    return ESP_OK;
}

static esp_err_t read_power_chip_packet(ts_power_chip_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int uart_num = s_pm.config.power_chip_config.uart_num;
    uint8_t buffer[TS_POWER_CHIP_PACKET_SIZE];
    
    /* 检查可用数据 */
    size_t uart_length = 0;
    esp_err_t err = uart_get_buffered_data_len(uart_num, &uart_length);
    if (err != ESP_OK || uart_length < TS_POWER_CHIP_PACKET_SIZE) {
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 读取最多 8 字节来查找有效的 4 字节数据包 */
    const int max_read_size = 8;
    uint8_t read_buffer[8];
    
    int bytes_read = uart_read_bytes(uart_num, read_buffer, max_read_size, pdMS_TO_TICKS(100));
    if (bytes_read <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 查找 0xFF 帧头 */
    int packet_start = -1;
    for (int i = 0; i <= bytes_read - TS_POWER_CHIP_PACKET_SIZE; i++) {
        if (read_buffer[i] == TS_POWER_CHIP_HEADER) {
            packet_start = i;
            break;
        }
    }
    
    if (packet_start == -1) {
        ESP_LOGD(TAG, "No valid packet header found in %d bytes", bytes_read);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    /* 检查是否有完整的数据包 */
    if (packet_start + TS_POWER_CHIP_PACKET_SIZE > bytes_read) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    /* 复制有效的 4 字节数据包 */
    memcpy(buffer, &read_buffer[packet_start], TS_POWER_CHIP_PACKET_SIZE);
    
    /* 解析数据包: [0xFF header][voltage][current][CRC] */
    uint8_t voltage_raw = buffer[1];
    uint8_t current_raw = buffer[2];
    uint8_t received_crc = buffer[3];
    
    /* CRC-8/MAXIM: init=0, LSB first, reflected polynomial 0x8C.
     * Covers the header, voltage and current bytes. FF 1C 32 -> 71. */
    uint8_t calculated_crc = 0;
    for (size_t i = 0; i < TS_POWER_CHIP_PACKET_SIZE - 1; i++) {
        calculated_crc ^= buffer[i];
        for (int bit = 0; bit < 8; bit++) {
            calculated_crc = (calculated_crc & 1)
                ? (calculated_crc >> 1) ^ 0x8C : calculated_crc >> 1;
        }
    }
    
    bool crc_valid = (calculated_crc == received_crc);
    
    if (!crc_valid) {
        s_pm.stats.crc_errors++;
        /* 继续解析，但标记 CRC 错误 */
    }
    
    /* 转换原始数据为实际值（根据实际硬件校准） */
    float voltage = voltage_raw * 1.0f;   /* 1.0V per unit */
    float current = current_raw * 0.1f;   /* 0.1A per unit */
    float power = voltage * current;
    
    /* 填充数据结构 */
    data->voltage = voltage;
    data->current = current;
    data->power = power;
    data->valid = crc_valid;
    data->crc_valid = crc_valid;
    data->timestamp = esp_log_timestamp();
    memcpy(data->raw_data, buffer, TS_POWER_CHIP_PACKET_SIZE);
    
    if (s_pm.config.power_chip_config.enable_protocol_debug) {
        ESP_LOGI(TAG, "Power chip: V=%.1fV, I=%.2fA, P=%.2fW, CRC=%s [raw: 0x%02X 0x%02X 0x%02X 0x%02X]",
                 voltage, current, power, crc_valid ? "OK" : "FAIL",
                 buffer[0], buffer[1], buffer[2], buffer[3]);
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Monitoring Task                                   */
/*===========================================================================*/

esp_err_t ts_power_monitor_start(void)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (atomic_exchange(&s_pm.control_busy, true)) return ESP_ERR_NOT_FINISHED;

    if (s_pm.running) {
        ESP_LOGW(TAG, "Power monitor already running");
        atomic_store(&s_pm.control_busy, false);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting power monitor");
    
    xSemaphoreTake(s_pm.exit_done, 0);
    s_pm.running = true;
    
    /* 创建监控任务 */
    BaseType_t ret = xTaskCreate(power_monitor_task, "power_monitor",
                                 s_pm.config.task_stack_size, NULL,
                                 s_pm.config.task_priority,
                                 &s_pm.monitor_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitoring task");
        s_pm.running = false;
        s_pm.monitor_task_handle = NULL;
        atomic_store(&s_pm.control_busy, false);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Power monitor started");
    atomic_store(&s_pm.control_busy, false);
    return ESP_OK;
}

static esp_err_t stop_pm_task(void)
{
    if (s_pm.monitor_task_handle == xTaskGetCurrentTaskHandle()) return ESP_ERR_NOT_FINISHED;
    s_pm.running = false;
    if (s_pm.monitor_task_handle) {
        xSemaphoreTake(s_pm.exit_done, portMAX_DELAY);
        s_pm.monitor_task_handle = NULL;
    }
    return ESP_OK;
}

esp_err_t ts_power_monitor_stop(void)
{
    if (!s_pm.initialized) return ESP_ERR_INVALID_STATE;
    if (atomic_exchange(&s_pm.control_busy, true)) return ESP_ERR_NOT_FINISHED;
    esp_err_t ret = stop_pm_task();
    atomic_store(&s_pm.control_busy, false);
    return ret;
}

bool ts_power_monitor_is_initialized(void)
{
    return s_pm.initialized;
}

bool ts_power_monitor_is_running(void)
{
    return s_pm.initialized && s_pm.running;
}

static void power_monitor_task(void *pvParameters)
{
    ts_power_voltage_data_t voltage_data;
    ts_power_chip_data_t power_data;
    TickType_t last_voltage_time = 0;
    
    ESP_LOGI(TAG, "Power monitor task started");
    
    while (s_pm.running) {
        TickType_t current_time = xTaskGetTickCount();
        
        /* 按配置间隔读取电压 */
        if (current_time - last_voltage_time >= 
            pdMS_TO_TICKS(s_pm.config.voltage_config.sample_interval_ms)) {
            
            if (read_voltage_sample(&voltage_data) == ESP_OK) {
                if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    memcpy(&s_pm.latest_voltage, &voltage_data, sizeof(ts_power_voltage_data_t));
                    s_pm.stats.voltage_samples++;
                    
                    /* 更新平均电压 */
                    s_pm.stats.avg_voltage = 
                        (s_pm.stats.avg_voltage * (s_pm.stats.voltage_samples - 1) + 
                         voltage_data.supply_voltage) / s_pm.stats.voltage_samples;
                    
                    xSemaphoreGive(s_pm.data_mutex);
                }
                
                /* 检查阈值 */
                if (s_pm.config.voltage_config.enable_threshold_alarm) {
                    if (voltage_data.supply_voltage < s_pm.config.voltage_config.voltage_min_threshold ||
                        voltage_data.supply_voltage > s_pm.config.voltage_config.voltage_max_threshold) {
                        s_pm.stats.threshold_violations++;
                        trigger_event(TS_POWER_EVENT_VOLTAGE_THRESHOLD, &voltage_data);
                    }
                }
                
                /* 检查电压显著变化 */
                if (s_pm.last_supply_voltage > 0 &&
                    fabsf(voltage_data.supply_voltage - s_pm.last_supply_voltage) > s_pm.voltage_threshold) {
                    ESP_LOGD(TAG, "Voltage changed: %.2fV -> %.2fV",
                             s_pm.last_supply_voltage, voltage_data.supply_voltage);
                }
                s_pm.last_supply_voltage = voltage_data.supply_voltage;
            }
            last_voltage_time = current_time;
        }
        
        /* 检查电源芯片数据 */
        if (read_power_chip_packet(&power_data) == ESP_OK) {
            if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(&s_pm.latest_power_data, &power_data, sizeof(ts_power_chip_data_t));
                s_pm.stats.power_chip_packets++;
                
                if (power_data.crc_valid) {
                    /* 更新平均值 */
                    s_pm.stats.avg_current = 
                        (s_pm.stats.avg_current * (s_pm.stats.power_chip_packets - 1) + 
                         power_data.current) / s_pm.stats.power_chip_packets;
                    
                    s_pm.stats.avg_power = 
                        (s_pm.stats.avg_power * (s_pm.stats.power_chip_packets - 1) + 
                         power_data.power) / s_pm.stats.power_chip_packets;
                } else {
                    trigger_event(TS_POWER_EVENT_CRC_ERROR, &power_data);
                }
                
                xSemaphoreGive(s_pm.data_mutex);
            }
            
            trigger_event(TS_POWER_EVENT_POWER_DATA_RECEIVED, &power_data);
        }
        
        /* 更新统计 */
        update_statistics();
        
        /* 防止任务占用过多 CPU */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "Power monitor task ended");
    xSemaphoreGive(s_pm.exit_done);
    vTaskDelete(NULL);
}

/*===========================================================================*/
/*                          Statistics and Events                             */
/*===========================================================================*/

static void update_statistics(void)
{
    if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_pm.stats.uptime_ms = (esp_timer_get_time() - s_pm.start_time_us) / 1000;
        xSemaphoreGive(s_pm.data_mutex);
    }
}

static void trigger_event(ts_power_event_type_t event_type, void *event_data)
{
    if (s_pm.callback) {
        s_pm.callback(event_type, event_data, s_pm.callback_user_data);
    }
}

/*===========================================================================*/
/*                          Public API                                        */
/*===========================================================================*/

esp_err_t ts_power_monitor_get_voltage_data(ts_power_voltage_data_t *data)
{
    if (!s_pm.initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(data, &s_pm.latest_voltage, sizeof(ts_power_voltage_data_t));
        xSemaphoreGive(s_pm.data_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_monitor_read_voltage_now(ts_power_voltage_data_t *data)
{
    if (!s_pm.initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 直接读取 ADC */
    esp_err_t ret = read_voltage_sample(data);
    if (ret == ESP_OK) {
        /* 更新缓存 */
        if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&s_pm.latest_voltage, data, sizeof(ts_power_voltage_data_t));
            xSemaphoreGive(s_pm.data_mutex);
        }
    }
    
    return ret;
}

esp_err_t ts_power_monitor_get_power_chip_data(ts_power_chip_data_t *data)
{
    if (!s_pm.initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(data, &s_pm.latest_power_data, sizeof(ts_power_chip_data_t));
        xSemaphoreGive(s_pm.data_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_monitor_get_stats(ts_power_monitor_stats_t *stats)
{
    if (!s_pm.initialized || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(stats, &s_pm.stats, sizeof(ts_power_monitor_stats_t));
        xSemaphoreGive(s_pm.data_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_monitor_reset_stats(void)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_pm.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&s_pm.stats, 0, sizeof(ts_power_monitor_stats_t));
        s_pm.start_time_us = esp_timer_get_time();
        xSemaphoreGive(s_pm.data_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_monitor_set_voltage_thresholds(float min_voltage, float max_voltage)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (min_voltage >= max_voltage || min_voltage < 0 || max_voltage < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_pm.config.voltage_config.voltage_min_threshold = min_voltage;
    s_pm.config.voltage_config.voltage_max_threshold = max_voltage;
    
    ESP_LOGI(TAG, "Voltage thresholds set: %.2fV - %.2fV", min_voltage, max_voltage);
    return ESP_OK;
}

esp_err_t ts_power_monitor_get_voltage_thresholds(float *min_voltage, float *max_voltage)
{
    if (!s_pm.initialized || min_voltage == NULL || max_voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *min_voltage = s_pm.config.voltage_config.voltage_min_threshold;
    *max_voltage = s_pm.config.voltage_config.voltage_max_threshold;
    
    return ESP_OK;
}

esp_err_t ts_power_monitor_set_sample_interval(uint32_t interval_ms)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (interval_ms < 100 || interval_ms > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_pm.config.voltage_config.sample_interval_ms = interval_ms;
    ESP_LOGI(TAG, "Sample interval set to %lu ms", (unsigned long)interval_ms);
    
    return ESP_OK;
}

esp_err_t ts_power_monitor_get_sample_interval(uint32_t *interval_ms)
{
    if (!s_pm.initialized || interval_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *interval_ms = s_pm.config.voltage_config.sample_interval_ms;
    return ESP_OK;
}

esp_err_t ts_power_monitor_register_callback(ts_power_event_callback_t callback, void *user_data)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_pm.callback = callback;
    s_pm.callback_user_data = user_data;
    
    ESP_LOGI(TAG, "Event callback registered");
    return ESP_OK;
}

esp_err_t ts_power_monitor_unregister_callback(void)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_pm.callback = NULL;
    s_pm.callback_user_data = NULL;
    
    ESP_LOGI(TAG, "Event callback unregistered");
    return ESP_OK;
}

esp_err_t ts_power_monitor_set_debug_mode(bool enable)
{
    if (!s_pm.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_pm.config.power_chip_config.enable_protocol_debug = enable;
    ESP_LOGI(TAG, "Protocol debug %s", enable ? "enabled" : "disabled");
    
    return ESP_OK;
}
