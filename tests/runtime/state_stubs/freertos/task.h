#pragma once
#include "freertos/FreeRTOS.h"
typedef struct test_task *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,unsigned);
unsigned ulTaskNotifyTake(int,unsigned);
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelete(TaskHandle_t);
