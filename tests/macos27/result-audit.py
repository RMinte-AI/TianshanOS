"""Check recorded UI results; expected disabled entries are not hidden as passes."""
import json
from pathlib import Path
b=Path('output/macos27-20260928');rows=[];problems=[]
suites=['baseline-pages','baseline-surfaces','nested-surfaces','network','files','terminal','commands','security','ota','responsive','states','error-long','imports']
for name in suites:
 d=json.loads((b/'after'/f'{name}-replay.json').read_text());values=d if isinstance(d,list) else d['results'];errors=[] if isinstance(d,list) else d.get('errors',[])
 over=[i for i,x in enumerate(values) if x.get('overflow')];small=[i for i,x in enumerate(values)if x.get('smallText')];fail=[x for x in values if x.get('status')=='FAIL'];expected=name=='baseline-surfaces' and {x['handler']for x in fail}=={'showCertCSRModal()','showCertInstallModal()','showCertViewModal()'}
 row={'suite':name,'rows':len(values),'pageErrors':errors,'overflowRows':over,'smallTextRows':small,'failedCaptures':len(fail),'expectedDisabled':expected,'status':'PASS' if not(errors or over or small or fail and not expected)else 'FAIL'}
 if expected:row['note']='3 disabled controls in missing-key profile cannot be clicked; all 3 separately covered in provisioned security suite. Raw FAIL is retained.'
 rows.append(row)
 if row['status']=='FAIL':problems.append(row)
fields={'nested-surfaces':['fields','handlers'],'network':['fields'],'commands':['fields','controls'],'security':['fields','controls'],'ota':['fields','controls'],'files':['inputs','rows'],'imports':['disabled'],'states':['controls']}
contracts=[]
for name,keys in fields.items():
 a=json.loads((b/'baseline'/f'{name}-replay.json').read_text());d=json.loads((b/'after'/f'{name}-replay.json').read_text());diff=[{'row':i,'field':f}for i,(x,y)in enumerate(zip(a,d))for f in keys if x.get(f)!=y.get(f)]
 row={'suite':name,'baselineRows':len(a),'afterRows':len(d),'fields':keys,'differences':diff,'status':'PASS'if len(a)==len(d)and not diff else 'FAIL'};contracts.append(row)
 if row['status']=='FAIL':problems.append(row)
for x in json.loads((b/'after/imports-replay.json').read_text()):
 if x['disabled']==x['valid']:problems.append({'import':x})
extra=[]
for name in ['rare-surfaces','remaining-surfaces','deep-conditional','ota-conditional','populated-variables']:
 values=[x for x in json.loads((b/(name+'.json')).read_text())if x['label']=='after'];bad=[x for x in values if x.get('overflow') or x.get('smallText')];extra.append({'suite':name,'records':len(values),'status':'PASS'if not bad else 'FAIL','issues':bad});problems.extend(bad)
final=json.loads((b/'final-browser.json').read_text());assert not [x for x in final if x.get('label')=='B' and (x.get('overflow')or x.get('errors'))]
assert all(x['status']=='PASS'for x in json.loads((b/'control-contrast.json').read_text()))
result={'status':'PASS'if not problems else 'FAIL','scope':'Recorded local UI observations and field contracts only. Synthetic backend, not hardware/all-path proof.','suites':rows,'fieldContracts':contracts,'extra':extra,'problems':problems}
(b/'regression-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2));print(json.dumps(result,ensure_ascii=False,indent=2));assert not problems
