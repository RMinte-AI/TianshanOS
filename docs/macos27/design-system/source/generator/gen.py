import re,json
P='dc/project/'
WM=open('wm.txt').read().split('\n',1)[1]
# ---------- css additions
css=open(P+'spec.css').read()
css=css.replace('.t-big{font-family:var(--display);font-size:40px;line-height:44px;font-weight:400;letter-spacing:-.5px;','.t-big{font-size:40px;line-height:44px;font-weight:300;letter-spacing:-1px;')
css=css.replace('.t-title{font-family:var(--display);font-size:26px;line-height:32px;font-weight:700;letter-spacing:-.2px}','.t-title{font-size:26px;line-height:32px;font-weight:600;letter-spacing:-.4px}')
if '/*PAGES*/' not in css:
    css+='''
/*PAGES*/
.tr{display:grid;grid-template-columns:var(--cols);gap:16px;align-items:center;min-height:44px;padding:0 16px;border-top:1px solid var(--hair);font-size:14px}
.tr.th{min-height:34px;border-top:0;font-size:12px;color:var(--ink-3)}
.tr .act{display:flex;gap:6px;justify-content:flex-end}
.mono{font-family:var(--mono);font-size:13px}
.tag{display:inline-block;font-size:12px;line-height:16px;padding:1px 6px;border-radius:5px;border:1px solid var(--hair-strong);color:var(--ink-2)}
.btn.dg{color:var(--bad)}
.chk{width:16px;height:16px;border-radius:4px;border:1px solid var(--hair-strong);background:#fff;flex:none;display:inline-block}.chk.on{background:var(--accent-text);border-color:var(--accent-text);position:relative}.chk.on::after{content:'';position:absolute;left:4.5px;top:1.5px;width:4px;height:8px;border:solid #fff;border-width:0 1.8px 1.8px 0;transform:rotate(45deg)}
.sel{position:relative;padding-right:30px}.sel::after{content:"";position:absolute;right:12px;top:50%;width:6px;height:6px;border-right:1.5px solid var(--ink-3);border-bottom:1.5px solid var(--ink-3);transform:translateY(-70%) rotate(45deg)}
.sec-h{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:8px 0 12px;min-height:28px}
.tile{display:flex;align-items:center;gap:12px;padding:12px 16px;border-radius:var(--r-inner);background:rgba(255,255,255,.62)}
.tile.on{box-shadow:inset 0 0 0 2px var(--accent)}
.stat{display:grid;grid-template-columns:repeat(6,1fr)}.stat>div{padding:4px 20px;border-left:1px solid var(--hair)}.stat>div:first-child{border-left:0;padding-left:0}
.term{background:#1e1f2b;border-radius:var(--r-card);color:#e6e6ef;font:13px/20px var(--mono);padding:16px}
.kbd{font:12px/16px var(--mono);padding:1px 6px;border-radius:5px;background:var(--fill-2)}
.sheet{border-radius:32px;padding:24px;position:absolute}
.sheet .sh{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}
.sheet .st{font-size:20px;line-height:26px;font-weight:600;letter-spacing:-.3px}
.sheet .sf{display:flex;justify-content:flex-end;gap:8px;margin-top:20px}
.fl{display:grid;gap:6px}.fl>label{font-size:13px;line-height:18px;color:var(--ink-2)}
.dim{position:absolute;inset:0;background:rgba(20,30,45,.20)}
.dis{display:flex;align-items:center;gap:8px;font-size:15px;font-weight:600;padding:12px 16px}
'''
open(P+'spec.css','w').write(css)

# ---------- icon helpers
def I(p): return f'<svg class="i" viewBox="0 0 16 16">{p}</svg>'
IC=dict(
 refresh=I('<path d="M13 8a5 5 0 1 1-1.6-3.7M13 2.5v3h-3"/>'),
 plus=I('<path d="M8 3v10M3 8h10"/>'),
 dl=I('<path d="M8 2.5v8M5 7.5l3 3 3-3M3 12.5h10"/>'),
 ul=I('<path d="M8 10.5V2.5M5 5.5l3-3 3 3M3 10.5v2h10v-2"/>'),
 edit=I('<path d="M9.5 3.5l3 3M3 13l.7-3L10.5 3.2a1.4 1.4 0 0 1 2 0l.3.3a1.4 1.4 0 0 1 0 2L6 12.3 3 13Z"/>'),
 trash=I('<path d="M3.5 4.5h9M6.5 4.5V3h3v1.5M5 4.5l.5 8h5l.5-8"/>'),
 play=I('<path d="M5.5 3.5v9l7-4.5-7-4.5Z"/>'),
 stop=I('<rect x="4" y="4" width="8" height="8" rx="1.5"/>'),
 pause=I('<path d="M6 3.5v9M10 3.5v9"/>'),
 eye=I('<path d="M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8Z"/><circle cx="8" cy="8" r="2"/>'),
 x=I('<path d="M4 4l8 8M12 4l-8 8"/>'),
 folder=I('<path d="M2.5 12.5v-8h4l1.5 1.5h5.5v6.5h-11Z"/>'),
 fplus=I('<path d="M2.5 12.5v-8h4l1.5 1.5h5.5v6.5h-11ZM8 8v3M6.5 9.5h3"/>'),
 doc=I('<path d="M4 2.5h5l3 3v8H4v-11ZM9 2.5v3h3"/>'),
 home=I('<path d="M2.5 8 8 3l5.5 5M4 7v6h8V7"/>'),
 key=I('<circle cx="5.5" cy="10.5" r="2.5"/><path d="M7.3 8.7 13 3M11 5l1.5 1.5"/>'),
 pw=I('<path d="M8 2.5v5M4.8 4.6a5 5 0 1 0 6.4 0"/>'),
 eject=I('<path d="M3.5 9.5 8 4l4.5 5.5h-9ZM3.5 12.5h9"/>'),
 clock=I('<circle cx="8" cy="8" r="5.5"/><path d="M8 4.8V8l2.2 1.4"/>'),
 term=I('<rect x="2" y="3" width="12" height="10" rx="2"/><path d="M5 7l2 1.5L5 10M8.5 10.5H11"/>'),
 info=I('<circle cx="8" cy="8" r="5.5"/><path d="M8 7.2V11M8 5v.1"/>'),
 warn=I('<path d="M8 2.5 14 13H2L8 2.5ZM8 7v3M8 11.6v.1"/>'),
 cpy=I('<rect x="5.5" y="5.5" width="8" height="8" rx="1.5"/><path d="M10.5 5.5V4a1.5 1.5 0 0 0-1.5-1.5H4A1.5 1.5 0 0 0 2.5 4v5A1.5 1.5 0 0 0 4 10.5h1.5"/>'),
 srv=I('<rect x="2.5" y="3" width="11" height="4" rx="1.5"/><rect x="2.5" y="9" width="11" height="4" rx="1.5"/><path d="M5 5h.1M5 11h.1"/>'),
 net=I('<rect x="6" y="2.5" width="4" height="3" rx=".8"/><rect x="2" y="10.5" width="4" height="3" rx=".8"/><rect x="10" y="10.5" width="4" height="3" rx=".8"/><path d="M8 5.5v2.5M4 10.5V8h8v2.5"/>'),
 wifi=I('<path d="M2 6.5a8.5 8.5 0 0 1 12 0M4.3 9a5.3 5.3 0 0 1 7.4 0M8 12h.1"/>'),
 ap=I('<circle cx="8" cy="7" r="1.2"/><path d="M5.2 4.2a4 4 0 0 0 0 5.6M10.8 4.2a4 4 0 0 1 0 5.6M8 8.5V13"/>'),
 lock=I('<rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2"/>'),
 shield=I('<path d="M8 2.5 13 4v4c0 3-2.2 4.8-5 5.5C5.2 12.8 3 11 3 8V4l5-1.5Z"/>'),
 grid=I('<rect x="3" y="3" width="4" height="4" rx="1"/><rect x="9" y="3" width="4" height="4" rx="1"/><rect x="3" y="9" width="4" height="4" rx="1"/><rect x="9" y="9" width="4" height="4" rx="1"/>'),
 box=I('<path d="M8 2.5 13.5 5v6L8 13.5 2.5 11V5L8 2.5ZM2.5 5 8 7.5 13.5 5M8 7.5v6"/>'),
)
def B(t,ic='',cls='',extra=''): return f'<button class="btn {cls}" {extra}>{IC[ic] if ic else ""}{t}</button>'
def IB(ic,label,cls='sm',extra=''): return f'<button class="btn icon {cls}" aria-label="{label}" title="{label}" {extra}>{IC[ic]}</button>'
def SW(on=True,label=''): return f'<button class="switch{" on" if on else ""}" role="switch" aria-checked="{str(on).lower()}" aria-label="{label}"></button>'
def ST(t,k=''): return f'<span class="state {k}">{t}</span>'
def SELECT(t,w=140): return f'<button class="field sel" style="width:{w}px;text-align:left">{t}</button>'
def KV(a,b): return f'<div class="kv"><span>{a}</span><span>{b}</span></div>'
def TR(cols,cells,th=False): return f'<div class="tr{" th" if th else ""}" style="--cols:{cols}">'+''.join(f'<div>{c}</div>' if not str(c).startswith('<div class="act"') else c for c in cells)+'</div>'
def ACT(*b): return '<div class="act">'+''.join(b)+'</div>'

