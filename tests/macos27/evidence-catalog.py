import json,hashlib,html
from pathlib import Path
from PIL import Image,ImageDraw
base=Path('output/macos27-20260928');rows=[]
for p in sorted((base/'after').glob('*.png')):
 if p.name.startswith('built-A-') or p.name=='terminal-ansi-A.png':continue
 q=base/'baseline'/p.name
 if p.name.startswith('built-B-'):q=p.with_name(p.name.replace('built-B-','built-A-'))
 if p.name=='terminal-ansi-B.png':q=p.with_name('terminal-ansi-A.png')
 rows.append({'name':p.name,'before':str(q)if q.exists()else None,'after':str(p),'afterSize':list(Image.open(p).size),'afterHash':hashlib.sha256(p.read_bytes()).hexdigest(),'kind':'diagnostic' if 'background' in p.name or 'attempt' in p.name else 'production-render','beforeHash':hashlib.sha256(q.read_bytes()).hexdigest()if q.exists()else None,'beforeSize':list(Image.open(q).size)if q.exists()else None})
json.dump(rows,open(base/'screenshot-catalog.json','w'),indent=2)
body=['<!doctype html><meta charset="utf-8"><title>macOS27 production evidence</title><style>body{font:14px system-ui;background:#eee;color:#222;margin:24px}article{margin:28px 0;border-top:1px solid #aaa}section{display:flex;gap:16px}figure{margin:0;flex:1;min-width:0}img{width:100%;height:auto}summary{padding:12px;cursor:pointer}</style><h1>Actual production A/B screenshots</h1><p>Local synthetic backend. Baseline on the left; final implementation on the right. Open the image for its original viewport. Missing counterpart is explicitly identified.</p>']
for r in rows:
 body.append('<details><summary>'+html.escape(r['name'])+' ['+r['kind']+']</summary><section>')
 for key,caption in [('before','Baseline'),('after','After')]:
  p=r[key];body.append('<figure><figcaption>'+caption+'</figcaption>'+('<a href="'+html.escape(str(Path(p).relative_to(base)))+'"><img loading="lazy" src="'+html.escape(str(Path(p).relative_to(base)))+'"></a>'if p else '<p>NOT_RUN: no paired screenshot</p>')+'</figure>')
 body.append('</section></details>')
(base/'screenshots.html').write_text('\n'.join(body))
for n,pages in enumerate([['system','network','files'],['terminal','automation','commands'],['security','ota','logs']]):
 sheet=Image.new('RGB',(1440,1416),'#eee');draw=ImageDraw.Draw(sheet)
 for row,name in enumerate(pages):
  for col,side in enumerate(['baseline','after']):
   p=base/side/f'responsive-{name}-zh-CN-1440.png';im=Image.open(p).convert('RGB');im.thumbnail((720,450));sheet.paste(im,(col*720,row*472+22));draw.text((col*720+8,row*472+4),name+' / '+side,fill='black')
 sheet.save(base/f'comparison-{n+1}.png')
print(len(rows),'after screenshots',sum(r['before']is not None for r in rows),'paired')
