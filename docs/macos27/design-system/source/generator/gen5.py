exec(open('gen4.py').read())
import re
WMS=re.search(r'<svg viewBox="0 -760[^>]*>.*?</svg>',header('系统'),re.S).group(0)
MK='<svg viewBox="0 0 30 30" width="30" height="30" fill="none"><path d="M3 24 12 6l5 10 3-5 8 13H3Z" stroke="#293d54" stroke-width="1.8" stroke-linejoin="round"/><path d="m9 12 3 3 3-3M7 24l7-9 6 9" stroke="#293d54" stroke-width="1.2"/></svg>'
BR=f'<div style="display:flex;align-items:flex-start;gap:9px;height:30px">{MK}{WMS.replace("style=\"margin-top:5.4px\"","style=\"margin-top:5.4px\"")}</div>'
NAVEN=['System','Network','Files','Terminal','Automation','SSH Commands','Security']
def navb(items,on,scroll=False):
    a=''.join(f'<a class="{"on" if n==on else ""}" href="#" style="flex:none;white-space:nowrap">{n}</a>' for n in items)
    return f'<nav class="nav" style="{"overflow:hidden;width:100%;box-sizing:border-box;" if scroll else ""}">{a}</nav>'
RIGHT_EN='<span class="state ok">Connected</span><button class="btn sm quiet">EN</button><span class="t-label">root</span><button class="btn sm">Log out</button>'
def frame(x,y,w,h,inner,label):
    return f'<div style="position:absolute;left:{x}px;top:{y}px;width:{w}px"><div class="t-note" style="margin-bottom:8px;color:var(--ink-2)">{label}</div><div class="wall" style="width:{w}px;height:{h}px;position:relative;overflow:hidden;border-radius:20px">{inner}</div></div>'
# A: 1440 English (nav fits?) - 只画顶栏
A=f'<div class="m-bar" style="position:absolute;top:20px;left:24px;right:24px;height:60px;border-radius:30px;display:grid;grid-template-columns:1fr auto 1fr;align-items:center;padding:0 16px"><div style="justify-self:start;margin-left:6px">{BR}</div>{navb(NAVEN,"System")}<div style="justify-self:end;display:flex;align-items:center;gap:14px">{RIGHT_EN}</div></div>'
# B: 820 <=1100 断点：两行顶栏
def hdr2(w):
    return f'<div class="m-bar" style="position:absolute;top:12px;left:12px;right:12px;border-radius:28px;padding:12px 14px;display:grid;gap:10px"><div style="display:flex;align-items:center;justify-content:space-between">{BR}<div style="display:flex;align-items:center;gap:10px">{RIGHT_EN if w>600 else '<span class="dot ok"></span><button class="btn sm quiet">EN</button><button class="btn sm">Log out</button>'}</div></div>{navb(NAVEN,"System",True)}</div>'
def cards(w):
    g=2 if w>600 else 1
    def c(t,v): return f'<div class="card" style="padding:14px 16px"><div class="t-section">{t}</div><div class="t-value" style="font-size:22px;margin-top:6px">{v}</div></div>'
    return f'<div class="m-surface" style="position:absolute;top:{124 if w>600 else 118}px;left:12px;right:12px;bottom:12px;border-radius:{28 if w>600 else 22}px;padding:{16 if w>600 else 10}px;overflow:hidden"><div style="display:grid;grid-template-columns:repeat({g},1fr);gap:12px">{c("Memory","128 / 320 KB")}{c("Temperature","41.5 °C")}{c("Fan 0","2 350 RPM")}{c("Network","192.0.2.10")}</div></div>'