NAV=['系统','网络','文件','终端','自动化','指令','安全']
def header(active):
    nav=''.join(f'<a class="{"on" if n==active else ""}" href="#">{n}</a>' for n in NAV)
    return f'''<div class="m-bar" style="position:absolute;top:20px;left:24px;right:24px;height:60px;border-radius:30px;display:grid;grid-template-columns:1fr auto 1fr;align-items:center;padding:0 16px"><div style="display:flex;align-items:flex-start;gap:9px;justify-self:start;margin-left:6px;height:30px"><svg viewBox="0 0 30 30" width="30" height="30" fill="none"><path d="M3 24 12 6l5 10 3-5 8 13H3Z" stroke="#293d54" stroke-width="1.8" stroke-linejoin="round"/><path d="m9 12 3 3 3-3M7 24l7-9 6 9" stroke="#293d54" stroke-width="1.2"/></svg><svg viewBox="0 -760 5636 900" height="22" aria-label="TianshanOS" fill="#1d1d1f" style="margin-top:5.4px"><path d="M561 -700V-580H356V0H236V-580H30V-700H236H356H561ZM651 0V-500H771V0ZM651 -700H771V-580H651ZM1389 -501V0H1269V-250Q1269 -310 1227 -352Q1185 -394 1125 -394Q1096 -394 1069.5 -383Q1043 -372 1023 -352Q981 -310 981 -250Q981 -191 1023 -149Q1043 -128 1069.5 -117.5Q1096 -107 1125 -107Q1155 -107 1179 -117L1222 -10Q1176 13 1115 13Q1060 13 1013.5 -7.5Q967 -28 933 -64Q899 -100 880 -148Q861 -196 861 -250Q861 -305 880 -353Q899 -401 933 -437Q967 -473 1013.5 -493.5Q1060 -514 1115 -514Q1164 -514 1202 -499Q1240 -484 1269 -461V-501ZM1766 -514Q1815 -514 1855 -495.5Q1895 -477 1923.5 -444.5Q1952 -412 1967.5 -369Q1983 -326 1983 -277V0H1863V-277Q1863 -327 1828 -360Q1794 -394 1746 -394Q1697 -394 1663 -360Q1629 -326 1629 -277V0H1509V-500H1629V-462Q1656 -485 1690 -499.5Q1724 -514 1766 -514ZM2278 -310Q2290 -307 2318 -296Q2346 -285 2375.5 -265.5Q2405 -246 2427 -217Q2449 -188 2449 -148Q2449 -113 2437.5 -84Q2426 -55 2404 -33Q2382 -11 2350 1Q2318 13 2278 13Q2217 13 2164.5 -9Q2112 -31 2080 -63L2163 -146Q2189 -115 2218.5 -106Q2248 -97 2268 -97Q2299 -97 2314 -109Q2329 -121 2329 -138Q2329 -150 2319 -159Q2309 -168 2295.5 -174Q2282 -180 2268 -183.5Q2254 -187 2245 -190Q2233 -194 2204.5 -204Q2176 -214 2146.5 -233Q2117 -252 2095 -281Q2073 -310 2073 -351Q2073 -393 2087.5 -423.5Q2102 -454 2126 -474Q2150 -494 2181 -503.5Q2212 -513 2245 -513Q2296 -513 2331.5 -501Q2367 -489 2390 -475Q2416 -458 2434 -437L2350 -353Q2330 -379 2306 -391Q2282 -403 2255 -403Q2234 -403 2213.5 -393Q2193 -383 2193 -361Q2193 -349 2202 -340.5Q2211 -332 2224 -326.5Q2237 -321 2251.5 -317Q2266 -313 2278 -310ZM2806 -514Q2855 -514 2895 -495.5Q2935 -477 2963.5 -444.5Q2992 -412 3007.5 -369Q3023 -326 3023 -277V0H2903V-277Q2903 -301 2894 -322Q2885 -343 2868 -360Q2834 -394 2786 -394Q2737 -394 2703 -360Q2669 -326 2669 -277V0H2549V-700H2669V-460Q2696 -484 2730 -499Q2764 -514 2806 -514ZM3621 -501V0H3501V-250Q3501 -310 3459 -352Q3417 -394 3357 -394Q3328 -394 3301.5 -383Q3275 -372 3255 -352Q3213 -310 3213 -250Q3213 -191 3255 -149Q3275 -128 3301.5 -117.5Q3328 -107 3357 -107Q3387 -107 3411 -117L3454 -10Q3408 13 3347 13Q3292 13 3245.5 -7.5Q3199 -28 3165 -64Q3131 -100 3112 -148Q3093 -196 3093 -250Q3093 -305 3112 -353Q3131 -401 3165 -437Q3199 -473 3245.5 -493.5Q3292 -514 3347 -514Q3396 -514 3434 -499Q3472 -484 3501 -461V-501ZM3998 -514Q4047 -514 4087 -495.5Q4127 -477 4155.5 -444.5Q4184 -412 4199.5 -369Q4215 -326 4215 -277V0H4095V-277Q4095 -327 4060 -360Q4026 -394 3978 -394Q3929 -394 3895 -360Q3861 -326 3861 -277V0H3741V-500H3861V-462Q3888 -485 3922 -499.5Q3956 -514 3998 -514ZM4659 -595Q4610 -595 4566 -576.5Q4522 -558 4487 -523Q4452 -488 4433.5 -444Q4415 -400 4415 -351Q4415 -302 4433.5 -257.5Q4452 -213 4487 -178Q4522 -143 4566 -125Q4610 -107 4659 -107Q4708 -107 4752.5 -125Q4797 -143 4832 -178Q4867 -213 4885 -257.5Q4903 -302 4903 -351Q4903 -400 4885 -444Q4867 -488 4832 -523Q4797 -558 4752.5 -576.5Q4708 -595 4659 -595ZM4659 -715Q4734 -715 4800.5 -686.5Q4867 -658 4916.5 -608.5Q4966 -559 4994.5 -492.5Q5023 -426 5023 -351Q5023 -276 4994.5 -209.5Q4966 -143 4916.5 -93.5Q4867 -44 4800.5 -15.5Q4734 13 4659 13Q4584 13 4517.5 -15.5Q4451 -44 4401.5 -93.5Q4352 -143 4323.5 -209.5Q4295 -276 4295 -351Q4295 -426 4323.5 -492.5Q4352 -559 4401.5 -608.5Q4451 -658 4517.5 -686.5Q4584 -715 4659 -715ZM5596 -200Q5596 -151 5580 -111.5Q5564 -72 5534.5 -44.5Q5505 -17 5463 -2Q5421 13 5369 13Q5286 13 5221 -13.5Q5156 -40 5114 -74L5197 -157Q5230 -127 5271 -111Q5296 -101 5318 -98Q5340 -95 5354 -95Q5380 -95 5401.5 -101.5Q5423 -108 5439 -119Q5473 -144 5473 -185Q5473 -210 5455 -230Q5441 -246 5419 -256Q5401 -266 5381 -272Q5361 -278 5345 -283Q5340 -285 5336 -285.5Q5332 -286 5329 -288Q5327 -288 5323 -290Q5302 -296 5262.5 -311Q5223 -326 5184 -352Q5093 -413 5093 -502Q5093 -561 5113.5 -601.5Q5134 -642 5167 -667.5Q5200 -693 5240.5 -704Q5281 -715 5320 -715Q5392 -715 5441 -698Q5490 -681 5519 -664Q5534 -655 5544.5 -646.5Q5555 -638 5563 -631L5478 -545Q5451 -572 5423 -586Q5381 -606 5335 -606Q5315 -606 5295 -601Q5275 -596 5260 -588Q5216 -561 5216 -517Q5216 -490 5234 -471Q5247 -456 5270 -444Q5289 -434 5310 -428Q5331 -422 5349 -417L5361 -414Q5366 -412 5380 -408Q5394 -404 5413 -396Q5432 -388 5454.5 -377Q5477 -366 5499 -351Q5596 -287 5596 -200Z"/></svg></div><nav class="nav">{nav}</nav><div style="justify-self:end;display:flex;align-items:center;gap:14px"><span class="state ok">已连接</span><button class="btn sm quiet">{IC['info'] if False else ''}中文</button><span class="t-label">root</span><button class="btn sm">退出登录</button></div></div>'''
