#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:=/Users/massif/esp/v5.5.2/esp-idf}"
: "${CONFIG_TEST_BUILD:=/tmp/tianshan-config-storage-tests}"
mkdir -p "$CONFIG_TEST_BUILD"
crypto_link=()
if [[ "$(uname -s)" != Darwin ]]; then crypto_link=(-lcrypto); fi
common=(-std=c11 -D_DEFAULT_SOURCE -g -fsanitize=address,undefined -Wno-deprecated-declarations)
for encryption in plain encrypted; do
  mode=()
  if [[ "$encryption" == encrypted ]]; then mode=(-DCONFIG_NVS_ENCRYPTION=1); fi
  cc "${common[@]}" "${mode[@]}" -Itests/runtime/partition_stubs \
    -Itests/runtime/storage_stubs -Itests/runtime/rule_pack/stubs \
    -Icomponents/ts_core/ts_config/include tests/runtime/test_partition_init.c \
    -o "$CONFIG_TEST_BUILD/partitions-$encryption"
  "$CONFIG_TEST_BUILD/partitions-$encryption"
done
cc "${common[@]}" -Itests/runtime/rule_pack/stubs -Icomponents/ts_automation/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_rule_codec.c \
  components/ts_automation/src/ts_rule_codec.c components/ts_automation/src/ts_action_filter.c \
  "$IDF_PATH/components/json/cJSON/cJSON.c" -lm -o "$CONFIG_TEST_BUILD/codec"
"$CONFIG_TEST_BUILD/codec"
migration_includes=(-Itests/runtime/led_persist_stubs -Itests/runtime/storage_stubs
  -I"$IDF_PATH/components/nvs_flash/include" -Itests/runtime/rule_pack/stubs
  -Icomponents/ts_core/ts_config/include -Icomponents/ts_cert/include
  -Icomponents/ts_config_pack/include -Icomponents/ts_led/include
  -Icomponents/ts_storage/include -I"$IDF_PATH/components/json/cJSON")
cc "${common[@]}" "${migration_includes[@]}" -include tests/runtime/led_persist_stubs/paths.h \
  -c components/ts_led/src/ts_led_color_correction.c -o "$CONFIG_TEST_BUILD/led_persistence.o"
python3 - "$CONFIG_TEST_BUILD" <<'PY_DRIVER'
from pathlib import Path
import re,sys
out=[]
for path,names in [
 ('components/ts_drivers/src/ts_temp_source.c',['sanitize_bound_weight','has_positive_bound_weight','sync_bound_variable_compat','ts_temp_source_type_to_str','save_preferred_source_to_nvs','save_bound_vars_to_nvs','load_migrated_temp_config','load_preferred_source_from_nvs']),
 ('components/ts_drivers/src/ts_power_policy.c',['load_config_values','save_config_values'])]:
 text=Path(path).read_text()
 if 'ts_temp_source' in path:
  out.extend(re.findall(r'^#define NVS_.*',text,re.M))
  begin=text.index('typedef struct {\n    ts_temp_source_type_t type;')
  end=text.index('} temp_source_state_t;',begin)+len('} temp_source_state_t;')
  out.append(text[begin:end]);out.append('static temp_source_state_t s_state;')
 else:
  out.extend(re.findall(r'^#define CONFIG_NAMESPACE.*',text,re.M))
  out.append('static struct { ts_power_policy_config_t config; bool enabled_on_boot; } s_pp;')
 for name in names:
  match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',text,re.M)
  assert match,name
  out.append(text[match.start():text.index('\n}',match.start())+2])
Path(sys.argv[1],'system_driver_persistence.inc').write_text('\n'.join(out))
PY_DRIVER
cc "${common[@]}" "${migration_includes[@]}" -Icomponents/ts_drivers/include -I"$CONFIG_TEST_BUILD" tests/runtime/test_system_migration.c \
  "$CONFIG_TEST_BUILD/led_persistence.o" "$IDF_PATH/components/json/cJSON/cJSON.c" \
  -lm -o "$CONFIG_TEST_BUILD/migration"
"$CONFIG_TEST_BUILD/migration"

cc "${common[@]}" -Itests/runtime/storage_stubs -Itests/runtime/rule_pack/stubs \
  -Icomponents/ts_automation/src -Icomponents/ts_core/ts_config/include -Icomponents/ts_security/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_automation_file.c \
  tests/runtime/storage_stubs/crypto.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  "${crypto_link[@]}" -lm -o "$CONFIG_TEST_BUILD/files"
"$CONFIG_TEST_BUILD/files"

cc "${common[@]}" -Itests/runtime/storage_stubs -Itests/runtime/rule_pack/stubs \
  -Icomponents/ts_automation/include -Icomponents/ts_security/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_config_codecs.c \
  components/ts_automation/src/ts_action_codec.c components/ts_automation/src/ts_action_filter.c \
  components/ts_automation/src/ts_rule_codec.c \
  components/ts_security/src/ts_ssh_config_codec.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  -lm -o "$CONFIG_TEST_BUILD/codecs"
