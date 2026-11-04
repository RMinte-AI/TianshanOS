#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include "ts_dhcps_binding_table.h"

/* Synchronous TCP/IP call. Copies the table before returning; ERR_INVALID_STATE
 * means another owner or active lease conflicts, with the old table preserved. */
esp_err_t ts_netif_dhcps_replace_bindings(esp_netif_t *netif,
                                        const ts_dhcps_binding_table_t *table);
