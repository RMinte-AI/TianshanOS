/**
 * @file ts_eth.h
 * @brief Ethernet Driver (W5500)
 */

#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ethernet configuration */
typedef struct {
    int spi_host;
    int gpio_mosi;
    int gpio_miso;
    int gpio_sclk;
    int gpio_cs;
    int gpio_int;
    int gpio_rst;
    int spi_clock_mhz;
    esp_netif_ip_info_t ip_info; /**< Validated host address, owned by ts_net. */
} ts_eth_config_t;

#define TS_ETH_DEFAULT_CONFIG() { \
    .spi_host = 2, \
    .gpio_mosi = -1, \
    .gpio_miso = -1, \
    .gpio_sclk = -1, \
    .gpio_cs = -1, \
    .gpio_int = -1, \
    .gpio_rst = -1, \
    .spi_clock_mhz = 20 \
}

/**
 * @brief Initialize Ethernet
 */
esp_err_t ts_eth_init(const ts_eth_config_t *config);

/**
 * @brief Deinitialize Ethernet
 */
esp_err_t ts_eth_deinit(void);

/**
 * @brief Start Ethernet
 */
esp_err_t ts_eth_start(void);

/**
 * @brief Stop Ethernet
 */
esp_err_t ts_eth_stop(void);

/**
 * @brief Check if link is up
 */
bool ts_eth_is_link_up(void);

/**
 * @brief Get netif handle
 */
esp_netif_t *ts_eth_get_netif(void);

#ifdef __cplusplus
}
#endif
