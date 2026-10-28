#pragma once
#include "ts_source_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define TS_SOURCE_MAX_SIO_CONNECTIONS 4

/* The source table is copied under this mutex. Network work owns a copied
 * definition and may publish only while its instance is still current. */
typedef struct {
    ts_auto_source_t *sources;
    int count, capacity;
    SemaphoreHandle_t mutex;
    bool initialized, running, closing;
    unsigned active_polls;
    ts_source_manager_stats_t stats;
    uint64_t next_instance;
} ts_source_runtime_t;
extern ts_source_runtime_t ts_source_runtime;
esp_err_t ts_source_runtime_init(void);
void ts_source_runtime_deinit(void);
void ts_source_transport_changed(void);
