#!/usr/bin/env python3
"""Generate only this project's two DHCP SDK inputs; the shared SDK is read-only."""
import argparse
from pathlib import Path


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('SDK DHCP source changed at: ' + old[:100])
    return source.replace(old, new, 1)


DHCPS_HELPERS = r'''
/* Project address ownership, updated only on the TCP/IP thread. The lease list
 * remains owned by the SDK: normal renewal, RELEASE and expiry are unchanged. */
static const ts_dhcps_binding_t *ts_dhcps_owner(dhcps_t *dhcps, const u8_t *mac)
{
    for (size_t i = 0; i < dhcps->ts_bindings.count; ++i) {
        if (!memcmp(dhcps->ts_bindings.entries[i].mac, mac, 6)) {
            return &dhcps->ts_bindings.entries[i];
        }
    }
    return NULL;
}

static bool ts_dhcps_reserved(dhcps_t *dhcps, u32_t ip)
{
    for (size_t i = 0; i < dhcps->ts_bindings.count; ++i) {
        if (dhcps->ts_bindings.entries[i].ip == ip) return true;
    }
    return false;
}

static bool ts_dhcps_leased(dhcps_t *dhcps, u32_t ip)
{
    for (list_node *node = dhcps->plist; node; node = node->pnext) {
        struct dhcps_pool *pool = node->pnode;
        if (pool->ip.addr == ip) return true;
    }
    return false;
}

err_t ts_dhcps_replace_bindings(dhcps_t *dhcps, const ts_dhcps_binding_table_t *table)
{
    if (!dhcps || !table || table->count > TS_DHCPS_BINDING_MAX) return ERR_ARG;
    for (size_t i = 0; i < table->count; ++i) {
        const ts_dhcps_binding_t *entry = &table->entries[i];
        if (!entry->ip) return ERR_ARG;
        for (size_t j = 0; j < i; ++j) {
            if (table->entries[j].ip == entry->ip ||
                !memcmp(table->entries[j].mac, entry->mac, 6)) return ERR_USE;
        }
        for (list_node *node = dhcps->plist; node; node = node->pnext) {
            struct dhcps_pool *pool = node->pnode;
            if (pool->ip.addr == entry->ip && memcmp(pool->mac, entry->mac, 6)) return ERR_USE;
        }
    }
    dhcps->ts_bindings = *table;
    return ERR_OK;
}

/* Keep the SDK's candidate order, skipping both reservations and live leases.
 * Clamp an out-of-pool owner's address before resuming dynamic allocation. */
static u32_t ts_dhcps_dynamic_candidate(dhcps_t *dhcps, u32_t candidate)
{
    u32_t start = ntohl(dhcps->dhcps_poll.start_ip.addr);
    u32_t end = ntohl(dhcps->dhcps_poll.end_ip.addr);
    u32_t first = ntohl(candidate);
    if (first < start || first > end) first = start;
    u32_t ip = first;
    do {
        if (!ts_dhcps_reserved(dhcps, htonl(ip)) && !ts_dhcps_leased(dhcps, htonl(ip))) return htonl(ip);
        ip = ip == end ? start : ip + 1;
    } while (ip != first);
    return 0;
}

/* RELEASE may refer to an old address after a rebind. It releases that ordinary
 * lease, without affecting ownership of the new address. */
static bool ts_dhcps_is_release(const u8_t *opts, u16_t len)
{
    for (size_t i = 0; i < len;) {
        if (opts[i] == DHCP_OPTION_END) break;
        if (opts[i] == 0) { ++i; continue; }
        if (i + 1 >= len || i + 2 + opts[i + 1] > len) break;
        if (opts[i] == DHCP_OPTION_MSG_TYPE && opts[i + 1] == 1) return opts[i + 2] == DHCPRELEASE;
        i += 2 + opts[i + 1];
    }
    return false;
}
'''

BRIDGE = r'''
#if ESP_DHCPS
/* Project-private synchronous bridge; only this callback touches the SDK table.
 * No project mutex, NVS, or callback is entered from the TCP/IP thread. */
err_t ts_dhcps_replace_bindings(dhcps_t *dhcps, const ts_dhcps_binding_table_t *table);

static esp_err_t ts_netif_dhcps_replace_bindings_api(esp_netif_api_msg_t *msg)
{
    esp_netif_t *netif = msg->esp_netif;
    const ts_dhcps_binding_table_t *table = msg->data;
    if (!netif || !netif->dhcps) return ESP_ERR_NOT_SUPPORTED;
    if (table->count > TS_DHCPS_BINDING_MAX) return ESP_ERR_INVALID_ARG;
    const esp_netif_ip_info_t *info = netif->ip_info;
    for (size_t i = 0; i < table->count; ++i) {
        uint32_t ip = table->entries[i].ip;
        uint32_t mask = info->netmask.addr;
        if (!mask || (ip & mask) != (info->ip.addr & mask) ||
            ip == info->ip.addr || (ip & ~mask) == 0 || (ip & ~mask) == ~mask) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    err_t ret = ts_dhcps_replace_bindings(netif->dhcps, table);
    if (ret == ERR_USE) return ESP_ERR_INVALID_STATE;
    return ret == ERR_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}
#endif

esp_err_t ts_netif_dhcps_replace_bindings(esp_netif_t *netif,
                                        const ts_dhcps_binding_table_t *table)
{
#if ESP_DHCPS
    if (!netif || !table) return ESP_ERR_INVALID_ARG;
    return esp_netif_lwip_ipc_call(ts_netif_dhcps_replace_bindings_api, netif, (void *)table);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
'''