B_=hdr2(820)+cards(820)
# C: 390 手机 + 底部 sheet
sheetC=f'<div class="m-float" style="position:absolute;left:0;right:0;bottom:0;border-radius:32px 32px 0 0;padding:20px 16px 24px"><div style="width:36px;height:5px;border-radius:3px;background:rgba(60,60,67,.3);margin:-8px auto 14px"></div><div class="t-section" style="font-size:20px;margin-bottom:12px">Shutdown settings</div>{GRP(ROWF("Low-voltage threshold",FI("12.6","",64,"num")+UN("V"),"Start countdown below this value"),ROWF("Shutdown countdown",FI("60","",64,"num")+UN("s")))}<div style="display:grid;gap:8px;margin-top:16px"><button class="btn lg primary" style="width:100%">Save</button><button class="btn lg" style="width:100%">Cancel</button></div></div>'
C=hdr2(390)+cards(390)+'<div class="dim" style="background:rgba(20,30,45,.14)"></div>'+sheetC
notes=f'''<div style="position:absolute;left:1000px;top:36px;width:400px" class="t-note"><div class="t-section" style="font-size:17px;margin-bottom:10px">响应式与英文规则</div><div style="display:grid;gap:8px;color:var(--ink-2)"><div>· 断点沿用现有 CSS：≤1100 顶栏改两行、≤600 收紧边距，不新增断点。</div><div>· 顶栏：第一行品牌 + 状态/用户，第二行导航胶囊整宽；放不下时胶囊内横向滚动，不换行、不缩小字号。</div><div>· 卡片网格：≥820 两列，≤600 一列；表格在窄屏改为横向滚动，不折行成卡片。</div><div>· 手机上 sheet 贴底，顶部圆角 32，带拖动条；按钮上下堆叠、整宽，主按钮在上。</div><div>· 英文文案会更长：按钮不设固定宽度，分组行的标签允许折成两行，控件宽度保持。</div><div>· 英文导航最长项为 SSH Commands（取自 en-US 语言包 nav.ssh）。</div></div></div>'''
body=f'<div class="dc wall" style="width:1440px;height:1290px;position:relative;overflow:hidden"><div style="position:absolute;inset:0">'+'''<div style="position:absolute;left:40px;top:40px;width:920px"><div class="t-section" style="font-size:17px;margin-bottom:10px">顶栏在不同宽度下的实测（中文 / 英文，导航为 7 项，英文最长项 SSH Commands）</div><div class="card" style="padding:0"><div class="tr th" style="--cols:120px 1fr 1fr 1fr"><div>视口宽度</div><div>导航宽度</div><div>导航与品牌间距</div><div>导航与右侧间距</div></div><div class="tr" style="--cols:120px 1fr 1fr 1fr"><div class="mono">1440</div><div>426 / 590 px</div><div>260 / 178 px</div><div>229 / 147 px</div></div><div class="tr" style="--cols:120px 1fr 1fr 1fr"><div class="mono">1280</div><div>426 / 590 px</div><div>180 / 98 px</div><div>149 / 67 px</div></div><div class="tr" style="--cols:120px 1fr 1fr 1fr"><div class="mono">1200</div><div>426 / 590 px</div><div>140 / 58 px</div><div>109 / 27 px</div></div><div class="tr" style="--cols:120px 1fr 1fr 1fr"><div class="mono">1101</div><div>426 / 576 px</div><div>91 / 0 px</div><div>59 / 0 px</div></div></div><div class="t-note" style="margin-top:8px">数据：Chromium 无头渲染，顶栏为「1fr auto 1fr」网格，品牌区按 200px 占位（实际约 183px）；每格为「中文 / 英文」。英文在 1101px 时导航与右侧间距为 0，只是贴住没有重叠；因此建议两行顶栏的断点从 1100 提到 1180，留出余量。</div></div>'''+frame(40,360,820,560,B_,'820 宽（≤1100 断点）· 顶栏两行')+frame(920,360,390,844,C,'390 宽（≤600）· 底部 sheet')+notes+'</div></div>'
write('Resp','响应式与英文 目标稿',1290,body);BOARDS['Resp']=1290
c=json.load(open(P+'canvas.json'))
c['boards']['Resp.dc.html']={"x":0,"y":0,"w":1440,"h":1290,"title":"响应式与英文 目标稿"}
rows=[['Main','Modal'],['Net','Files','Term'],['Auto','Cmds','Ota'],['Sec','Spec'],['Sheets1','Sheets2','SheetsAuto'],['SheetsLed','SheetsSec','SheetsTypes'],['SheetsMore','SheetsStates','Resp']]
Hs={k[:-8]:v['h'] for k,v in c['boards'].items()};Hs.update(BOARDS)
y=0
for r in rows:
    for i,k in enumerate(r):
        b=c['boards'][k+'.dc.html'];b['x']=i*1520;b['y']=y;b['h']=Hs[k]
    y+=max(Hs[k] for k in r)+140
c['order']=[k+'.dc.html' for r in rows for k in r]
json.dump(c,open(P+'canvas.json','w'),ensure_ascii=False,indent=1)
