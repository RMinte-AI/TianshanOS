#!/usr/bin/env python3
"""Offline DHCP ownership: production project entry points + generated SDK parser.
No sockets/devices. Hardware, RTOS lifecycle, NVS and TCP/IP dispatch are host models.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?[\w *]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2]


def network_sources(output, manager, driver, network, console):
    manager_names = ['ts_net_manager_init', 'ts_net_manager_is_initialized', 'ts_net_manager_deinit', 'stop_eth', 'operate_eth', 'start_eth', 'ts_net_manager_start', 'ts_net_manager_stop',
        'ts_net_manager_restart', 'ts_net_manager_get_status', 'ts_net_manager_get_if_status',
        'ts_net_manager_get_netif', 'ts_net_manager_get_config', 'ts_net_manager_set_config', 'ts_net_manager_set_ip_mode',
        'ts_net_manager_set_static_ip', 'ts_net_manager_set_hostname', 'ts_net_manager_save_config',
        'read_eth_config', 'ts_net_manager_load_config', 'net_event_handler', 'update_eth_ip_from_netif', 'ts_net_ip_u32_to_str', 'ts_net_ip_str_to_u32']
    def manager_identifiers(text):
        return re.sub(r'\bNVS_(?:NAMESPACE|KEY_\w+)\b', lambda m: 'MGR_' + m.group(), text)
    begin = manager.index('/** 事件回调链表节点 */')
    end = manager.index('static ts_net_manager_state_t s_state = {0};')
    defines = '\n'.join(line for line in manager.splitlines() if line.startswith('#define NVS_'))
    (output / 'manager_types.inc').write_text(manager_identifiers(defines + '\n' + manager[begin:end]))
    (output / 'manager_core.inc').write_text('static esp_err_t stop_eth(void);\n' + manager_identifiers('\n\n'.join(sorted(
        (function(manager, n) for n in manager_names if re.search(r'^(?:static )?[\w *]+\b' + n + r'\([^;]*?\)\s*\{', manager, re.M)), key=manager.index))))
    (output / 'eth_creation.inc').write_text(function(driver, 'create_eth_netif_with_dhcps'))
    init = function(driver, 'ts_eth_init')
    creation = re.findall(r'^\s*s_eth_netif = create_eth_netif_with_dhcps\([^;]*;', init, re.M)
    assert len(creation) == 1, 'ETH init must create exactly one netif from its supplied configuration'
    (output / 'eth_init_creation.inc').write_text(creation[0] + '\n' + '\n'.join(re.findall(r'^    s_driver_stop_complete = [^;]+;', init, re.M)))
    (output / 'eth_events.inc').write_text('\n\n'.join(function(driver, n) for n in
        ['dhcp_start_task', 'eth_event_handler', 'ts_eth_start', 'ts_eth_stop']))
    (output / 'network_setter.inc').write_text('\n'.join(function(network, n) for n in ['get_netif', 'ts_net_set_ip_info', 'ts_net_get_status']))
    (output / 'network_cli.inc').write_text('\n'.join(function(console,n) for n in ['do_net_set','do_net_ip','do_net_start','do_net_stop','do_net_restart']))


def project_sources(output, project, names):
    state_start = project.index('/** 事件回调节点 */')
    state_end = project.index('static ts_dhcp_module_state_t s_state = {0};')
    defines = '\n'.join(line for line in project.splitlines() if line.startswith(('#define NVS_', '#define DHCP_EVENT_')))
    event_end = project.index('} dhcp_ip_event_t;') + len('} dhcp_ip_event_t;')
    event_start = project.rfind('typedef struct {', 0, event_end)
    (output / 'project_types.inc').write_text(defines + '\n' + project[event_start:event_end] + '\n' + project[state_start:state_end])
    (output / 'project_core.inc').write_text('\n\n'.join(sorted((function(project, n) for n in names), key=project.index)))


def run(output, idf):
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'generated'
    subprocess.run(['python3', str(ROOT / 'tools/prepare_dhcps_bindings.py'), '--idf-dir', str(idf), '--output', str(generated)], check=True)
    sdk = (generated / 'dhcpserver.c').read_text()
    netif = (generated / 'esp_netif_lwip.c').read_text()
    project = (ROOT / 'components/ts_net/src/ts_dhcp_server.c').read_text()
    console = (ROOT / 'components/ts_console/commands/ts_cmd_dhcp.c').read_text()
    stubs = output / 'stubs'
    (stubs / 'lwip').mkdir(parents=True, exist_ok=True)
    (stubs / 'sdkconfig.h').write_text('#define CONFIG_LWIP_DHCPS_LEASE_UNIT 60\n')
    (stubs / 'lwip/err.h').write_text('#pragma once\ntypedef int err_t;\n#define ERR_OK 0\n#define ERR_ARG -1\n#define ERR_USE -8\n')
    (stubs / 'lwip/ip_addr.h').write_text('''#pragma once
#include <stdint.h>
#include <arpa/inet.h>
typedef uint8_t u8_t;typedef uint16_t u16_t;typedef uint32_t u32_t;typedef int16_t s16_t;
typedef struct {uint32_t addr;} ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
typedef ip4_addr_t esp_ip4_addr_t;
struct netif;
#define ip4_addr_isany(a) ((a)->addr==0)
#define ip4_addr_isany_val(a) ((a).addr==0)
#define ipaddr_addr inet_addr
#define ip4addr_aton(s,a) inet_pton(AF_INET,s,&(a)->addr)
''')
    (stubs / 'esp_wifi_types.h').write_text('#pragma once\ntypedef int wifi_auth_mode_t;\n')
    (stubs / 'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5002
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5003
#define ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED 0x5004
#define ESP_ERR_ESP_NETIF_INVALID_PARAMS 0x5001
#define ESP_ERR_ESP_NETIF_IF_NOT_READY 0x5005
''')
    (stubs / 'esp_netif.h').write_text('''#pragma once
#include "lwip/ip_addr.h"
typedef struct {ip4_addr_t ip,netmask,gw;} esp_netif_ip_info_t;
typedef enum {ESP_NETIF_DHCP_INIT=0, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED} esp_netif_dhcp_status_t;
typedef struct esp_netif_obj {struct dhcps_t *dhcps;esp_netif_ip_info_t *ip_info;esp_netif_dhcp_status_t status;bool up;} esp_netif_t;
typedef esp_err_t (*esp_netif_callback_fn)(void *);
''')
    types = sdk[sdk.index('#define BOOTP_BROADCAST'):sdk.index('static void dhcps_tmr(void* arg);')]
    (output / 'sdk_types.inc').write_text(types)
    (output / 'sdk_dns.inc').write_text(function(netif, 'esp_netif_set_dns_info'))
    sdk_names = ['node_insert_to_list', 'node_remove_from_list', 'ts_dhcps_owner', 'ts_dhcps_reserved', 'ts_dhcps_leased',
                 'ts_dhcps_replace_bindings', 'ts_dhcps_dynamic_candidate', 'ts_dhcps_is_release',
                 'parse_options', 'parse_msg', 'kill_oldest_dhcps_pool', 'dhcps_tmr']
    (output / 'sdk_core.inc').write_text('\n\n'.join(function(sdk, n) for n in sdk_names))
    offer = function(sdk, 'dhcps_option_ip') + '\n' + function(sdk, 'add_offer_options')
    (output / 'sdk_offer.inc').write_text(offer)
    (output / 'sdk_offer_no_dns.inc').write_text(offer.replace('dhcps_option_ip', 'dhcps_option_ip_no_dns').replace('add_offer_options', 'add_offer_options_no_dns'))
    bridge_names = ['esp_netif_api_cb', 'esp_netif_lwip_ipc_call_msg', 'esp_netif_lwip_ipc_call',
                    'ts_netif_dhcps_replace_bindings_api', 'ts_netif_dhcps_replace_bindings']
    (output / 'bridge_core.inc').write_text('\n\n'.join(function(netif, n) for n in bridge_names))
    network = (ROOT / 'components/ts_net/src/ts_net.c').read_text()
    (output / 'network_validation.inc').write_text('\n\n'.join(function(network, n) for n in ['ts_net_validate_eth_ip', 'ts_net_validate_eth_config']))
    project_names = ['validate_network_info', 'apply_interface', 'initialize_eth_intent', 'eth_resume_error', 'stop_on_tcpip', 'stop_interface',
                     'ts_dhcp_server_init', 'ts_dhcp_server_eth_suspend', 'ts_dhcp_server_eth_resume', 'ts_dhcp_server_apply_eth_network', 'set_default_config', 'set_interface_defaults', 'same_network_config',
                     'get_netif_for_iface', 'build_binding_table', 'validate_config', 'validate_network',
                     'configure_stopped_netif', 'apply_on_tcpip', 'apply_candidate', 'install_static_bindings',
                     'commit_static_bindings', 'same_bindings', 'interface_error', 'dhcp_nvs_keys', 'read_saved_interface',
                     'ts_dhcp_server_add_static_binding', 'ts_dhcp_server_remove_static_binding', 'ts_dhcp_server_get_static_bindings',
                     'ts_dhcp_server_clear_static_bindings', 'ts_dhcp_server_save_config', 'ts_dhcp_server_load_config',
                     'ts_dhcp_server_start', 'ts_dhcp_server_stop', 'ts_dhcp_server_restart', 'ts_dhcp_server_set_config',
                     'ts_dhcp_server_set_pool', 'ts_dhcp_server_set_lease_time', 'ts_dhcp_server_reset_config', 'ts_dhcp_server_get_config', 'ts_dhcp_server_get_status',
                     'ts_dhcp_server_register_event_cb', 'ts_dhcp_server_unregister_event_cb', 'ts_dhcp_server_get_state', 'ts_dhcp_server_is_running', 'ts_dhcp_state_to_str', 'ts_dhcp_server_get_restore_error', 'ts_dhcp_server_get_apply_error', 'client_lease_expiry',
                     'wifi_event_handler', 'ts_dhcp_mac_array_to_str', 'dhcp_event_task', 'ts_dhcp_server_is_initialized', 'ts_dhcp_mac_str_to_array']
    project_sources(output, project, project_names)
    (output / 'notify_core.inc').write_text(function(project, 'notify_event').replace('static void notify_event(', 'static void production_notify_event(', 1))
    (output / 'console_core.inc').write_text('\n'.join(function(console, n) for n in ['state_color', 'format_uptime', 'do_dhcp_list_all', 'do_dhcp_status', 'do_dhcp_bind', 'do_dhcp_clear_bindings', 'do_dhcp_pool']))
    args_start = console.index('static struct {')
    args_end = console.index('} s_dhcp_args;', args_start) + len('} s_dhcp_args;')
    (output / 'console_args.inc').write_text(console[args_start:args_end])
    (output / 'console_command.inc').write_text(function(console, 'do_cmd_dhcp'))
    network_sources(output, (ROOT / 'components/ts_net/src/ts_net_manager.c').read_text(),
        (ROOT / 'components/ts_net/src/ts_eth.c').read_text(), network,
        (ROOT / 'components/ts_console/commands/ts_cmd_net.c').read_text())
    api = (ROOT / 'components/ts_api/src/ts_api_dhcp.c').read_text()
    (output / 'dhcp_status_api.inc').write_text('\n'.join(function(api, n) for n in ['iface_name', 'add_recovery_status', 'api_dhcp_status']))
    (output / 'status_api.inc').write_text(function((ROOT / 'components/ts_api/src/ts_api_network.c').read_text(), 'add_eth_config_status'))
    (output / 'network_service.inc').write_text('\n'.join(function((ROOT / 'main/ts_services.c').read_text(), n) for n in ['network_service_init','network_service_start','network_service_stop']))
    (output / 'service_api.inc').write_text('\n'.join(function((ROOT / 'components/ts_api/src/ts_api_service.c').read_text(), n) for n in ['api_service_start','api_service_stop','api_service_restart']))
    wifi = (ROOT / 'components/ts_net/src/ts_wifi.c').read_text()
    (output / 'wifi_core.inc').write_text('\n'.join(function(wifi,n) for n in ['ts_wifi_set_mode','ts_wifi_get_mode','ts_wifi_sta_disconnect','ts_wifi_ap_start','ts_wifi_ap_stop']))
    (output / 'wifi_cli.inc').write_text(function((ROOT / 'components/ts_console/commands/ts_cmd_wifi.c').read_text(),'do_wifi_scan'))
    (output / 'wifi_api.inc').write_text(function((ROOT / 'components/ts_api/src/ts_api_network.c').read_text(),'api_network_wifi_mode'))
    registry = (ROOT / 'components/ts_core/ts_service/src/ts_service.c').read_text()
    (output / 'service_types.inc').write_text(registry[registry.index('typedef struct ts_service_handle'):registry.index('static ts_service_context_t s_svc_ctx')])
    service_names = ['find_service_by_name','start_service_internal','stop_service_internal','set_service_state','check_dependencies','notify_state_change','ts_service_find','ts_service_start','ts_service_stop','ts_service_restart']
    declarations = '\n'.join(function(registry,n).split('\n{')[0]+';' for n in service_names if n.startswith(('find_','start_','stop_','set_','check_','notify_')))
    (output / 'service_core.inc').write_text(declarations+'\n'+'\n'.join(sorted((function(registry,n) for n in service_names),key=registry.index)))
    command = ['cc', '-I' + str(idf / 'components/json/cJSON'), str(idf / 'components/json/cJSON/cJSON.c'), '-std=c11', '-pthread', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter',
               '-fsanitize=address,undefined', '-I' + str(stubs), '-I' + str(output),
               '-I' + str(idf / 'components/lwip/include/apps'), '-I' + str(ROOT / 'components/ts_net/include'),
               '-I' + str(ROOT / 'components/ts_net/private_include'), '-I' + str(ROOT / 'components/ts_core/ts_service/include'), str(ROOT / 'tests/runtime/dhcp_bindings/test_bindings.c'),
               '-o', str(output / 'dhcp-bindings')]
    with (output / 'compile.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / 'observations.jsonl').open('w') as log, (output / 'stderr.log').open('w') as err:
        subprocess.run([str(output / 'dhcp-bindings')], stdout=log, stderr=err, check=True, timeout=30)
    assert '#define DHCPS_MAX_LEASE 0x64' in (idf / 'components/lwip/include/apps/dhcpserver/dhcpserver.h').read_text()
    assert 'range_start_ip = server_ip + 1;' in sdk
    assert 'range_end_ip = range_start_ip + DHCPS_MAX_LEASE - 1;' in sdk
    # A changed SDK must fail generation explicitly, with no old-source fallback.
    # The last two cases guard dependencies the patches rely on but do not touch.
    drift_results = []
    relative_sources = ['components/lwip/apps/dhcpserver/dhcpserver.c', 'components/esp_netif/lwip/esp_netif_lwip.c']
    drift_cases = [
        (relative_sources[0], '    bool has_declined_ip;', '/* fixture changed SDK anchor */'),
        (relative_sources[1], 'struct array_mac_ip_t {', '/* fixture changed SDK anchor */'),
        (relative_sources[0], 'state = parse_msg(dhcps, pmsg_dhcps, tlen - 240);',
         'state = parse_msg(dhcps, pmsg_dhcps, tlen - 236);'),
        (relative_sources[0], 'parse_options(dhcps, &m->options[4], len)',
         'parse_options(dhcps, &m->options[8], len)'),
    ]
    for changed, old, new in drift_cases:
        with tempfile.TemporaryDirectory(prefix='ts-dhcp-sdk-drift-') as tmp:
            clone = Path(tmp) / 'sdk'
            for relative in relative_sources:
                target = clone / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                content = (idf / relative).read_text()
                if relative == changed:
                    assert old in content, old
                    content = content.replace(old, new, 1)
                target.write_text(content)
            result = subprocess.run(['python3', str(ROOT / 'tools/prepare_dhcps_bindings.py'), '--idf-dir', str(clone), '--output', str(Path(tmp) / 'out')], capture_output=True, text=True)
            assert result.returncode != 0 and 'SDK DHCP source changed' in result.stderr
            assert not (Path(tmp) / 'out/dhcpserver.c').exists()
            drift_results.append(dict(changed=changed, marker=old[:48], returncode=result.returncode, passed=True))
    (output / 'generator-drift.json').write_text(json.dumps(drift_results, indent=2))
    sources = [ROOT / 'components/ts_net/src/ts_dhcp_server.c', ROOT / 'tools/prepare_dhcps_bindings.py',
               generated / 'dhcpserver.c', generated / 'esp_netif_lwip.c',
               ROOT / 'components/ts_net/src/ts_eth.c', ROOT / 'components/ts_net/src/ts_net_manager.c',
               ROOT / 'components/ts_net/src/ts_net.c', ROOT / 'components/ts_net/src/ts_wifi.c',
               ROOT / 'main/ts_services.c', ROOT / 'components/ts_console/commands/ts_cmd_wifi.c',
               ROOT / 'components/ts_core/ts_service/src/ts_service.c', ROOT / 'tests/runtime/dhcp_bindings/test_eth_lifecycle.inc']
    (output / 'result.json').write_text(json.dumps(dict(status='PASS', command=command,
        sources={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        sdk_functions=sdk_names, sdk_netif_functions=['esp_netif_set_dns_info'], bridge_functions=bridge_names, project_functions=project_names,
        scope='Synthetic DHCP messages and virtual lease ticks; source-extracted production parser, ownership table, bridge, project operations, NVS load/save and CLI binding entry. Real pthread TCP/IP dispatch model. No UDP/netif hardware/NVS hardware/device acceptance.'), indent=2))
    print((output / 'observations.jsonl').read_text(), end='')
    print('PASS F38 generated SDK + project entry points (ASan/UBSan); evidence:', output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path)
    p.add_argument('--idf-dir', type=Path, default=Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf')))
    args = p.parse_args()
    if args.output:
        run(args.output, args.idf_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='ts-dhcp-bindings-') as tmp:
            run(Path(tmp), args.idf_dir)