def page(name,active,h,inner,w=1440):
    body=f'''<div class="dc wall" style="width:{w}px;height:{h}px;position:relative">{header(active)}<div class="m-surface" style="position:absolute;top:100px;left:24px;right:24px;bottom:56px;border-radius:32px;padding:16px;overflow:hidden"><div style="height:100%;overflow:hidden;display:flex;flex-direction:column;gap:16px">{inner}</div></div><div class="foot" style="position:absolute;bottom:18px;left:0;right:0">© 2026 TianshanOS · RMinte® AI Technology Co., Ltd. · www.RMinte.com</div></div>'''
    return body
def write(name,title,h,body):
    t=f'''<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="./support.js"></script>
<link rel="stylesheet" href="quantify.css">
<link rel="stylesheet" href="spec.css">
</head>
<body>
<x-dc>
<helmet>
<style>body{{margin:0}}</style>
</helmet>
{body}
</x-dc>
<script type="text/x-dc" data-dc-script data-props='{{"$preview":{{"width":1440,"height":{h}}}}}'>
class Component extends DCLogic {{
renderVals() {{ return {{}}; }}
}}
</script>
</body>
</html>
'''
    open(P+name+'.dc.html','w').write(t)
    open('dc/t-'+name+'.html','w').write('<!doctype html><meta charset="utf-8"><link rel="stylesheet" href="project/quantify.css"><link rel="stylesheet" href="project/spec.css"><style>body{margin:0}</style>'+body)
CARD='class="card"'
def sec(title,btns=''): return f'<div class="sec-h"><span class="t-section" style="font-size:17px">{title}</span><div style="display:flex;gap:8px">{btns}</div></div>'
BOARDS={}

