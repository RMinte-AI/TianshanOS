"""Sample actual composited backgrounds under control text (not exhaustive WCAG proof)."""
import json,re,sys
from pathlib import Path
from PIL import Image
base=Path(sys.argv[1] if len(sys.argv)>1 else 'output/macos27-20260928')
rows=json.loads((base/'control-contrast.json').read_text())
def lum(rgb):
 c=[v/255 for v in rgb];c=[v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in c]
 return sum(x*y for x,y in zip(c,[.2126,.7152,.0722]))
for t in rows:
 if 'rect' not in t:continue
 r=t['rect'];v=list(map(float,re.findall(r'[\d.]+',t['color'])));alpha=v[3] if len(v)>3 else 1
 im=Image.open(base/'after'/('control-'+t['id']+'-background.png')).convert('RGB')
 box=im.crop((int(r['x'])+2,int(r['y'])+2,int(r['x']+r['width'])-2,int(r['y']+r['height'])-2))
 ratios=[]
 for bg in box.getdata():
  fg=lum([v[i]*alpha+bg[i]*(1-alpha) for i in range(3)]);b=lum(bg);ratios.append((max(fg,b)+.05)/(min(fg,b)+.05))
 t['minSampleContrast']=round(min(ratios),2);t['status']='PASS' if min(ratios)>=4.5 else 'FAIL'
 print(t['id'],t['color'],t['minSampleContrast'],t['status'])
(base/'control-contrast.json').write_text(json.dumps(rows,indent=2))
