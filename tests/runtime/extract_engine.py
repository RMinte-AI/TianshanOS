from pathlib import Path
import re
s=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
a=s.index('typedef struct {\n    ts_auto_rule_t *rules;');b=s.index('static void payload_free',a)
parts=[s[a:b]]
for name in ['find_rule_index','payload_free','payload_adopt','ts_rule_resolve_presentation','ts_rule_acquire','ts_rule_release','same_config','protected_bindings','ts_rule_commit','compare_values','ts_rule_eval_condition','ts_rule_eval_condition_group','execute_rule','ts_rule_get_by_index']:
 m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',s,re.M);assert m,name
 end=s.index('\n}',m.start())+2;parts.append(s[m.start():end])
Path('/tmp/tianshan-runtime-tests/engine.inc').write_text('\n'.join(parts))
