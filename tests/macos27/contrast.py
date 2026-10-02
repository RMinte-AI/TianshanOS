"""Contrast on actual composited pixels, from a text-cleared diagnostic screenshot.
Only the named text nodes are cleared; normal screenshot remains the visual record.
This is a sample audit, not blanket accessibility certification.
"""
import json, re, sys
from pathlib import Path
from PIL import Image
base=Path(sys.argv[1])/'stage1' if len(sys.argv)>1 else Path('output/macos27-20260928/stage1')
if len(sys.argv)>1:
    a=json.loads((Path(sys.argv[1])/'material-audit.json').read_text())
else:
    log=(base/'material-audit.log').read_text()
    a=json.loads(log.split('### Result\n')[1].split('\n###')[0])
im=Image.open(base/'contrast-background.png').convert('RGB')
def lum(rgb):
    c=[v/255 for v in rgb]
    c=[v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in c]
    return sum(x*y for x,y in zip(c,[.2126,.7152,.0722]))
for t in a[0]['text']:
    r=t['rect'];rgb=list(map(int,re.findall(r'\d+',t['color'])[:3]));fg=lum(rgb)
    box=im.crop((int(r['x'])+2,int(r['y'])+2,int(r['x']+r['width'])-2,int(r['y']+r['height'])-2))
    values=[(max(fg,lum(c))+.05)/(min(fg,lum(c))+.05) for c in box.getdata()]
    t['minSampleContrast']=round(min(values),2)
    t['status']='PASS' if min(values)>=4.5 else 'FAIL'
    print(t['selector'],t['text'],t['minSampleContrast'],t['status'])
(base/'material-audit.json').write_text(json.dumps(a,ensure_ascii=False,indent=2))
