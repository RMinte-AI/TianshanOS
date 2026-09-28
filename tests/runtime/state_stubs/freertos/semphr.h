#pragma once
#include "platform.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t *p=malloc(sizeof(*p));pthread_mutex_init(p,NULL);return p;}
