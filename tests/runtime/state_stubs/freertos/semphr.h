#pragma once
#include "platform.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t *p=malloc(sizeof(*p));pthread_mutex_init(p,NULL);return p;}

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void){pthread_mutex_t *p=malloc(sizeof(*p));pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);pthread_mutex_init(p,&attr);pthread_mutexattr_destroy(&attr);return p;}
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t p,unsigned timeout){return (timeout?pthread_mutex_lock(p):pthread_mutex_trylock(p))==0;}
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
