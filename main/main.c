/**
 * @file main.c
 * @brief TianShanOS Main Entry Point
 *
 * TianShanOS 主程序入口
 *
 * @author TianShanOS Team
 * @version 0.2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_private/startup_internal.h"
#include "ts_core.h"
#include "ts_services.h"

static const char *TAG = "main";

/*
 * 超早期 AGX GPIO 初始化 - 使用 ESP_SYSTEM_INIT_FN
 * 
 * 这会在 esp_system_init 阶段执行，比 constructor 更早
 * 在 SECONDARY 阶段以最高优先级 (100) 执行
 */
ESP_SYSTEM_INIT_FN(agx_gpio_early_init, SECONDARY, BIT(0), 100)
{
    /*
     * 直接操作 GPIO 寄存器，避免函数调用开销
     * GPIO3: AGX_FORCE_SHUTDOWN - 必须为 LOW 允许 AGX 启动
     * GPIO1: AGX_RESET - 必须为 LOW 让 AGX 正常运行
     */
    
    /* GPIO3 = LOW (允许 AGX 开机) */
    gpio_ll_output_enable(&GPIO, GPIO_NUM_3);
    gpio_ll_set_level(&GPIO, GPIO_NUM_3, 0);
    gpio_ll_pulldown_en(&GPIO, GPIO_NUM_3);
    gpio_ll_pullup_dis(&GPIO, GPIO_NUM_3);
    
    /* GPIO1 = LOW (正常运行) */
    gpio_ll_output_enable(&GPIO, GPIO_NUM_1);
    gpio_ll_set_level(&GPIO, GPIO_NUM_1, 0);
    gpio_ll_pulldown_en(&GPIO, GPIO_NUM_1);
    gpio_ll_pullup_dis(&GPIO, GPIO_NUM_1);
    
    return ESP_OK;
}

/*
 * Constructor 作为备份 - 以最高优先级执行
 * 优先级越小越先执行，101 是最早的合法用户优先级
 */
__attribute__((constructor(101)))
static void early_critical_gpio_init(void)
{
    /*
     * 再次确保 AGX GPIO 处于正确状态
     * 这里使用完整的 gpio_config 以确保配置完整
     */
    
    /* GPIO3: AGX_FORCE_SHUTDOWN */
    gpio_set_level(GPIO_NUM_3, 0);  /* 先设置电平 */
    gpio_config_t io_conf_gpio3 = {
        .pin_bit_mask = (1ULL << GPIO_NUM_3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_gpio3);
    
    /* GPIO1: AGX_RESET */
    gpio_set_level(GPIO_NUM_1, 0);  /* 先设置电平 */
    gpio_config_t io_conf_gpio1 = {
        .pin_bit_mask = (1ULL << GPIO_NUM_1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_gpio1);
}

/**
 * @brief 打印启动横幅
 */
static void print_banner(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                                                                        ║\n");
    printf("║   ████████╗██╗ █████╗ ███╗   ██╗███████╗██╗  ██╗ █████╗ ███╗   ██╗     ║\n");
    printf("║   ╚══██╔══╝██║██╔══██╗████╗  ██║██╔════╝██║  ██║██╔══██╗████╗  ██║     ║\n");
    printf("║      ██║   ██║███████║██╔██╗ ██║███████╗███████║███████║██╔██╗ ██║     ║\n");
    printf("║      ██║   ██║██╔══██║██║╚██╗██║╚════██║██╔══██║██╔══██║██║╚██╗██║     ║\n");
    printf("║      ██║   ██║██║  ██║██║ ╚████║███████║██║  ██║██║  ██║██║ ╚████║     ║\n");
    printf("║      ╚═╝   ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝     ║\n");
    printf("║                                                                        ║\n");
    printf("║                   TianShanOS %-16s                   ║\n", TIANSHAN_OS_VERSION_STRING);
    printf("║                     ESP32 Rack Management Operating System             ║\n");
    printf("║                                                                        ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/**
 * @brief 打印系统信息
 */
static void print_system_info(void)
{
    ESP_LOGI(TAG, "System Information:");
    ESP_LOGI(TAG, "  - IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "  - Free Heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  - Min Free Heap: %lu bytes", (unsigned long)esp_get_minimum_free_heap_size());

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "  - Chip: %s, %d cores, WiFi%s%s",
             (chip_info.model == CHIP_ESP32S3) ? "ESP32-S3" :
             (chip_info.model == CHIP_ESP32) ? "ESP32" : "Unknown",
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
}

/**
 * @brief 应用程序主入口
 */
void app_main(void)
{
    esp_err_t ret;

    // 打印启动横幅
    print_banner();

    // 打印系统信息
    print_system_info();

    // 初始化 TianShanOS 核心
    ESP_LOGI(TAG, "Initializing TianShanOS...");
    ret = ts_core_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TianShanOS core: %s", esp_err_to_name(ret));
        return;
    }

    ret = ts_services_prepare_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configuration migration failed: %s", esp_err_to_name(ret));
        return;
    }

    // 注册所有核心服务
    ESP_LOGI(TAG, "Registering core services...");
    ret = ts_services_register_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register services: %s", esp_err_to_name(ret));
        return;
    }

    // 启动所有服务
    ESP_LOGI(TAG, "Starting TianShanOS services...");
    ret = ts_core_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TianShanOS: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "TianShanOS started successfully!");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);

    // 打印服务状态
    ts_service_dump();

    // 主循环
    ESP_LOGI(TAG, "Entering main loop...");

    while (1) {
        // 定期打印堆信息（调试用）
        vTaskDelay(pdMS_TO_TICKS(60000));  // 每分钟

#ifdef CONFIG_TS_LOG_DEBUG
        ESP_LOGI(TAG, "Heap: free=%lu, min=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());
#endif
    }
}
