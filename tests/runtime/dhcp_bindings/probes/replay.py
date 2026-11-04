#!/usr/bin/env python3
"""Manual old-failure/current-pass boot probes. Requires the listed Git objects.
Uses the ordinary suite's SDK generation, extraction and native build settings.
Historical adapters live here, never in the default regression harness.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('dhcp_suite', ROOT / 'tests/runtime/test_dhcp_bindings.py')
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)

AP_PROBE = r'''
int main(void) {
    s_state.mutex=&project_mutex;s_state.initialized=true;
    for(unsigned i=0;i<2;i++) {
        set_default_config(&s_state.iface[i].config);
        infos[i]=(esp_netif_ip_info_t){.ip={inet_addr(i?"10.10.99.97":"192.168.4.1")},.netmask={inet_addr("255.255.255.0")}};
        netifs[i]=(esp_netif_t){.dhcps=&servers[i],.ip_info=&infos[i],.status=ESP_NETIF_DHCP_INIT};
    }
    const char *values[]={"10.10.99.100","10.10.99.103","10.10.99.97","255.255.255.0",""};
    const char *keys[]={NVS_KEY_AP_START_IP,NVS_KEY_AP_END_IP,NVS_KEY_AP_GATEWAY,NVS_KEY_AP_NETMASK,NVS_KEY_AP_DNS};
    for(unsigned i=0;i<5;i++)nvs_set_str(0,keys[i],values[i]);
    nvs_set_u8(0,NVS_KEY_AP_ENABLED,1);nvs_set_u32(0,NVS_KEY_AP_LEASE,120);
    ts_dhcp_static_binding_t owner={.mac={2,0,0,0,0,1},.ip="10.10.99.50",.enabled=true};
    nvs_set_blob(0,NVS_KEY_AP_BINDINGS,&owner,sizeof(owner));nvs_set_u8(0,NVS_KEY_AP_BIND_CNT,1);
    if(ts_dhcp_server_load_config()!=ESP_OK||s_state.iface[0].static_binding_count!=1||servers[0].ts_bindings.count!=1)return 10;
    interface_up(0);return netifs[0].status==ESP_NETIF_DHCP_STARTED?0:11;
}
'''


def replay(output, idf):
    suite.run(output, idf)
    manifest = json.loads((output / 'result.json').read_text())
    harness = (ROOT / 'tests/runtime/dhcp_bindings/test_bindings.c').read_text()
    prelude = harness[:harness.index('#include "project_core.inc"')]
    support = harness[:harness.index('static void check_transitions_and_queries')]
    lifecycle = (ROOT / 'tests/runtime/dhcp_bindings/test_eth_lifecycle.inc').read_text()
    legacy = ['apply_candidate_to_host', 'restore_error', 'apply_config_to_netif', 'restore_static_bindings']
    cases = [('ap', 'aad8d1a59', 10), ('ap', 'f43f3b742', 11), ('ap', 'current', 0),
             ('eth', 'c55643827', 12), ('eth', 'current', 0), ('isolation', '4abf001a3', 15), ('isolation', 'current', 0),
             ('control', '0ab404a15', -6), ('control', 'current', 0),
             ('intent', '0ab404a15', -6), ('intent', 'current', 0),
             ('service_eth', 'e05946717', 17), ('service_eth', 'current', 0),
             ('service_sta', 'e05946717', 18), ('service_sta', 'current', 0)]
    results = []
    for kind, ref, expected in cases:
        directory = output / (kind + '-' + ref)
        directory.mkdir(exist_ok=True)
        for source in output.glob('*.inc'):
            shutil.copyfile(source, directory / source.name)
        def read(relative):
            return (ROOT / relative).read_text() if ref == 'current' else subprocess.check_output(['git', 'show', ref + ':' + relative], cwd=ROOT, text=True)
        project = read('components/ts_net/src/ts_dhcp_server.c')
        names = [n for n in manifest['project_functions'] + legacy if re.search(r'^(?:static )?[\w *]+\b'+n+r'\([^;]*?\)\s*\{', project, re.M)]
        if kind == 'ap':
            names = [n for n in names if not n.startswith('ts_dhcp_server_') or n == 'ts_dhcp_server_load_config']
            names = [n for n in names if n not in ['wifi_event_handler', 'dhcp_event_task', 'apply_interface', 'stop_on_tcpip', 'stop_interface']]
        suite.project_sources(directory, project, names)
        if kind == 'ap':
            source = prelude + '#include "project_core.inc"\n' + AP_PROBE
        else:
            if ref == 'c55643827':
                with (directory / 'project_core.inc').open('a') as f:
                    f.write('\nstatic void initialize_eth_intent(void) {}\n')
            suite.network_sources(directory, *(read('components/ts_net/src/'+n) for n in ['ts_net_manager.c','ts_eth.c','ts_net.c']), read('components/ts_console/commands/ts_cmd_net.c'))
            constants = '\n'.join(line for line in read('components/ts_net/src/ts_eth.c').splitlines() if line.startswith('#define ETH_'))
            (directory / 'test_eth_lifecycle.inc').write_text(constants+'\n'+(lifecycle if kind in ('control','intent','service_eth','service_sta') else lifecycle.split('static void check_eth_lifecycle')[0]))
            (directory / 'wifi_core.inc').write_text('\n'.join(suite.function(read('components/ts_net/src/ts_wifi.c'),n) for n in ['ts_wifi_set_mode','ts_wifi_get_mode','ts_wifi_sta_disconnect','ts_wifi_ap_start','ts_wifi_ap_stop']))
            if kind.startswith('service_'):
                assert read('components/ts_core/ts_service/src/ts_service.c') == (ROOT / 'components/ts_core/ts_service/src/ts_service.c').read_text(), 'Registry changed: extract historical registry before replay'
            (directory / 'network_service.inc').write_text('\n'.join(suite.function(read('main/ts_services.c'),n) for n in ['network_service_init','network_service_start','network_service_stop']))
            main = ('int ret=service_restart_probe('+('true' if kind=='service_eth' else 'false')+');for(unsigned i=0;i<2;i++)drain(&servers[i]);return ret;' if kind.startswith('service_') else
                    'check_manager_control_boundaries();for(unsigned i=0;i<2;i++)drain(&servers[i]);return 0;' if kind == 'control' else
                    'check_start_intent_boundaries();return 0;' if kind == 'intent' else
                    'int ret=boot_eth('+('true' if kind=='isolation' else 'false')+');for(unsigned i=0;i<2;i++)drain(&servers[i]);return ret;')
            source = 'static void *s_dhcp_task;\nstatic void vTaskDelay(unsigned ticks);\n' + support + '\nint main(void) { '+main+' }\n'
        (directory / 'probe.c').write_text(source)
        command = manifest['command'].copy()
        command[command.index('-I'+str(output))] = '-I'+str(directory)
        command[-3] = str(directory / 'probe.c')
        command[-1] = str(directory / 'probe')
        with (directory / 'compile.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(directory / 'probe')], capture_output=True, text=True, timeout=30)
        (directory / 'run.log').write_text(result.stdout + result.stderr)
        assert result.returncode == expected, (kind, ref, expected, result.returncode, result.stderr)
        results.append(dict(probe=kind, ref=ref, expected_exit=expected, actual_exit=result.returncode))
        print(f'PASS {kind} {ref}: exit {result.returncode}')
    (output / 'probes.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--idf-dir', type=Path, default=Path(suite.os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf')))
    args = parser.parse_args()
    replay(args.output.resolve(), args.idf_dir)