# ================= 网络
def net():
    st=lambda ic,n,s,k,v:f'<div class="tile" style="padding:16px"><span style="color:var(--ink-2)">{IC[ic].replace("class=\"i\"","class=\"i\" style=\"width:24px;height:24px\"")}</span><div style="flex:1"><div class="t-section">{n}</div><div style="margin-top:2px">{ST(s,k)}</div></div><span class="num t-value">{v}</span></div>'
    top=f'<div style="display:grid;grid-template-columns:repeat(3,1fr);gap:16px">{st("net","以太网","已连接","ok","192.0.2.10")}{st("wifi","WiFi 客户端","未连接","","-")}{st("ap","WiFi 热点","未启用","","-")}</div>'
    left=f'''<div>{sec("接口配置",'<div class="seg"><button class="on">以太网</button><button>WiFi</button></div>')}<div class="card">{KV("链路状态",ST("已连接","ok"))}{KV("IP 地址","192.0.2.10")}{KV("子网掩码","255.255.255.0")}{KV("网关","192.0.2.1")}{KV("DNS","-")}{KV("MAC 地址",'<span class="mono">02:00:00:00:00:01</span>')}</div>
<div class="t-note" style="margin:16px 0 8px">切换到「WiFi」标签时显示：</div><div class="card" style="display:grid;gap:12px"><div style="display:flex;align-items:center;justify-content:space-between"><span class="t-label">WiFi 模式</span>{SELECT("关闭",120)}</div><div style="display:flex;gap:8px;justify-content:flex-end">{B("扫描","refresh","sm")}{B("断开","","sm")}</div><hr class="sep"><div style="display:flex;align-items:center;justify-content:space-between"><span class="t-label">WiFi 热点</span><div style="display:flex;gap:8px">{B("配置","","sm")}{B("设备","","sm")}</div></div></div></div>'''
    right=f'''<div>{sec("网络服务")}<div class="card" style="display:grid;gap:0">
<div style="padding-bottom:16px"><div style="display:flex;justify-content:space-between;margin-bottom:8px"><span class="t-section">主机名</span><span class="t-note">-</span></div><div style="display:flex;gap:8px"><input class="field" style="flex:1" placeholder="新主机名" aria-label="新主机名">{B("设置")}</div></div><hr class="sep">
<div style="padding:16px 0"><div style="display:flex;justify-content:space-between;align-items:center"><span class="t-section">DHCP 服务器</span>{ST("运行中","ok")}</div><div style="display:flex;justify-content:space-between;align-items:center;margin-top:10px"><span class="t-label">0 活跃租约</span>{B("客户端","","sm")}</div></div><hr class="sep">
<div style="padding-top:16px"><div style="display:flex;justify-content:space-between;align-items:center"><span class="t-section">NAT 网关</span>{ST("已停止","")}</div><div style="display:flex;align-items:center;gap:16px;margin-top:12px"><span class="t-label">WiFi <b class="t-value" style="color:var(--ink-3)">✕</b></span><span class="t-label">以太网 <b class="t-value" style="color:var(--ink-3)">✕</b></span><span style="flex:1"></span><span class="t-label">启用</span>{SW(False,"启用 NAT")}{B("保存","","sm")}</div></div></div></div>'''
    up=f'<div class="card" style="display:flex;align-items:center;justify-content:space-between;padding:16px 20px"><div><div class="t-section">接入上层网络</div><div style="margin-top:4px">{ST("未启动","")}</div></div>{B("通过 LPMU 接入","","sm")}</div>'
    inner=top+f'<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;flex:1">{left}{right}</div>'+up
    write('Net','网络页 目标稿',940,page('Net','网络',940,inner));BOARDS['Net']=940
net()

# ================= 文件
def files():
    bar=f'<div style="display:flex;align-items:center;justify-content:space-between"><div style="display:flex;align-items:center;gap:8px">{SELECT(IC["home"]+" /",84)}<span class="t-note">/</span>{SELECT("sdcard",120)}</div><div style="display:flex;gap:8px">{B("上传文件","ul","sm")}{B("新建文件夹","fplus","sm")}{IB("refresh","刷新")}</div></div>'
    tabs=f'<div style="display:flex;align-items:center;justify-content:space-between"><div class="seg"><button class="on">SD 卡</button><button>SPIFFS</button></div>{B("卸载 SD","eject","sm dg")}</div>'
    cols='24px 1fr 120px 132px'
    tbl=f'<div class="card" style="padding:0;overflow:hidden">'+TR(cols,['<span class="chk"></span>','名称','大小','<div style="text-align:right">操作</div>'],True)+TR(cols,['<span class="chk"></span>',f'<span style="display:flex;align-items:center;gap:8px;color:var(--accent-text)">{IC["folder"]}images</span>','-',ACT(IB("edit","重命名"),IB("trash","删除",'sm dg'))])+TR(cols,['<span class="chk on"></span>',f'<span style="display:flex;align-items:center;gap:8px">{IC["doc"]}fixture.txt</span>','1.0 KB',ACT(IB("dl","下载"),IB("edit","重命名"),IB("trash","删除",'sm dg'))])+'</div>'
    sel=f'<div class="m-float" style="border-radius:22px;padding:8px 8px 8px 20px;display:flex;align-items:center;gap:12px;align-self:center"><span class="t-value">已选 1 项</span><span style="width:1px;height:20px;background:var(--hair)"></span>{B("批量下载","dl","sm")}{B("批量删除","trash","sm dg")}{B("取消选择","","sm quiet")}</div>'
    stat=f'<div style="display:flex;gap:20px;padding:0 4px"><span class="t-label">SD {ST("已挂载","ok")}</span><span class="t-label">SPIFFS {ST("已挂载","ok")}</span></div>'
    inner=bar+tabs+tbl+sel+stat
    write('Files','文件页 目标稿',560,page('Files','文件',560,inner));BOARDS['Files']=560
files()

# ================= 终端
def term():
    tb=f'<div style="display:flex;gap:8px">{B("系统日志","doc","sm")}{B("清屏","trash","sm")}{B("断开","","sm dg")}</div>'
    t='<div class="term" style="flex:1"><div style="border:1px solid #57c7c0;padding:6px 40px;display:inline-block;color:#ffd98a;font-weight:700">TianshanOS Web Terminal</div><div style="margin-top:28px">正在连接到设备...</div><div style="color:#8ee59a">已连接到设备</div><div>输入 <b style="color:#ffd98a">help</b> 查看可用命令</div><div style="margin-top:12px">fixture&gt; <span style="display:inline-block;width:8px;height:16px;vertical-align:-3px;border:1px solid #e6e6ef"></span></div></div>'
    hint=f'<div class="t-note" style="display:flex;align-items:center;gap:8px">{IC["info"]} 提示：输入 <span class="kbd">help</span> 查看命令 · <span class="kbd">Ctrl+C</span> 中断 · <span class="kbd">Ctrl+L</span> 清屏 · <span class="kbd">↑↓</span> 历史</div>'
    write('Term','终端页 目标稿',860,page('Term','终端',860,tb+t+hint));BOARDS['Term']=860
