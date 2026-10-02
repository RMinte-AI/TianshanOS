"""Summarize current revision observations; never import historical PASS labels."""
import json,sys
from pathlib import Path
built = '--built' in sys.argv
root = Path('output/macos27-20260929-card-revision') / ('built' if built else '')
baseline = Path('output/macos27-20260928/baseline')
read = lambda p: json.loads(p.read_text())
suites = []
for name in ['baseline-pages','baseline-surfaces','nested-surfaces','network','files','terminal','commands','security','ota','responsive','states','error-long','imports']:
    data = read(root / (name + '.json'))
    rows = data if isinstance(data, list) else data['results']
    errors = [] if isinstance(data, list) else data.get('errors', [])
    failures = [x for x in rows if x.get('status') == 'FAIL']
    expected = name == 'baseline-surfaces' and {x.get('handler') for x in failures} == {'showCertCSRModal()', 'showCertInstallModal()', 'showCertViewModal()'}
    issues = [i for i, x in enumerate(rows) if x.get('overflow') or x.get('smallText')]
    suites.append(dict(name=name, rows=len(rows), pageErrors=errors, layoutIssues=issues, failedCaptures=failures, expectedDisabled=expected, status='FAIL' if errors or issues or failures and not expected else 'PASS'))
fields = {'nested-surfaces':['fields','handlers'], 'network':['fields'], 'commands':['fields','controls'], 'security':['fields','controls'], 'ota':['fields','controls'], 'files':['inputs','rows'], 'imports':['disabled'], 'states':['controls']}
contracts = []
for name, keys in fields.items():
    a, b = read(baseline / (name + '-replay.json')), read(root / (name + '.json'))
    differences = [dict(row=i,field=key) for i,(x,y) in enumerate(zip(a,b)) for key in keys if x.get(key) != y.get(key)]
    contracts.append(dict(name=name, baselineRows=len(a), revisedRows=len(b), differences=differences, status='PASS' if len(a)==len(b) and not differences else 'FAIL'))
extra = []
for name in ['rare-surfaces','remaining-surfaces','deep-conditional','ota-conditional','populated-variables']:
    rows = [x for x in read(root / (name + '.json')) if x['label']=='after']
    issues = [x for x in rows if x.get('overflow') or x.get('smallText')]
    extra.append(dict(name=name, rows=len(rows), issues=issues, status='FAIL' if issues else 'PASS'))
result = dict(scope='Local fixture UI and recorded field lists only; not device authentication or exhaustive path proof.', suites=suites, fieldContracts=contracts, extra=extra)
result['status'] = 'FAIL' if any(x['status']=='FAIL' for group in [suites,contracts,extra] for x in group) else 'PASS'
(root/'regression-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(result['status'], 'UI rows',sum(x['rows'] for x in suites), 'extra rows',sum(x['rows'] for x in extra))
for group in [suites,contracts,extra]:
    for row in group:
        if row['status']=='FAIL': print(json.dumps(row,ensure_ascii=False))
