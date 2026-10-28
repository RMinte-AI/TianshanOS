# TianShanOS - Configuration Management Component
# 配置管理组件

idf_component_register(
    SRCS
        "src/ts_config.c"
        "src/ts_config_nvs.c"
        "src/ts_config_partitions.c"
        "src/ts_config_migrate.c"
        "src/ts_config_file.c"
        "src/ts_config_json.c"
        "src/ts_config_meta.c"
        "src/ts_config_module.c"
        "src/ts_config_schemas.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
        spiffs
        fatfs
        json
        vfs
        esp_partition
        ts_event
    PRIV_REQUIRES
        esp_system
        ts_config_pack
        ts_cert
)

# 添加版本信息
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    TS_CONFIG_VERSION_MAJOR=0
    TS_CONFIG_VERSION_MINOR=1
    TS_CONFIG_VERSION_PATCH=0
)
