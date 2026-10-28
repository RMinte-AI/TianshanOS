#include "ts_storage.h"
#include "ts_config_partitions.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdlib.h>

static bool s_mounted;

/* A failed mount of existing content is never permission to format it. */
static esp_err_t is_blank(const esp_partition_t *partition, bool *blank)
{
    unsigned char *block = malloc(4096);
    if (!block) return ESP_ERR_NO_MEM;
    *blank = true;
    esp_err_t ret = ESP_OK;
    for (size_t offset = 0; offset < partition->size; offset += 4096) {
        ret = esp_partition_read(partition, offset, block, 4096);
        if (ret != ESP_OK) break;
        for (size_t i = 0; i < 4096; ++i) {
            if (block[i] != 0xff) { *blank = false; break; }
        }
        if (!*blank) break;
    }
    free(block);
    return ret;
}

esp_err_t ts_storage_mount_auto(void)
{
    if (s_mounted) return ESP_OK;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, TS_CONFIG_AUTO_PARTITION);
    if (!partition) return ESP_ERR_NOT_FOUND;
    esp_vfs_spiffs_conf_t config = {
        .base_path = TS_CONFIG_AUTO_MOUNT,
        .partition_label = TS_CONFIG_AUTO_PARTITION,
        .max_files = 4,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&config);
    if (ret == ESP_FAIL) {
        bool blank;
        esp_err_t checked = is_blank(partition, &blank);
        if (checked != ESP_OK) return checked;
        if (blank) {
            ret = esp_spiffs_format(TS_CONFIG_AUTO_PARTITION);
            if (ret == ESP_OK) ret = esp_vfs_spiffs_register(&config);
        }
    }
    s_mounted = ret == ESP_OK;
    if (ret != ESP_OK)
        ESP_LOGE("storage_auto", "Internal automation storage unavailable: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t ts_storage_unmount_auto(void)
{
    if (!s_mounted) return ESP_OK;
    esp_err_t ret = esp_vfs_spiffs_unregister(TS_CONFIG_AUTO_PARTITION);
    if (ret == ESP_OK) s_mounted = false;
    return ret;
}

bool ts_storage_auto_mounted(void)
{
    return s_mounted;
}

esp_err_t ts_storage_auto_info(size_t *total, size_t *used)
{
    if (!total || !used) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    return esp_spiffs_info(TS_CONFIG_AUTO_PARTITION, total, used);
}
