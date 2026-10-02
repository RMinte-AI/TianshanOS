"""Measure actual screenshot pixels on and beside the sampled slider track."""
import json,math,hashlib,sys
from pathlib import Path
from PIL import Image
candidate=sys.argv[1] if len(sys.argv)>1 else 'build-v7'
assert candidate in ['build-v7','build-v8']
root=Path('output/macos27-20260929-native-rework/slider-track-fix')/('rendered' if candidate=='build-v7' else 'rendered-v8')
raw=root/'raw.json';d=json.loads(raw.read_text());assert d['completed'] and not d['errors'] and len(d['rows'])==24

def lum(c):
 v=[x/255 for x in c];v=[x/12.92 if x<=.04045 else ((x+.055)/1.055)**2.4 for x in v]
 return sum(x*w for x,w in zip(v,[.2126,.7152,.0722]))
def contrast(a,b):
 x,y=lum(a),lum(b);return (max(x,y)+.05)/(min(x,y)+.05)
assert abs(contrast([0,0,0],[255,255,255])-21)<1e-9
rows=[]
for r in d['rows']:
 im=Image.open(r['screenshot']).convert('RGB'); b=r['rect']; assert im.width==r['innerWidth'] and im.height==r['innerHeight']
 # First straight segment, away from midpoint thumb and rounded end; DPR is explicitly 1.
 x=math.ceil(b['x'])+12;y=math.floor(b['y']+b['height']/2)
 samples=[]
 for dx in [0,2,4]:
  track=im.getpixel((x+dx,y)); adjacent=[im.getpixel((x+dx,y-8)),im.getpixel((x+dx,y+8))]
  samples.append({'point':[x+dx,y],'track':track,'adjacent':adjacent,'ratios':[contrast(track,c) for c in adjacent]})
 minimum=min(v for s in samples for v in s['ratios'])
 rows.append({k:r[k] for k in ['build','mode','width','screenshot']}|{'samples':samples,'minimumRatio':minimum,'status':'PASS' if minimum>=3 else 'FAIL','screenshotSha256':hashlib.sha256(Path(r['screenshot']).read_bytes()).hexdigest()})
assert len({(r['build'],r['mode'],r['width']) for r in rows})==24
assert any(r['status']=='FAIL' for r in rows if r['build']=='build-v6'),'Known weak track must be detected'
assert all(r['status']=='PASS' for r in rows if r['build']==candidate),'New track fails 3:1'
result={'scope':'Rendered first enabled color-correction slider track boundary only, six modes and two widths; not thumb, all sliders or whole dialog contrast acceptance. No screenshot editing.','rawSha256':hashlib.sha256(raw.read_bytes()).hexdigest(),'rows':rows}
(root/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
for build in ['build-v6',candidate]:
 subset=[r for r in rows if r['build']==build];print(build,{status:sum(r['status']==status for r in subset) for status in ['PASS','FAIL']},'minimum',min(r['minimumRatio'] for r in subset))
