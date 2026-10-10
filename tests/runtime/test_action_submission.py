"""Exercise production snapshot, sequencer, submission and executor lifetimes.

Extract definitions verbatim from current sources. Only configuration lookup,
remote execution, heap failure injection and RTOS scheduling are simulated.
"""
from pathlib import Path
import os
import re
import subprocess

root = Path(__file__).resolve().parents[2]
build = Path(os.environ.get('TS_ACTION_TEST_BUILD', '/tmp/tianshan-action-submission'))
build.mkdir(parents=True, exist_ok=True)
manager = (root / 'components/ts_automation/src/ts_action_manager.c').read_text()
engine = (root / 'components/ts_automation/src/ts_rule_engine.c').read_text()


def function(source, name):
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    depth = 0
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*|[{}]'
    for token in re.finditer(tokens, source[match.end() - 1:]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() - 1 + token.end()]
    raise AssertionError(name)


def typedef(name):
    end = manager.index('} ' + name + ';') + len('} ' + name + ';')
    return manager[manager.rfind('typedef struct {', 0, end):end]


(build / 'action_submission_types.inc').write_text('\n'.join(
    typedef(name) for name in ['action_manager_ctx_t', 'action_binding_t', 'action_completion_t']))
definitions = [function(manager, name) for name in [
    'completion_release', 'ts_action_snapshot_retain', 'ts_action_snapshot_release',
    'ts_action_get_ssh_host_ex', 'ts_action_get_ssh_host', 'snapshot_command', 'ts_action_snapshot', 'action_admit',
    'action_finished', 'direct_service_finished', 'entry_finished',
    'ts_action_manager_accepting', 'ts_action_manager_quiesce', 'ts_action_manager_resume',
    'prepare_action_entry', 'action_manager_execute', 'ts_action_manager_execute',
    'action_queue', 'ts_action_queue', 'ts_action_submit_prepared',
    'ts_action_cancel_all', 'ts_action_template_execute', 'action_executor_task']]
definitions += [function(engine, name) for name in [
    'execute_ssh_ref_action', 'ts_action_execute', 'check_action_condition', 'rule_wait',
    'execute_action_with_repeat', 'execute_actions_with_stats']]
(build / 'action_submission.inc').write_text('\n'.join(definitions))
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
command = ['cc', '-std=c11', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
           '-Wno-unused-parameter', '-Wno-unused-function', '-I' + str(build),
           '-I' + str(root / 'tests/runtime/stubs'), '-I' + str(root / 'tests/certificate/stubs'),
           '-I' + str(root / 'components/ts_automation/include'),
           '-I' + str(root / 'components/ts_security/include'),
           '-I' + str(idf / 'components/json/cJSON'),
           str(root / 'tests/runtime/test_action_submission.c'),
           '-o', str(build / 'action_submission')]
subprocess.run(command, check=True, env=env)
subprocess.run([str(build / 'action_submission')], check=True, env=env)
