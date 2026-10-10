from pathlib import Path
import re
s=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
a=s.index('typedef struct {\n    ts_auto_rule_t *rules;');b=s.index('static void payload_free',a)
header=Path('components/ts_automation/src/ts_rule_store_set.h').read_text().replace('#pragma once','')
header=header.replace('#include "ts_rule_store.h"', '#include "'+str(Path('components/ts_automation/src/ts_rule_store.h').resolve())+'"')
header=header.replace('#include "ts_config_pack.h"', '#include "'+str(Path('components/ts_config_pack/include/ts_config_pack.h').resolve())+'"')
# These legacy engine tests deliberately have no saved-set instance. Full set/pack integration is tested separately.
stubs = r"""
#ifndef TS_RULE_SET_INTEGRATION_TEST
bool ts_rule_store_set_available(void){return false;}
bool ts_rule_store_set_present(void){return false;}
int ts_rule_store_set_count(void){return 0;}
uint32_t ts_rule_store_set_generation(void){return 0;}
esp_err_t ts_rule_store_set_info(const char *id,ts_rule_saved_info_t *out){return ESP_ERR_NOT_FOUND;}
esp_err_t ts_rule_store_set_info_at(int i,ts_rule_saved_info_t *out){return ESP_ERR_NOT_FOUND;}
esp_err_t ts_rule_store_set_commit(const ts_auto_rule_t*r,const char*id,uint32_t v,uint32_t g,const char*p,size_t n,const ts_config_pack_acceptance_t*a,ts_rule_commit_t*out){assert(!"unexpected saved-set commit in legacy engine test");return ESP_FAIL;}
#endif
"""
parts=[header,stubs,s[a:b]]
for name in ['find_rule_index','payload_free','payload_adopt','ts_rule_resolve_presentation','ts_rule_acquire','ts_rule_release','same_config','protected_bindings','classify_rule_state','state_pending','pending_id','rule_union_count_locked','rule_commit_impl','ts_rule_commit','compare_values','ts_rule_eval_condition','ts_rule_eval_condition_group','execute_rule','ts_rule_get_by_index']:
 m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',s,re.M);assert m,name
 end=s.index('\n}',m.start())+2;parts.append(s[m.start():end])
Path('/tmp/tianshan-runtime-tests/engine.inc').write_text('\n'.join(parts))
