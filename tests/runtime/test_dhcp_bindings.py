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
struct netif;
#define ip4_addr_isany(a) ((a)->addr==0)
#define ipaddr_addr inet_addr
#define ip4addr_aton(s,a) inet_pton(AF_INET,s,&(a)->addr)
''')
    (stubs / 'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5002
''')
    (stubs / 'esp_netif.h').write_text('''#pragma once
#include "lwip/ip_addr.h"
typedef struct {ip4_addr_t ip,netmask,gw;} esp_netif_ip_info_t;
typedef struct esp_netif_obj {struct dhcps_t *dhcps;esp_netif_ip_info_t *ip_info;} esp_netif_t;
''')
    types = sdk[sdk.index('#define BOOTP_BROADCAST'):sdk.index('static void dhcps_tmr(void* arg);')]
    (output / 'sdk_types.inc').write_text(types)
    sdk_names = ['node_insert_to_list', 'node_remove_from_list', 'ts_dhcps_owner', 'ts_dhcps_reserved', 'ts_dhcps_leased',
                 'ts_dhcps_replace_bindings', 'ts_dhcps_dynamic_candidate', 'ts_dhcps_is_release',
                 'parse_options', 'parse_msg', 'kill_oldest_dhcps_pool', 'dhcps_tmr']
    (output / 'sdk_core.inc').write_text('\n\n'.join(function(sdk, n) for n in sdk_names))
    bridge_names = ['esp_netif_api_cb', 'esp_netif_lwip_ipc_call_msg', 'esp_netif_lwip_ipc_call',
                    'ts_netif_dhcps_replace_bindings_api', 'ts_netif_dhcps_replace_bindings']
    (output / 'bridge_core.inc').write_text('\n\n'.join(function(netif, n) for n in bridge_names))
    state_start = project.index('/** 事件回调节点 */')
    state_end = project.index('static ts_dhcp_module_state_t s_state = {0};')
    defines = '\n'.join(line for line in project.splitlines() if line.startswith('#define NVS_'))
    (output / 'project_types.inc').write_text(defines + '\n' + project[state_start:state_end])
    project_names = ['set_default_config', 'get_netif_for_iface', 'apply_config_to_netif', 'install_static_bindings', 'commit_static_bindings',
                     'ts_dhcp_server_add_static_binding', 'ts_dhcp_server_remove_static_binding', 'ts_dhcp_server_get_static_bindings',
                     'ts_dhcp_server_clear_static_bindings', 'ts_dhcp_server_save_config', 'ts_dhcp_server_load_config',
                     'ts_dhcp_server_start', 'ts_dhcp_server_stop', 'ts_dhcp_server_is_initialized', 'ts_dhcp_mac_str_to_array']
    (output / 'project_core.inc').write_text('\n\n'.join(function(project, n) for n in project_names))
    (output / 'console_core.inc').write_text(function(console, 'do_dhcp_bind'))
    command = ['cc', '-std=c11', '-pthread', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter',
               '-fsanitize=address,undefined', '-I' + str(stubs), '-I' + str(output),
               '-I' + str(idf / 'components/lwip/include/apps'), '-I' + str(ROOT / 'components/ts_net/include'),
               '-I' + str(ROOT / 'components/ts_net/private_include'), str(ROOT / 'tests/runtime/dhcp_bindings/test_bindings.c'),
               '-o', str(output / 'dhcp-bindings')]
    with (output / 'compile.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / 'observations.jsonl').open('w') as log, (output / 'stderr.log').open('w') as err:
        subprocess.run([str(output / 'dhcp-bindings')], stdout=log, stderr=err, check=True, timeout=30)
    # A changed SDK must fail generation explicitly, with no old-source fallback.
    drift_results = []
    relative_sources = ['components/lwip/apps/dhcpserver/dhcpserver.c', 'components/esp_netif/lwip/esp_netif_lwip.c']
    markers = ['    bool has_declined_ip;', 'struct array_mac_ip_t {']
    for changed, marker in zip(relative_sources, markers):
        with tempfile.TemporaryDirectory(prefix='ts-dhcp-sdk-drift-') as tmp:
            clone = Path(tmp) / 'sdk'
            for relative in relative_sources:
                target = clone / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                content = (idf / relative).read_text()
                target.write_text(content.replace(marker, '/* fixture changed SDK anchor */', 1) if relative == changed else content)
            result = subprocess.run(['python3', str(ROOT / 'tools/prepare_dhcps_bindings.py'), '--idf-dir', str(clone), '--output', str(Path(tmp) / 'out')], capture_output=True, text=True)
            assert result.returncode != 0 and 'SDK DHCP source changed' in result.stderr
            assert not (Path(tmp) / 'out/dhcpserver.c').exists()
            drift_results.append(dict(changed=changed, returncode=result.returncode, passed=True))
    (output / 'generator-drift.json').write_text(json.dumps(drift_results, indent=2))
    sources = [ROOT / 'components/ts_net/src/ts_dhcp_server.c', ROOT / 'tools/prepare_dhcps_bindings.py',
               generated / 'dhcpserver.c', generated / 'esp_netif_lwip.c']
    (output / 'result.json').write_text(json.dumps(dict(status='PASS', command=command,
        sources={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        sdk_functions=sdk_names, bridge_functions=bridge_names, project_functions=project_names,
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
