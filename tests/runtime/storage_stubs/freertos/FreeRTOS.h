#pragma once
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
