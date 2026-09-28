#ifndef CERT_TEST_PLATFORM_H
#define CERT_TEST_PLATFORM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x106
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NOT_SUPPORTED 0x107
static inline const char *esp_err_to_name(int e) { return e == 0 ? "ESP_OK" : "TEST_ERROR"; }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define portMAX_DELAY 0xffffffff
#define TS_TIME_MIN_VALID_YEAR 2025
#define TS_EVENT_BASE_PKI "ts_pki"
#define TS_EVENT_PKI_MATERIAL_CHANGED 1
#define NVS_READWRITE 1
typedef int nvs_handle_t;
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) { pthread_mutex_init(p,NULL); return p; }
static inline int xSemaphoreTake(SemaphoreHandle_t p, unsigned t) { (void)t; return pthread_mutex_lock(p)==0; }
static inline int xSemaphoreGive(SemaphoreHandle_t p) { return pthread_mutex_unlock(p)==0; }
void *heap_caps_malloc(size_t n, unsigned caps);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t ts_event_post(const char *, int, const void *,size_t,unsigned);
typedef void *ts_keypair_t;
#define TS_CRYPTO_KEY_EC_P256 1
esp_err_t ts_crypto_keypair_generate(int,ts_keypair_t *);
esp_err_t ts_crypto_keypair_export_private(ts_keypair_t,char *,size_t *);
void ts_crypto_keypair_free(ts_keypair_t);
typedef void esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *p) { (void)p; return NULL; }
static inline int esp_netif_get_ip_info(esp_netif_t *p,esp_netif_ip_info_t *i) { (void)p; (void)i; return ESP_FAIL; }
#endif
