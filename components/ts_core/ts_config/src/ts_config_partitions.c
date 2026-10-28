#include "ts_config_partitions.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>

esp_err_t ts_config_partitions_init(void)
{
    const esp_partition_t *keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, TS_CONFIG_KEYS_PARTITION);
    const esp_partition_t *system = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, TS_CONFIG_SYSTEM_PARTITION);
    const esp_partition_t *automation = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, TS_CONFIG_AUTO_PARTITION);
    if (!keys || keys->address != 0x9000 || keys->size != 0xC000 ||
        !system || system->address != 0x9B0000 || system->size != 0x20000 ||
        !automation || automation->address != 0x9D0000 || automation->size != 0x200000) {
        ESP_LOGE("config_partitions", "Install the matching partition table using USB/serial");
        return ESP_ERR_INVALID_STATE;
    }
#ifdef CONFIG_NVS_ENCRYPTION
    /* Default initialization owns the SDK's existing key-protection scheme.
     * The renamed key region must use that same security configuration. */
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) return ret;
    nvs_sec_cfg_t cfg = {0};
    ret = nvs_flash_read_security_cfg_v2(nvs_flash_get_default_security_scheme(), &cfg);
    if (ret == ESP_OK) ret = nvs_flash_secure_init_partition(TS_CONFIG_KEYS_PARTITION, &cfg);
    memset(&cfg, 0, sizeof(cfg));
#else
    esp_err_t ret = nvs_flash_init_partition(TS_CONFIG_KEYS_PARTITION);
    if (ret == ESP_OK) ret = nvs_flash_init();
#endif
    return ret;
}
