# TianShanOS Certificate/PKI Component

idf_component_register(
    SRCS
        "src/ts_cert.c"
        "src/ts_cert_time.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ts_core
        ts_security
        ts_net
        mbedtls
        nvs_flash
        esp_netif
    PRIV_REQUIRES
        lwip
)
