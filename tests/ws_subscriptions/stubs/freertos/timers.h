#pragma once
#include "platform.h"
typedef struct host_timer *TimerHandle_t;
TimerHandle_t xTimerCreate(const char *,TickType_t,BaseType_t,void *,void (*)(TimerHandle_t));
void *pvTimerGetTimerID(TimerHandle_t);
BaseType_t xTimerStart(TimerHandle_t,TickType_t);
BaseType_t xTimerDelete(TimerHandle_t,TickType_t);
BaseType_t xTimerPendFunctionCall(void (*)(void *,uint32_t),void *,uint32_t,TickType_t);
#ifndef pdFALSE
#define pdFALSE 0
#define pdFAIL 0
#endif