term()

# ================= 自动化
def auto():
    ctl=f'<div style="display:flex;gap:8px">{B("启动","play","sm pw ok")}{B("停止","stop","sm pw bad")}{B("暂停","pause","sm")}{B("重载","refresh","sm")}</div>'
    cell=lambda v,l:f'<div><div class="t-value" style="font-size:24px;line-height:30px;font-weight:500">{v}</div><div class="t-label" style="margin-top:2px">{l}</div></div>'
    stat=f'<div class="card stat" style="padding:16px 20px">{cell('<span style="display:inline-flex;align-items:center;gap:8px;color:var(--ok)"><i style="width:9px;height:9px;border-radius:50%;background:var(--ok-dot)"></i>运行中</span>',"引擎状态")}{cell("1","规则")}{cell("1","变量")}{cell("1","数据源")}{cell("2","触发次数")}{cell("1 时 0 分","运行时长")}</div>'
    hb=lambda: B("添加","plus","sm")+B("导入","dl","sm")+IB("refresh","刷新")
    c1='1.2fr 1.2fr .8fr 1fr .8fr 156px'
    src=f'{sec("数据源",hb())}<div class="card" style="padding:0">'+TR(c1,['ID','标签','类型','启用','更新间隔','<div style="text-align:right">操作</div>'],True)+TR(c1,['<span class="mono">fixture-source</span>','Fixture source','http',SW(True,"启用数据源"),'5 秒',ACT(IB("eye","查看变量"),IB("dl","导出配置包"),IB("trash","删除",'sm dg'))])+'</div>'
    c2='1.2fr 1.4fr 1fr .6fr .6fr .7fr 196px'
    rule=f'{sec("规则列表",hb())}<div class="card" style="padding:0">'+TR(c2,['ID','名称','启用','条件','动作','触发次数','<div style="text-align:right">操作</div>'],True)+TR(c2,['<span class="mono">fixture-rule</span>','Fixture rule <span class="tag" style="margin-left:6px">手动</span>',SW(True,"启用规则"),'1','1','2',ACT(IB("play","手动触发"),IB("edit","编辑"),IB("dl","导出配置包"),IB("trash","删除",'sm dg'))])+'</div>'
    c3='1.2fr 1.2fr .8fr .9fr 1.4fr 196px'
    act=f'{sec("动作模板",hb())}<div class="card" style="padding:0">'+TR(c3,['ID','名称','类型','模式','描述','<div style="text-align:right">操作</div>'],True)+TR(c3,['<span class="mono">fixture-action</span>','Fixture action','日志','异步执行','Synthetic UI data',ACT(IB("play","测试"),IB("edit","编辑"),IB("dl","导出配置包"),IB("trash","删除",'sm dg'))])+'</div>'
    write('Auto','自动化页 目标稿',900,page('Auto','自动化',900,ctl+stat+f'<div>{src}</div><div>{rule}</div><div>{act}</div>'));BOARDS['Auto']=900
auto()

# ================= 指令
def cmds():
    hd=f'{sec("选择主机",B("导入指令","dl","sm")+B("新建指令","plus","sm"))}'
    hosts=f'<div style="display:flex;gap:12px"><div class="tile on" style="width:280px">{IC["srv"]}<div><div class="t-body" style="font-weight:600">fixture-host</div><div class="t-note mono">fixture@192.0.2.30:22</div></div></div><div class="tile" style="width:300px;background:rgba(255,149,0,.10)"><span style="color:var(--warn)">{IC["warn"]}</span><div><div class="t-body" style="font-weight:600">孤儿命令</div><div class="t-note" style="color:var(--warn)">1 个命令关联的主机已不存在</div></div></div></div>'
    lst=f'''{sec("命令列表")}<div class="card" style="padding:0"><div class="tr" style="--cols:32px 1fr 1.6fr 96px"><span style="color:var(--accent-text)">{IC["play"]}</span><div><div class="t-body" style="font-weight:500">重启服务</div><div class="t-note mono">restart_nginx</div></div><span class="mono t-label">sudo systemctl restart nginx</span>{ACT(IB("play","执行"),IB("edit","编辑"),IB("trash","删除","sm dg"))}</div></div>
<div class="t-note" style="margin-top:8px">空状态：「请先选择一个主机」+ {B("创建第一个指令","plus","sm")}</div>'''
    exe=f'''<div class="card" style="display:grid;gap:12px"><div style="display:flex;align-items:center;justify-content:space-between"><span class="t-section">执行结果</span><div style="display:flex;gap:8px;flex-wrap:wrap">{B("取消 (Esc)","","sm dg")}{B("清除","","sm")}<span style="width:1px;background:var(--hair);margin:0 4px"></span>{B("查看日志","","sm")}{B("实时跟踪","","sm")}{B("停止跟踪","","sm")}{B("检查进程","","sm")}{B("停止进程","","sm dg")}</div></div><div class="term" style="height:96px;padding:12px">$ sudo systemctl restart nginx<br>&nbsp;</div></div>'''
    write('Cmds','指令页 目标稿',760,page('Cmds','指令',760,hd+hosts+f'<div>{lst}</div>'+exe));BOARDS['Cmds']=760
cmds()