def prepare(idf_dir, output):
    dhcps = (idf_dir / 'components/lwip/apps/dhcpserver/dhcpserver.c').read_text()
    netif = (idf_dir / 'components/esp_netif/lwip/esp_netif_lwip.c').read_text()
    dhcps = replace_once(dhcps, '#include "dhcpserver/dhcpserver.h"',
                         '#include "dhcpserver/dhcpserver.h"\n#include "ts_dhcps_binding_table.h"')
    dhcps = replace_once(dhcps, '    bool has_declined_ip;\n};',
                         '    bool has_declined_ip;\n    ts_dhcps_binding_table_t ts_bindings;\n};')
    # dhcps_pool is declared in the SDK public header, not mirrored by the project.
    dhcps = replace_once(dhcps, 'static s16_t parse_msg(dhcps_t *dhcps,',
                         DHCPS_HELPERS + '\nstatic s16_t parse_msg(dhcps_t *dhcps,')
    dhcps = replace_once(dhcps, '        dhcps->renew = false;\n',
                         '        dhcps->renew = false;\n'
                         '        const ts_dhcps_binding_t *binding = ts_dhcps_owner(dhcps, m->chaddr);\n'
                         '        bool releasing = ts_dhcps_is_release(&m->options[4], len);\n'
                         '        bool existing_lease = false;\n')
    dhcps = replace_once(dhcps,
        'if (memcmp(pdhcps_pool->mac, m->chaddr, sizeof(pdhcps_pool->mac)) == 0) {',
        'if (memcmp(pdhcps_pool->mac, m->chaddr, sizeof(pdhcps_pool->mac)) == 0 &&\n'
        '                    (releasing ? !memcmp(&pdhcps_pool->ip.addr, m->ciaddr, 4) :\n'
        '                     (binding ? pdhcps_pool->ip.addr == binding->ip :\n'
        '                      (!memcmp(m->ciaddr, "\\0\\0\\0\\0", 4) || !memcmp(&pdhcps_pool->ip.addr, m->ciaddr, 4))))) {')
    dhcps = replace_once(dhcps,
        '                    pnode = pback_node;\n                    goto POOL_CHECK;',
        '                    pnode = pback_node;\n                    existing_lease = true;\n                    goto POOL_CHECK;')
    dhcps = replace_once(dhcps,
        '        if (dhcps->client_address.addr > dhcps->dhcps_poll.end_ip.addr) {',
        '        if (binding) {\n'
        '            dhcps->client_address.addr = binding->ip;\n'
        '        } else if (dhcps->ts_bindings.count) {\n'
        '            dhcps->client_address.addr = ts_dhcps_dynamic_candidate(dhcps, dhcps->client_address.addr);\n'
        '        }\n\n'
        '        if ((!binding && dhcps->client_address.addr > dhcps->dhcps_poll.end_ip.addr) ||\n'
        '            ip4_addr_isany(&dhcps->client_address)) {')
    dhcps = replace_once(dhcps,
        '            if (dhcps->client_address.addr == dhcps->dhcps_poll.end_ip.addr) {',
        '            if (binding) {\n'
        '                /* Reserved leases do not advance the dynamic cursor. */\n'
        '            } else if (dhcps->client_address.addr == dhcps->dhcps_poll.end_ip.addr) {')
    dhcps = replace_once(dhcps,
        'if ((dhcps->client_address.addr > dhcps->dhcps_poll.end_ip.addr) || (ip4_addr_isany(&dhcps->client_address))) {',
        'if ((!binding && !existing_lease && dhcps->client_address.addr > dhcps->dhcps_poll.end_ip.addr) || (ip4_addr_isany(&dhcps->client_address))) {')
    netif = replace_once(netif, '#include "esp_netif.h"',
                         '#include "esp_netif.h"\n#include "ts_dhcps_bindings.h"')
    netif = replace_once(netif, 'struct array_mac_ip_t {', BRIDGE + '\nstruct array_mac_ip_t {')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'dhcpserver.c').write_text(dhcps)
    (output / 'esp_netif_lwip.c').write_text(netif)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.idf_dir, args.output)
