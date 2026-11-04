/* Host model of the ordinary FreeRTOS mutex; shared by source-extracted suites. */
#include <pthread.h>
#include <errno.h>
#define portMAX_DELAY (-1)
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static StaticSemaphore_t s_fan_mutex_storage[TS_FAN_MAX] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
};
static SemaphoreHandle_t s_fan_mutex[TS_FAN_MAX] = {
    &s_fan_mutex_storage[0], &s_fan_mutex_storage[1],
    &s_fan_mutex_storage[2], &s_fan_mutex_storage[3]
};
static _Thread_local unsigned host_locks_held;
static void (*host_lock_waiting)(void);
static int xSemaphoreTake(SemaphoreHandle_t mutex, int timeout) {
    assert(!host_locks_held); /* No recursive public setters or nested fan locks. */
    int ret = pthread_mutex_trylock(mutex);
    if (ret == EBUSY) {
        if (host_lock_waiting) host_lock_waiting();
        ret = pthread_mutex_lock(mutex);
    }
    assert(ret == 0);
    ++host_locks_held;
    return 1;
}
static int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(host_locks_held == 1);
    --host_locks_held;
    assert(pthread_mutex_unlock(mutex) == 0);
    return 1;
}