# ================= OTA
def ota():
    f=lambda l,v,w='':f'<div style="display:flex;align-items:center;gap:12px"><span class="t-label" style="width:96px">{l}</span>{v}</div>'
    cur=f'''<div class="card"><div style="display:flex;align-items:baseline;gap:12px"><span class="t-label">当前版本</span><span class="t-value" style="font-size:20px;line-height:26px">BASELINE-FIXTURE</span></div><div class="t-note" style="margin-top:4px">TianshanOS · · IDF</div><hr class="sep" style="margin:16px 0"><div style="display:flex;align-items:center;gap:8px"><span class="t-label">OTA 服务器</span><input class="field" style="flex:1" value="https://fixture.invalid/firmware" aria-label="OTA 服务器">{B("保存")}{B("检查更新","","primary")}</div></div>'''
    part=lambda n,s,k,nm,addr,btn,cap:f'<div class="w-tile" style="padding:16px;display:grid;gap:10px"><div style="display:flex;justify-content:space-between;align-items:center"><span class="mono" style="font-weight:600">{n}</span>{ST(s,k)}</div><div class="t-body" style="font-weight:500">{nm}</div><div class="t-note num">{addr}</div>{btn}<div class="t-note" style="text-align:center">{cap}</div></div>'
    parts=f'''<div class="card" style="padding:0"><div class="dis">{IC["refresh"].replace("<path","<path transform=\'rotate(0)\'",1) if False else ""}<span style="display:inline-block;transform:rotate(90deg);color:var(--ink-3)">›</span>分区管理</div><div style="padding:0 16px 16px;display:grid;grid-template-columns:1fr 1fr;gap:12px">{part("ota_0","运行中","ok","FIXTURE-A","0x00010000 · 3.00 MB",B("标记有效","","","style='width:100%'"),"取消自动回滚保护")}{part("ota_1","可启动","warn","FIXTURE-B","0x00310000 · 3.00 MB",B("回滚到此版本","","dg","style='width:100%'"),"重启后加载此分区")}</div></div>'''
    man=f'''<div class="card" style="padding:0"><div class="dis"><span style="display:inline-block;transform:rotate(90deg);color:var(--ink-3)">›</span>手动升级</div><div style="padding:0 16px 16px;display:grid;gap:12px"><div class="fl"><label>从 URL 升级</label><div style="display:flex;gap:8px"><input class="field" style="flex:1" placeholder="http://example.com/firmware.bin" aria-label="固件 URL">{B("升级","ul","primary")}</div><div style="display:flex;gap:20px;font-size:13px"><span style="display:flex;gap:6px;align-items:center"><span class="chk"></span>同时升级 www</span><span style="display:flex;gap:6px;align-items:center"><span class="chk"></span>跳过证书校验</span></div></div><hr class="sep"><div class="fl"><label>从文件升级</label><div style="display:flex;gap:8px"><input class="field" style="flex:1" placeholder="/sdcard/firmware.bin" aria-label="固件文件">{B("升级","ul","primary")}</div><span style="display:flex;gap:6px;align-items:center;font-size:13px"><span class="chk"></span>同时升级 www</span></div><hr class="sep"><div style="display:flex;align-items:center;gap:12px"><div class="bar" style="flex:1"><i style="width:35%"></i></div><span class="t-value num">35%</span>{B("中止","stop","sm dg")}</div></div></div>'''
    inner=f'<div style="width:720px;margin:0 auto;display:grid;gap:16px"><span class="t-title">固件升级</span>{cur}{parts}{man}</div>'
    write('Ota','固件升级页 目标稿',1000,page('Ota','系统',1000,inner));BOARDS['Ota']=1000
ota()

# ================= 安全
def sec_():
    def pw(role):
        fld=lambda l,p:f'<div class="fl" style="flex:1"><label>{l}</label><div style="position:relative"><input class="field lg" style="width:100%;padding-right:40px" placeholder="{p}" type="password" aria-label="{l}"><span style="position:absolute;right:10px;top:10px;color:var(--ink-3)">{IC["eye"]}</span></div></div>'
        return f'<div class="card"><div class="t-section">{role} 密码管理</div><div class="t-note" style="margin:2px 0 12px">设置 {role} 新密码不会影响当前已登录会话。</div><div style="display:flex;gap:12px;align-items:flex-end">{fld(role+" 新密码","输入 "+role+" 新密码")}{fld("确认新密码","再次输入新密码")}{B("设置 "+role+" 新密码","","lg primary")}</div></div>'
    acct=f'{sec("账号安全")}<div class="t-note" style="display:flex;gap:6px;margin-bottom:12px">{IC["info"]}root 可在此管理 root 与 admin 账号密码。</div><div style="display:grid;gap:12px">{pw("root")}{pw("admin")}<div class="card" style="display:flex;align-items:center;justify-content:space-between;padding:16px 20px"><div><div class="t-section">危险操作</div><div class="t-note" style="margin-top:2px">将 admin 密码恢复为默认密码 rm01，并清除登录锁定状态。</div></div>{B("重置 admin 为默认密码","refresh","dg")}</div></div>'
    ck='1fr .7fr 1.5fr .6fr .6fr 330px'
    keys=f'{sec("密钥管理",B("生成新密钥","plus","sm"))}<div class="card" style="padding:0">'+TR(ck,['ID','类型','备注','创建时间','可导出','<div style="text-align:right">操作</div>'],True)+TR(ck,['<span class="mono">fixture-key</span>','ed25519','<span class="tag">SSH</span> Synthetic key row only','-','是',ACT(B("公钥","dl","sm"),B("私钥","key","sm"),B("部署","ul","sm"),B("撤销","","sm dg"),B("删除","","sm dg"))])+TR(ck,['<span class="mono">https</span>','-','<span class="tag">HTTPS</span> <i class="t-note">未生成密钥</i>','-','-',ACT(B("生成密钥","key","sm"))])+'</div>'
    ch='1fr 1.1fr .5fr .8fr 1fr 250px'
    hosts=f'{sec("已部署主机",B("导入主机","dl","sm"))}<div class="t-note" style="display:flex;gap:6px;margin-bottom:8px">{IC["info"]}通过上方密钥的「部署」按钮将公钥部署到远程服务器后，主机将自动出现在此列表</div><div class="card" style="padding:0">'+TR(ch,['主机 ID','地址','端口','用户名','部署密钥','<div style="text-align:right">操作</div>'],True)+TR(ch,['<span class="mono">fixture-host</span>','192.0.2.30','22','fixture','<span class="tag">default</span>',ACT(B("测试","","sm"),B("导出","","sm"),B("撤销","","sm dg"),B("移除","","sm dg"))])+'</div>'
    cf='1fr .5fr .8fr 2fr 1fr .6fr'
    fp=f'{sec("已知主机指纹")}<div class="card" style="padding:0">'+TR(cf,['主机','端口','密钥类型','指纹 (SHA256)','添加时间','操作'],True)+f'<div class="tr" style="--cols:1fr;color:var(--ink-3)">暂无已知主机指纹</div></div>'
    cert=f'''{sec("HTTPS 证书")}<div class="card"><div class="t-section">未知／待确认</div><div style="margin-top:8px;display:grid;gap:2px;font-size:13px;color:var(--ink-2)"><span>私钥：缺少 · 设备证书：缺少 · 客户端验证 CA：缺少</span><span>HTTPS：未运行 (443)</span><span>设备时间：— · ntp · 已同步</span></div><div style="margin-top:12px">{B("用当前电脑时间校时（来源：浏览器）","clock","sm")}</div><div class="t-note" style="margin-top:8px;font-style:italic">尚未生成密钥对，请先点击下方按钮生成</div><hr class="sep" style="margin:16px 0"><div style="display:flex;gap:8px;flex-wrap:wrap">{B("生成密钥对","key","sm")}{B("生成 CSR","doc","sm")}{B("安装证书","ul","sm")}{B("安装 CA","shield","sm")}{B("查看证书","eye","sm")}{B("删除凭证","trash","sm dg")}</div></div>'''
    pack=f'''{sec("配置包")}<div class="card"><div style="display:flex;justify-content:space-between;align-items:center"><span class="t-section">Developer 设备</span><span class="tag">Developer</span></div><div style="display:grid;grid-template-columns:repeat(4,1fr);gap:24px;margin-top:8px"><div>{KV("设备类型","Developer")}</div><div>{KV("证书 CN","fixture.invalid")}</div><div>{KV("证书指纹","-")}</div><div>{KV("格式版本","1")}</div></div><div class="t-note" style="display:flex;gap:6px;margin:8px 0 16px">{IC["info"]}配置包系统允许安全地加密和签名配置文件，用于设备间配置分发</div><hr class="sep" style="margin-bottom:16px"><div style="display:flex;gap:8px;flex-wrap:wrap">{B("导出设备证书","dl","sm")}{B("导入配置包","ul","sm")}{B("导出配置包","dl","sm")}{B("查看配置包列表","doc","sm")}</div></div>'''
    inner=f'<div>{acct}</div><div>{keys}</div><div>{hosts}</div><div>{fp}</div><div>{cert}</div><div>{pack}</div>'
    H=1740
    body=f'''<div class="dc wall" style="width:1440px;height:{H}px;position:relative">{header("安全")}<div class="m-surface" style="position:absolute;top:100px;left:24px;right:24px;bottom:56px;border-radius:32px;padding:16px 20px;overflow:hidden"><div style="display:grid;gap:20px">{inner}</div></div><div class="foot" style="position:absolute;bottom:18px;left:0;right:0">© 2026 TianshanOS · RMinte® AI Technology Co., Ltd. · www.RMinte.com</div></div>'''
    write('Sec','安全页 目标稿',H,body);BOARDS['Sec']=H
