#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../components/ts_core/ts_config/src/ts_config_partitions.c"

static esp_partition_t partitions[] = {{0x9000, 0xc000}, {0x9b0000, 0x20000},
                                       {0x9d0000, 0x200000}};
static nvs_sec_scheme_t scheme = {.scheme_id=7};
static unsigned calls, fail_step;
static esp_err_t step(void) { return ++calls == fail_step ? ESP_FAIL : ESP_OK; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA);
    unsigned i = !strcmp(label,"keys_nvs") ? 0 : !strcmp(label,"nvs") ? 1 : 2;
    assert(subtype == (i == 2 ? ESP_PARTITION_SUBTYPE_DATA_SPIFFS : ESP_PARTITION_SUBTYPE_DATA_NVS));
    return &partitions[i];
}
esp_err_t nvs_flash_init(void)
{
#ifdef CONFIG_NVS_ENCRYPTION
    assert(calls == 0);
#else
    assert(calls == 1);
#endif
    return step();
}
esp_err_t nvs_flash_init_partition(const char *label)
{
#ifdef CONFIG_NVS_ENCRYPTION
    assert(!"Encrypted storage must never use plaintext initialization");
#endif
    assert(calls == 0 && !strcmp(label,"keys_nvs"));
    return step();
}
nvs_sec_scheme_t *nvs_flash_get_default_security_scheme(void) { return &scheme; }
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *selected, nvs_sec_cfg_t *cfg)
{
    assert(calls == 1 && selected == &scheme);
    memset(cfg, 0x5a, sizeof(*cfg));
    return step();
}
esp_err_t nvs_flash_secure_init_partition(const char *label, nvs_sec_cfg_t *cfg)
{
    assert(calls == 2 && !strcmp(label,"keys_nvs"));
    for (size_t i=0; i<sizeof(*cfg); ++i) assert(((unsigned char *)cfg)[i] == 0x5a);
    return step();
}
int main(void)
{
    partitions[0].address++;
    assert(ts_config_partitions_init() == ESP_ERR_INVALID_STATE && calls == 0);
    partitions[0].address--;
#ifdef CONFIG_NVS_ENCRYPTION
    unsigned count=3;
#else
    unsigned count=2;
#endif
    assert(ts_config_partitions_init() == ESP_OK && calls == count);
    for (unsigned i=1; i<=count; ++i) {
        calls=0; fail_step=i;
        assert(ts_config_partitions_init() == ESP_FAIL && calls == i);
    }
    puts("PASS partition initialization: layout, key preservation and selected encryption route");
    return 0;
}
