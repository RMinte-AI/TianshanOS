#pragma once
#include <stdint.h>
typedef struct { uint32_t address, size; } esp_partition_t;
enum { ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_NVS = 2,
       ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82 };
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