sec_()

# ================= 弹窗板 1（系统）
def sheet(x,y,w,title,body,foot,extra=''):
    cl='' if '取消' in foot else IB("x","关闭","icon round","style=\'border-color:transparent;background:var(--fill-2)\'")
    return f'<div class="sheet m-float" style="left:{x}px;top:{y}px;width:{w}px"><div class="sh"><span class="st">{title}</span>{cl}</div>{body}<div class="sf">{foot}</div></div>'
def num(l,v,u,ex='<span class="t-note">&nbsp;</span>'): return f'<div class="fl"><label>{l}</label><div style="display:flex;align-items:center;gap:8px"><input class="field" style="width:110px" value="{v}" aria-label="{l}"><span class="t-note">{u}</span></div>{ex}</div>'
def wallwrap(n,h,inner,title):
    body=f'<div class="dc wall" style="width:1440px;height:{h}px;position:relative;overflow:hidden"><div class="dim" style="background:rgba(20,30,45,.14)"></div>{inner}</div>'
    write(n,title,h,body);BOARDS[n]=h
foot=lambda a,b:B(a,'','lg')+B(b,'','lg primary')
s1=''
s1+=sheet(60,60,520,'服务状态',f'<div style="display:grid;gap:4px">{TR("1fr auto",["HTTP 服务",ST("运行中","ok")+"&nbsp;&nbsp;"+B("重启","","sm")])}{TR("1fr auto",["LED 服务",ST("运行中","ok")+"&nbsp;&nbsp;"+B("重启","","sm")])}</div>',B('关闭','','lg primary'))
s1+=sheet(640,60,600,'关机设置',f'<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">{num("低电压阈值",12.6,"V",'<span class="t-note">低于此值开始倒计时</span>')}{num("恢复电压阈值",18.0,"V")}{num("关机倒计时",60,"秒",'<span class="t-note">范围 10–600 秒</span>')}{num("恢复保持时间",5,"秒")}{num("风扇停止延迟",60,"秒")}</div>',B('恢复默认','','lg dg')+B('取消','','lg')+B('保存','','lg primary'))
s1+=sheet(60,360,520,'设置时区',f'<div style="display:grid;gap:14px"><div class="fl"><label>时区</label>{SELECT("Asia/Shanghai (CST-8)",472).replace("width:472px","width:100%")}</div><div class="fl"><label>自定义时区</label><input class="field" placeholder="例如 CST-8" aria-label="自定义时区"></div></div>',foot('取消','应用'))
s1+=sheet(640,520,720,'组件管理',f'''<div class="fl" style="margin-bottom:16px"><label>面板设置</label><div style="display:flex;align-items:center;gap:8px"><span class="t-body">自动刷新间隔</span>{SELECT("5 秒",100)}</div></div><div class="t-section" style="margin-bottom:8px">已添加组件</div><div class="card" style="padding:0;background:var(--fill)">{TR("24px 1fr 130px",["<span class='t-note'>⋮⋮</span>","Fixture ring · 环形",ACT(IB("edit","编辑"),IB("trash","删除","sm dg"))])}{TR("24px 1fr 130px",["<span class='t-note'>⋮⋮</span>","Fixture number · 数值",ACT(IB("edit","编辑"),IB("trash","删除","sm dg"))])}</div><div style="margin-top:12px">{B("添加组件","plus","sm")}</div>''',B('关闭','','lg primary'))
s1+=sheet(60,720,520,'自动模式有什么不同？',f'<div class="t-body" style="color:var(--ink-2)">「自动」按内置温度—转速映射运行；「曲线」使用你在曲线管理中定义的节点，并可绑定温度变量。</div>',B('知道了','','lg primary'))
wallwrap('Sheets1',1180,s1,'系统页弹窗 目标稿')

