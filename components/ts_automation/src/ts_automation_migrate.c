#include "ts_automation_migrate.h"
#include "ts_automation_legacy.h"
#include "ts_automation_file.h"
#include "ts_config_partitions.h"
#include "ts_config_pack.h"
#include "ts_ssh_service.h"
#include "ts_ssh_config_codec.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "automation_migrate";

static esp_err_t register_legacy_services(const cJSON *legacy,ts_auto_config_error_t *error)
{
    esp_err_t ret=ESP_OK;
    const cJSON *commands=cJSON_GetObjectItemCaseSensitive(legacy,"commands");
    const cJSON *hosts=cJSON_GetObjectItemCaseSensitive(legacy,"hosts");
    if (!cJSON_GetArraySize(commands)) return ESP_OK;
    ts_ssh_command_config_t *command = heap_caps_malloc(sizeof(*command), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!command) {
        if (error) error->reason="out_of_memory";
        return ESP_ERR_NO_MEM;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row,commands){
        if(ret!=ESP_OK)break;
        if(ts_ssh_command_decode(row,command)!=ESP_OK||!command->nohup)continue;
        const cJSON *host_row;bool found=false;
        cJSON_ArrayForEach(host_row,hosts){
            ts_ssh_host_config_t host;
            if(ts_ssh_host_decode(host_row,&host)!=ESP_OK||strcmp(host.id,command->host_id))continue;
            found=true;ret=ts_ssh_service_import_legacy(command,&host);break;
        }
        if(!found)ESP_LOGW(TAG,"Legacy background command has no usable host: %s",command->id);
    }
    free(command);
    if(ret!=ESP_OK&&error)error->reason="legacy_service_records_not_saved";
    return ret;
}

static void cleanup_legacy(void)
{
    static const char *const namespaces[] = {
        "action_tpl", "ts_auto_src", "auto_rules", "rule_stage", "ts_ssh_cmd",
        "ts_ssh_cfg", "ts_rules", "ts_actions", "ts_sources", "ts_var", "ts_meta"
    };
    for (unsigned i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); ++i) {
        nvs_handle_t handle;
        esp_err_t ret = nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION,
            namespaces[i], NVS_READONLY, &handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) continue;
        if (ret == ESP_OK) {
            nvs_close(handle);
            ret = nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION,
                namespaces[i], NVS_READWRITE, &handle);
            if (ret == ESP_OK) {
                ret = nvs_erase_all(handle);
                if (ret == ESP_OK) ret = nvs_commit(handle);
                nvs_close(handle);
            }
        }
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "New configuration is active; legacy namespace cleanup failed: %s (%s)",
                     namespaces[i], esp_err_to_name(ret));
    }
}

esp_err_t ts_auto_config_migrate(bool sd_available, bool *installed, ts_auto_config_error_t *error)
{
    if (!installed) return ESP_ERR_INVALID_ARG;
    *installed = false;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("ts_cfg_state", NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    uint8_t version = 0;
    ret = nvs_get_u8(handle, "auto_v", &version);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    if (ret == ESP_OK && version > 1) ret = ESP_ERR_NOT_SUPPORTED;
    if (ret == ESP_OK && version == 0) {
        ts_auto_config_source_t source = sd_available ? TS_AUTO_CONFIG_SD : TS_AUTO_CONFIG_INTERNAL;
        char *bytes = NULL;cJSON *legacy=NULL;
        ret = ts_auto_file_load(sd_available ? TS_AUTO_FILE_SD : TS_AUTO_FILE_INTERNAL, &bytes);
        if (ret == ESP_ERR_NOT_FOUND) {
            ret = ts_config_pack_init();
            if (ret == ESP_OK) ret = ts_auto_legacy_read(sd_available, &legacy, error);
            bool empty=true;
            for(unsigned kind=0;legacy && kind<TS_AUTO_CONFIG_KIND_COUNT;++kind)
                if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(legacy,ts_auto_config_collection(kind))))empty=false;
            /* No card and no legacy automation is not an emergency backup.
             * Leave the internal file absent until the first user save. */
            if (ret == ESP_OK && (sd_available || !empty)) {
                ret = ts_auto_config_install(source, legacy, error);
                if(ret==ESP_OK)*installed=true;
            }
        } else if (ret == ESP_OK) {
            /* A prior boot may have saved/published successfully before the
             * completion marker was committed. Never import the old data again. */
            cJSON *json = cJSON_Parse(bytes);
            const cJSON *format = cJSON_GetObjectItemCaseSensitive(json, "format_version");
            const cJSON *migration = cJSON_GetObjectItemCaseSensitive(json, "migration_version");
            if (!cJSON_IsNumber(format) || format->valuedouble != 1 ||
                !cJSON_IsNumber(migration) || migration->valuedouble != 1) ret = ESP_ERR_INVALID_STATE;
            else ret = ts_auto_config_open(source, error);
            if(ret==ESP_OK)*installed=true;
            cJSON_Delete(json);
            ts_auto_config_status_t status;
            ts_auto_config_status(&status);
            if (ret == ESP_OK && status.error.reason &&
                (!strcmp(status.error.reason, "out_of_memory") || !strcmp(status.error.reason, "runtime_unavailable") ||
                 !strcmp(status.error.reason, "variable_capacity_reached") || !strcmp(status.error.reason, "source_connection_limit"))) {
                if (error) *error = status.error;
                ret = !strcmp(status.error.reason, "out_of_memory") ? ESP_ERR_NO_MEM :
                      (!strcmp(status.error.reason, "variable_capacity_reached") || !strcmp(status.error.reason, "source_connection_limit")) ?
                      ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_STATE;
            }
        }
        free(bytes);
        /* Read the original definitions even when a prior boot committed the
         * new file. Local deletion must not erase the old process locator. */
        if(ret==ESP_OK&&!legacy){
            ret=ts_config_pack_init();
            if(ret==ESP_OK)ret=ts_auto_legacy_read(sd_available,&legacy,error);
        }
        if(ret==ESP_OK)ret=register_legacy_services(legacy,error);
        cJSON_Delete(legacy);
        if (ret == ESP_OK) {
            ret = nvs_set_u8(handle, "auto_v", 1);
            if (ret == ESP_OK) ret = nvs_commit(handle);
            if (ret != ESP_OK && error) error->reason = "migration_saved_not_finished";
        }
    }
    nvs_close(handle);
    if (ret == ESP_OK) cleanup_legacy();
    return ret;
}