"$CONFIG_TEST_BUILD/codecs"

cc "${common[@]}" -Itests/runtime/storage_stubs -Itests/runtime/rule_pack/stubs \
  -Icomponents/ts_automation/src -Icomponents/ts_automation/include -Icomponents/ts_security/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_automation_document.c \
  components/ts_automation/src/ts_automation_document.c components/ts_automation/src/ts_automation_references.c \
  components/ts_automation/src/ts_rule_codec.c \
  components/ts_automation/src/ts_action_codec.c components/ts_automation/src/ts_action_filter.c \
  components/ts_security/src/ts_ssh_config_codec.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  -lm -o "$CONFIG_TEST_BUILD/document"
"$CONFIG_TEST_BUILD/document"

python3 - "$CONFIG_TEST_BUILD" <<'PY'
from pathlib import Path
import re, sys
source=Path('components/ts_automation/src/ts_source_manager.c').read_text()
begin=source.index('typedef struct {\n    char source_id[')
end=source.index('} s_sio_ctx = {0};',begin)+len('} s_sio_ctx = {0};')
parts=[source[begin:end]]
for name in ['find_source_index','current_source_index','current_connection_index','sio_find_connection',
             'sio_release_connection','ts_source_transport_changed','sio_handle_event_message']:
    match=re.search(r'^(?:static )?(?:int|void|sio_connection_t \*)\s*'+name+r'\([^;]+?\)\s*\{',source,re.M)
    assert match,name
    parts.append(source[match.start():source.index('\n}',match.start())+2])
Path(sys.argv[1],'source_transport.inc').write_text('\n'.join(parts))
engine=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
begin=engine.index('typedef struct {\n    ts_auto_rule_t *rules;')
end=engine.index('static void record_execution',begin)
parts=[engine[begin:end]]
for name in ['find_rule_index','record_execution','same_config','ts_rule_engine_init','ts_rule_engine_deinit','ts_rule_acquire',
             'ts_rule_release','ts_rule_config_discard','ts_rule_config_prepare','ts_rule_config_publish',
             'ts_rule_config_pause','ts_rule_config_clear','ts_rule_instance_current','ts_rule_get_by_index','ts_rule_count','compare_values','ts_rule_eval_condition',
             'ts_rule_eval_condition_group','refuse','execute_rule','ts_rule_evaluate','ts_rule_evaluate_all','ts_rule_trigger_ex','ts_rule_trigger_versioned','ts_rule_trigger']:
    match=re.search(r'^(?:static )?(?:int|bool|void|esp_err_t) '+name+r'\([^;]+?\)\s*\{',engine,re.M)
    assert match,name
    parts.append(engine[match.start():engine.index('\n}',match.start())+2])
Path(sys.argv[1],'rule_runtime.inc').write_text('\n'.join(parts))
auto=Path('components/ts_automation/src/ts_automation.c').read_text()
parts=[]
for name in ['ts_automation_start','ts_automation_stop','automation_task']:
    match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',auto,re.M)
    assert match,name
    parts.append(auto[match.start():auto.index('\n}',match.start())+2].replace('s_ctx','top_ctx'))
Path(sys.argv[1],'automation_task.inc').write_text('\n'.join(parts))
action=Path('components/ts_automation/src/ts_action_manager.c').read_text()
context=re.search(r'typedef struct \{[^}]*\} action_manager_ctx_t;',action).group(0)
binding=re.search(r'typedef struct \{[^}]*\} action_binding_t;',action).group(0)
parts=[context,'static action_manager_ctx_t native_action_ctx; static action_manager_ctx_t *s_ctx=&native_action_ctx;',binding, re.search(r'^static const char \*const cli_outputs[^;]+;',action,re.M).group(0)]
for name in ['same_host','action_variable','action_result_begin','action_result_end','ts_action_snapshot_owner','ts_action_snapshot_current','ts_action_snapshot_retain',
             'ts_action_snapshot_release','snapshot_command','ts_action_snapshot','ts_action_get_ssh_host']:
    match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',action,re.M)
    assert match,name
    parts.append(action[match.start():action.index('\n}',match.start())+2])
Path(sys.argv[1],'action_identity.inc').write_text('\n'.join(parts))
match=re.search(r'^static esp_err_t execute_entry\([^;]+?\)\s*\{',action,re.M)
parts=[]
for name in ['exec_cli_bound','ts_action_exec_set_var','execute_action_internal','execute_entry']:
    match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',action,re.M)
    assert match,name
    parts.append(action[match.start():action.index('\n}',match.start())+2])
Path(sys.argv[1],'action_execution.inc').write_text('\n'.join(parts))
parts=[]
for name in ['ts_action_execute','check_action_condition','rule_wait','execute_action_with_repeat']:
    match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',engine,re.M)
    assert match,name
    parts.append(engine[match.start():engine.index('\n}',match.start())+2])