# ================= 弹窗板 2（风扇曲线 / LED / 文件 / 密钥 / 指令）
s2=''
pt=lambda t,d:f'<div style="display:flex;gap:8px;align-items:center"><input class="field sm" style="width:72px" value="{t}" aria-label="温度"><span class="t-note">°C →</span><input class="field sm" style="width:72px" value="{d}" aria-label="转速"><span class="t-note">%</span><span style="flex:1"></span>{IB("trash","删除点","sm dg")}</div>'
s2+=sheet(40,40,760,'风扇曲线管理',f'''<div style="display:grid;gap:14px"><div style="display:flex;align-items:center;gap:8px"><span class="t-label">选择风扇</span>{SELECT("风扇 0",120)}</div>
<div class="card" style="background:var(--fill)"><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:10px"><span class="t-section">温度变量绑定</span>{ST("未绑定","")}</div><div style="display:flex;gap:8px">{SELECT("选择变量",200)}{B("添加","plus","sm")}{B("绑定","","sm")}{B("解除绑定","","sm dg")}</div><div class="fl" style="margin-top:10px"><label>公式</label><input class="field mono" placeholder="max(cpu_temp, gpu_temp)" aria-label="公式"></div></div>
<div style="display:flex;justify-content:space-between;align-items:center"><span class="t-section">曲线节点</span>{B("添加节点","plus","sm")}</div>{pt(40,30)}{pt(60,55)}{pt(80,100)}
<div class="fl"><label>曲线预览</label><div style="height:110px;border-radius:12px;background:var(--fill);position:relative"><svg viewBox="0 0 300 100" preserveAspectRatio="none" style="position:absolute;inset:8px;width:calc(100% - 16px);height:calc(100% - 16px)"><path d="M0 80 L100 50 L200 30 L300 5" fill="none" stroke="#007aff" stroke-width="2"/></svg></div></div>
<div style="display:grid;grid-template-columns:repeat(3,1fr);gap:12px">{num("最小转速",20,"%")}{num("最大转速",100,"%")}{num("温度回差",2,"°C")}</div></div>''',foot('取消','保存曲线'))
s2+=sheet(860,40,540,'上传文件',f'<div style="border:1.5px dashed var(--hair-strong);border-radius:16px;height:110px;display:grid;place-items:center;color:var(--ink-2)"><div style="text-align:center">{IC["ul"]}<div class="t-label" style="margin-top:6px">选择文件，或拖放到此处</div></div></div>',foot('取消','上传'))
s2+=sheet(40,860,440,'新建文件夹',f'<input class="field lg" placeholder="输入文件夹名称" aria-label="文件夹名称">',foot('取消','创建'))
s2+=sheet(520,860,440,'重命名',f'<input class="field lg" value="fixture.txt" aria-label="新名称">',foot('取消','确认'))
s2+=sheet(1000,860,400,'删除 fixture.txt？',f'<div class="t-body" style="color:var(--ink-2)">此操作无法撤销。</div>',B('取消','','lg')+B('删除','','lg primary danger-solid','style="background:var(--bad);border-color:var(--bad)"'))
s2+=sheet(40,1200,620,'生成新密钥',f'<div style="display:grid;grid-template-columns:1fr 1fr;gap:14px"><div class="fl"><label>密钥 ID</label><input class="field" placeholder="如: default, mykey"></div><div class="fl"><label>类型</label>{SELECT("RSA 2048-bit (推荐)",280).replace("width:280px","width:100%")}</div><div class="fl"><label>备注</label><input class="field" placeholder="如: TianshanOS@device"></div><div class="fl"><label>别名</label><input class="field" placeholder="用于替代密钥 ID 显示"></div></div><div style="display:flex;gap:24px;margin-top:14px;font-size:14px"><span style="display:flex;gap:8px;align-items:center">{SW(True,"可导出")}可导出</span><span style="display:flex;gap:8px;align-items:center">{SW(False,"隐藏")}隐藏</span></div>',foot('取消','生成'))
s2+=sheet(700,1200,700,'部署公钥',f'<div style="display:grid;grid-template-columns:1fr 1fr 100px;gap:14px"><div class="fl"><label>主机</label><input class="field" placeholder="192.168.55.100 或 hostname"></div><div class="fl"><label>用户名</label><input class="field" value="root"></div><div class="fl"><label>端口</label><input class="field" value="22"></div></div><div class="fl" style="margin-top:14px"><label>SSH 登录密码</label><input class="field" type="password" placeholder="输入 SSH 登录密码"></div>',foot('取消','开始部署'))
s2+=sheet(700,1540,700,'新建指令',f'<div style="display:grid;grid-template-columns:1fr 1fr;gap:14px"><div class="fl"><label>指令 ID</label><input class="field" placeholder="例如：restart_nginx"></div><div class="fl"><label>名称</label><input class="field" placeholder="例如：重启服务"></div></div><div class="fl" style="margin-top:14px"><label>命令</label><textarea class="field mono" style="height:64px;padding:8px 12px" placeholder="例如：sudo systemctl restart nginx"></textarea></div><div class="fl" style="margin-top:14px"><label>图标</label><div style="display:flex;gap:8px;align-items:center"><div class="seg"><button class="on">图标</button><button>图片</button></div>{IB("play","图标 1","btn icon")}{IB("refresh","图标 2","btn icon")}{IB("box","图标 3","btn icon")}<span class="t-note">…共 12 个预设</span></div></div><div style="display:flex;gap:24px;margin:14px 0 0;font-size:14px"><span style="display:flex;gap:8px;align-items:center">{SW(False,"nohup")}后台运行 (nohup)</span><span style="display:flex;gap:8px;align-items:center">{SW(False,"服务模式")}服务模式</span></div><div class="t-note" style="margin-top:10px">展开：就绪匹配 · 失败匹配 · 超时/间隔 · 变量名与提取正则 · 命中即停止</div>',foot('取消','保存'))
wallwrap('Sheets2',2160,s2,'其它弹窗 目标稿')

# ================= canvas
c=json.load(open(P+'canvas.json'))
pos={'Main':(0,0,1320),'Modal':(1520,0,1320),'Sheets1':(3040,0,1180),'Net':(0,1428,940),'Files':(1520,1428,560),'Term':(3040,1428,860),'Auto':(0,2568,900),'Cmds':(1520,2568,760),'Ota':(3040,2568,1000),'Sec':(0,3728,1740),'Spec':(1520,3728,1240),'Sheets2':(3040,4260,2160)}
ttl={'Main':'系统页 目标稿','Modal':'内存详情弹窗 目标稿','Sheets1':'系统页弹窗 目标稿','Net':'网络页 目标稿','Files':'文件页 目标稿','Term':'终端页 目标稿','Auto':'自动化页 目标稿','Cmds':'指令页 目标稿','Ota':'固件升级页 目标稿','Sec':'安全页 目标稿','Spec':'控件与材质规范','Sheets2':'其它弹窗 目标稿'}
c['boards']={k+'.dc.html':{'x':x,'y':y,'w':1440,'h':h,'title':ttl[k]} for k,(x,y,h) in pos.items()}
c['order']=[k+'.dc.html' for k in pos]
c['notes']['t1']['maxW']=4480
json.dump(c,open(P+'canvas.json','w'),ensure_ascii=False,indent=1)
# modal sheet footer buttons -> lg
m=open(P+'Modal.dc.html').read()
m=m.replace('<button class="btn">关闭</button><button class="btn primary">','<button class="btn lg">关闭</button><button class="btn lg primary">')
open(P+'Modal.dc.html','w').write(m)
print(BOARDS)
