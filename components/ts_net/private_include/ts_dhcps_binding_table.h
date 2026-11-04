#pragma once
#include <stddef.h>
#include <stdint.h>

/* Project-private contract shared by the two generated SDK inputs. Network byte
 * order; these are address owners, independent of ordinary expiring leases. */
#define TS_DHCPS_BINDING_MAX 16
typedef struct {
    uint8_t mac[6];
    uint32_t ip;
} ts_dhcps_binding_t;
typedef struct {
    size_t count;
    ts_dhcps_binding_t entries[TS_DHCPS_BINDING_MAX];
} ts_dhcps_binding_table_t;