Path(sys.argv[1],'rule_repeat.inc').write_text('\n'.join(parts))
api_result=Path('components/ts_api/src/ts_api.c').read_text()
results=[]
for name in ['ts_api_result_free','ts_api_result_ok','ts_api_result_error','ts_api_result_reason']:
    match=re.search(r'^void '+name+r'\([^;]+?\)\s*\{',api_result,re.M);assert match,name
    results.append(api_result[match.start():api_result.index('\n}',match.start())+2])
Path(sys.argv[1],'api_result.inc').write_text('\n'.join(results))
api=Path('components/ts_api/src/ts_api_automation.c').read_text()
parts=[]
for name in ['api_config_status','api_config_refresh','api_config_backup','config_operation',
             'api_config_preview','api_config_apply','api_config_enable','api_automation_reload',
             'trigger_reject_token','trigger_reject_code','api_automation_rules_trigger','action_execute_failure','api_automation_actions_execute']:
    match=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',api,re.M)
    assert match,name
    parts.append(api[match.start():api.index('\n}',match.start())+2])
ssh=Path('components/ts_api/src/ts_api_ssh.c').read_text()
for owner,names in [(api,['api_automation_variables_set','api_automation_sources_list','api_automation_sources_add',
                         'api_automation_sources_add_mapping','api_automation_sources_remove_mapping','api_automation_sources_enable',
                         'api_automation_actions_list','api_automation_actions_add','api_automation_actions_update',
                         'api_automation_actions_get','api_automation_actions_delete','api_automation_rules_delete','api_automation_actions_import','api_automation_sources_import']),
                    (ssh,['api_ssh_commands_list','api_ssh_commands_get','api_ssh_commands_add','api_ssh_commands_remove','api_ssh_hosts_list','api_ssh_hosts_add','api_ssh_hosts_import','api_ssh_commands_import'])]:
    for name in names:
        match=re.search(r'^static esp_err_t '+name+r'\([^;]+?\)\s*\{',owner,re.M);assert match,name
        # These adapters intentionally contain one shared-owner call.
        end=owner.index('}',match.end())+1;parts.append(owner[match.start():end])
Path(sys.argv[1],'config_api.inc').write_text('\n'.join(parts))

PY
cc "${common[@]}" -DCONFIG_TS_AUTOMATION_MAX_RULES=64 -Itests/runtime/storage_stubs -Icomponents/ts_automation/include \
  -Itests/runtime/rule_pack/stubs -Icomponents/ts_automation/src -Icomponents/ts_security/include \
  -Icomponents/ts_core/ts_config/include -Icomponents/ts_cert/include -Icomponents/ts_config_pack/include \
  -Icomponents/ts_storage/include \
  -Icomponents/ts_api/include \
  -I"$IDF_PATH/components/json/cJSON" -I"$CONFIG_TEST_BUILD" \
  tests/runtime/test_automation_config.c components/ts_api/src/ts_api_auto_config.c \
  components/ts_automation/src/ts_automation_document.c \
  components/ts_automation/src/ts_automation_control.c \
  components/ts_automation/src/ts_automation_migrate.c \
  components/ts_automation/src/ts_variable.c components/ts_automation/src/ts_action_templates.c \
  components/ts_automation/src/ts_source_runtime.c components/ts_automation/src/ts_rule_config.c \
  components/ts_automation/src/ts_rule_pack.c tests/runtime/storage_stubs/crypto.c \
  components/ts_automation/src/ts_automation_references.c components/ts_automation/src/ts_rule_codec.c \
  components/ts_automation/src/ts_action_codec.c components/ts_automation/src/ts_action_filter.c \
  components/ts_security/src/ts_ssh_config_codec.c components/ts_security/src/ts_ssh_commands_config.c \
  components/ts_security/src/ts_ssh_hosts_config.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  "${crypto_link[@]}" -lpthread -lm -o "$CONFIG_TEST_BUILD/owner"
"$CONFIG_TEST_BUILD/owner"

cc "${common[@]}" -Itests/runtime/storage_stubs -I"$IDF_PATH/components/nvs_flash/include" \
  -Itests/runtime/rule_pack/stubs -Icomponents/ts_automation/src \
  -Icomponents/ts_automation/include -Icomponents/ts_security/include \
  -Icomponents/ts_config_pack/include -Icomponents/ts_core/ts_config/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_automation_legacy.c \
  components/ts_automation/src/ts_rule_pack.c tests/runtime/storage_stubs/crypto.c \
  components/ts_automation/src/ts_automation_document.c components/ts_automation/src/ts_rule_codec.c \
  components/ts_automation/src/ts_action_codec.c components/ts_automation/src/ts_action_filter.c \
  components/ts_security/src/ts_ssh_config_codec.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  "${crypto_link[@]}" -lm -o "$CONFIG_TEST_BUILD/legacy"
"$CONFIG_TEST_BUILD/legacy"
