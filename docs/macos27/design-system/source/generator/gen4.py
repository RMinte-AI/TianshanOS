exec(open('gen3.py').read())
UN=lambda u:f'<span class="t-note" style="width:22px">{u}</span>'
NUM=lambda v,u,w=90:FI(v,'',w,'num')+UN(u)
# ---------- Sheets1
svc=flow('服务状态',GRP(ROWF('HTTP 服务',ST('运行中','ok')+B('重启','','sm')),ROWF('LED 服务',ST('运行中','ok')+B('重启','','sm'))),B('关闭','','lg primary'),w=520)
shut=flow('关机设置',GRP(ROWF('低电压阈值',NUM('12.6','V'),'电压低于此值开始关机倒计时（默认 12.6 V）'),ROWF('恢复电压阈值',NUM('18.0','V')),ROWF('关机倒计时',NUM('60','秒'),'低电压后等待多久执行关机（默认 60 秒，范围 10–600 秒）'),ROWF('恢复保持时间',NUM('5','秒')),ROWF('风扇停止延迟',NUM('60','秒'))),B('恢复默认','','lg')+B('取消','','lg')+B('保存','','lg primary'),w=600)
tz=flow('设置时区',GRP(ROWF('时区',SELECT('Asia/Shanghai (CST-8)',210)),ROWF('自定义时区',FI('','例如 CST-8',210,'mono'))),B('取消','','lg')+B('应用','','lg primary'),w=520)
wm=flow('组件管理',GRP(ROWF('自动刷新间隔',SELECT('5 秒',110)),title='面板设置')+'<div class="gt">已添加组件</div><div class="grp">'+ROWF('<span class="t-note">⋮⋮</span>&nbsp; Fixture ring · 环形',IB('edit','编辑','sm')+IB('trash','删除','sm dg'))+ROWF('<span class="t-note">⋮⋮</span>&nbsp; Fixture number · 数值',IB('edit','编辑','sm')+IB('trash','删除','sm dg'))+'</div>'+f'<div style="margin-top:12px">{B("添加组件","plus","sm")}</div>',B('关闭','','lg primary'),w=600)
hlp=flow('自动模式有什么不同？','<div class="t-body" style="color:var(--ink-2);line-height:22px">「自动」按内置温度—转速映射运行；「曲线」使用你在曲线管理中定义的节点，并可绑定温度变量。</div>',B('知道了','','lg primary'),w=520)
nodes=lambda: GRP(*[ROWF(f'节点 {i+1}',FI(t,'',70,'num')+'<span class="t-note">°C →</span>'+FI(d,'',70,'num')+UN('%')+IB('trash','删除节点','sm dg')) for i,(t,d) in enumerate([('40','30'),('60','55'),('80','100')])])
curve=flow('风扇曲线管理',GRP(ROWF('风扇',SELECT('风扇 0',120)))+GRP(ROWF('状态',ST('未绑定','')),ROWF('温度变量',SELECT('选择变量',170)+B('添加','plus','sm')),ROWF('公式',FI('','max(cpu_temp, gpu_temp)',220,'mono')),ROWF('',B('绑定','','sm')+B('解除绑定','','sm dg')),title='温度变量绑定')+f'<div class="sec-h" style="margin:18px 4px 6px"><span class="t-label">曲线节点</span>{B("添加节点","plus","sm")}</div>'+nodes()+'<div class="gt">曲线预览</div><div style="height:100px;border-radius:12px;background:rgba(255,255,255,.62);position:relative"><svg viewBox="0 0 300 100" preserveAspectRatio="none" style="position:absolute;inset:10px;width:calc(100% - 20px);height:calc(100% - 20px)"><path d="M0 80 L100 50 L200 30 L300 5" fill="none" stroke="#007aff" stroke-width="2"/></svg></div>'+GRP(ROWF('最小转速',NUM('20','%')),ROWF('最大转速',NUM('100','%')),ROWF('温度回差',NUM('2','°C')),title='限制'),B('取消','','lg')+B('保存曲线','','lg primary'),w=660)
board('Sheets1','系统页弹窗 目标稿',two(svc+shut+tz,wm+hlp+curve))
# ---------- Sheets2
upl=flow('上传文件','<div style="border:1.5px dashed var(--hair-strong);border-radius:16px;height:110px;display:grid;place-items:center;color:var(--ink-2)"><div style="text-align:center">'+IC['ul']+'<div class="t-label" style="margin-top:6px">选择文件，或拖放到此处</div></div></div>',B('取消','','lg')+B('上传','','lg primary'),w=520)
nf=flow('新建文件夹',GRP(ROWF('名称',FI('','输入文件夹名称',220))),B('取消','','lg')+B('创建','','lg primary'),w=460)
rn=flow('重命名',GRP(ROWF('新名称',FI('fixture.txt','',220))),B('取消','','lg')+B('确认','','lg primary'),w=460)
dl=flow('删除 fixture.txt？','<div class="t-body" style="color:var(--ink-2)">此操作无法撤销。</div>',B('取消','','lg')+B('删除','','lg primary','style="background:var(--bad);border-color:var(--bad)"'),w=420)
gk=flow('生成新密钥',GRP(ROWF('密钥 ID',FI('','如: default, mykey',200,'mono')),ROWF('类型',SELECT('RSA 2048-bit (推荐)',200)),ROWF('备注',FI('','如: TianshanOS@device',200)),ROWF('别名',FI('','用于替代密钥 ID 显示',200)),ROWF('可导出',SW(True,'可导出')),ROWF('隐藏',SW(False,'隐藏'))),B('取消','','lg')+B('生成','','lg primary'),w=560)
dp=flow('部署公钥',GRP(ROWF('主机',FI('','192.168.55.100 或 hostname',200,'mono')),ROWF('用户名',FI('root','',200)),ROWF('端口',FI('22','',80)),ROWF('SSH 登录密码',FI('','输入 SSH 登录密码',200))),B('取消','','lg')+B('开始部署','','lg primary'),w=560)
board('Sheets2','文件与密钥弹窗 目标稿',two(upl+nf+rn,dl+gk+dp))
# ---------- SheetsAuto
crow=lambda v,op,val:f'<div class="row" style="gap:8px">{SELECT(v,190)}{SELECT(op,90)}{FI(val,"比较值",120)}{IB("x","移除","sm quiet")}</div>'
rule=flow('编辑规则',GRP(ROWF('规则 ID',FI('fixture-rule','',220,'mono'),'唯一标识符'),ROWF('规则名称',FI('Fixture rule','',220),'规则显示名称'),ROWF('图标',SEGX(['图标','图片'])+'<span class="btn icon" style="width:34px">⚡</span>'))+GRP(ROWF('条件逻辑',SELECT('AND',110)),ROWF('冷却时间',NUM('1000','ms',100)),ROWF('立即启用',SW(True,'立即启用')),ROWF('显示在面板',SW(True,'显示在面板')),ROWF('允许手动触发',SW(True,'允许手动触发')),title='规则选项')+f'<div class="sec-h" style="margin:18px 4px 6px"><span class="t-label">触发条件</span><span style="display:flex;align-items:center;gap:12px"><span style="display:flex;gap:8px;align-items:center">{SW(False,"仅手动触发")}<span class="t-body">仅手动触发</span></span>{B("添加","plus","sm")}</span></div><div class="grp">{crow("选择变量","==","")}{crow("cpu.avg_usage",">","80")}</div><div class="t-note" style="margin:6px 4px 0">点击「添加」创建触发条件，或打开「仅手动触发」作为快捷动作。运算符：== · != · > · >= · < · <= · 变化 · 包含</div>'+f'<div class="sec-h" style="margin:18px 4px 6px"><span class="t-label">执行动作</span>{B("添加","plus","sm")}</div><div class="grp"><div class="row" style="gap:8px">{SELECT("Fixture action（日志）",300)}{IB("x","移除","sm quiet")}</div></div><div class="t-note" style="margin:6px 4px 0">从已创建的动作模板中选择；没有可选项时提示「请先在动作模板区域创建动作」。</div>',B('取消','','lg')+B('保存修改','','lg primary'),w=760)
imp=flow('导入数据源配置',f'<div class="t-label" style="margin-bottom:10px">选择 .tscfg 配置包文件以导入数据源</div><div style="display:flex;align-items:center;gap:12px">{B("选择文件","ul")}<span class="t-note">未选择任何文件</span></div>'+GRP(ROWF('配置 ID','<span class="mono">agx_temp</span>'),ROWF('类型','数据源'),ROWF('签名者',ST('官方','ok')),ROWF('备注','重启后自动加载'),ROWF('覆盖已存在的配置',SW(False,'覆盖'),'该配置已存在，导入将覆盖现有文件'),title='配置包内容')+f'<div style="margin-top:10px">{ST("签名验证通过","ok")}</div>',B('取消','','lg')+B('确认导入','','lg primary'),w=560)
board('SheetsAuto','自动化弹窗 目标稿',wide(rule,760)+two(varsel+vw,exp('数据源')+imp))
# ---------- SheetsSec
csr=flow('生成证书签名请求',GRP(ROWF('设备 ID (CN)',FI('TIANSHAN-RM01-0001','',210,'mono')),ROWF('组织',FI('HiddenPeak Labs','',210)),ROWF('部门',FI('Device','',210)))+'<div class="gt">CSR (PEM)</div>'+TA('生成后显示在此',90),B('复制到剪贴板','cpy','lg')+B('关闭','','lg')+B('生成证书签名请求','','lg primary'),w=660,close=False)
rev=flow('撤销公钥',GRP(ROWF('主机',FI('','192.168.55.100 或 hostname',210,'mono')),ROWF('用户名',FI('root','',210)),ROWF('端口',FI('22','',80)),ROWF('SSH 登录密码',FI('','输入 SSH 登录密码',210))),B('取消','','lg')+B('撤销公钥','','lg primary','style="background:var(--bad);border-color:var(--bad)"'),w=560)
pexp=flow('导出配置包',GRP(ROWF('目录',FI('/sdcard','',200,'mono')+B('上级目录','','sm')+IB('refresh','刷新','sm')),title='选择要导出的文件')+'<div class="grp" style="margin-top:8px">'+ROWF('<span style="display:flex;gap:10px;align-items:center"><span class="chk on"></span>led/effects.json</span>','')+ROWF('<span style="display:flex;gap:10px;align-items:center"><span class="chk"></span>led/text.json</span>','')+'</div>'+f'<div style="display:flex;gap:8px;margin-top:8px">{B("全选","","sm")}{B("取消全选","","sm")}{B("选择整个目录","","sm")}</div>'+GRP(ROWF('名称',FI('','自动从文件名获取',200)),ROWF('描述',FI('','LED 特效配置',200)),title='配置包信息')+'<div class="gt">接收方设备证书</div>'+TA('-----BEGIN CERTIFICATE-----',64)+'<div class="gt">配置包 (.tscfg)</div>'+TA('配置包将在此显示...',64),B('复制到剪贴板','cpy','lg')+B('下载到本地','dl','lg')+B('取消','','lg')+B('生成配置包','','lg primary'),w=760,close=False)
board('SheetsSec','安全页弹窗 目标稿',two(csr+ic_('安装证书','-----BEGIN CERTIFICATE-----')+ic_('安装 CA','-----BEGIN CERTIFICATE-----')+certv+ckp2,pexp+pimp+plist+rev+mis2+imph2))
# ---------- SheetsLed 局部
ap=flow('WiFi 热点配置',GRP(ROWF('SSID',FI('TianshanOS','',200)),ROWF('密码',FI('','至少 8 位',200)),ROWF('信道',SELECT('1',90)),ROWF('隐藏 SSID',SW(False,'隐藏 SSID'))),B('取消','','lg')+B('应用','','lg primary'),w=560)
login=flow('登录 TianshanOS',GRP(ROWF('用户名',FI('admin','',200)),ROWF('密码',FI('','请输入密码',200)))+f'<div style="margin:10px 4px 0">{ST("用户名或密码错误","bad")}</div>',B('取消','','lg')+B('登录','','lg primary'),w=460)
board('SheetsLed','LED · 网络 · 日志 · 登录 弹窗 目标稿',two(ledA+ledT+ledC+login+logs,ledI+ledQ+ledF+scan+wpw+ap+aps+dhcp+pick))
# ---------- SheetsTypes 增补 REST + CLI
rest=flow('添加外部数据源 · REST API',srcseg(0)+'<div style="height:14px"></div>'+head()+'<div class="gt" style="margin-top:16px">REST API 配置</div>'+fld('请求地址',f'<div style="display:flex;gap:8px"><input class="field mono" style="flex:1" placeholder="http://…" aria-label="请求地址">{B("测试")}</div>')+f'<div style="margin:12px 0 4px">{ST("连接成功","ok")} <span class="t-note">原始数据 ▸</span></div><div style="display:flex;gap:6px;flex-wrap:wrap;margin:8px 0 0">'+''.join(B(x,'','sm') for x in ['data.temperature','data.fan','data.status'])+'</div>'+GRP(ROWF('方法',SELECT('GET',100)),ROWF('轮询间隔 (ms)',FI('1000','',100)),ROWF('Authorization 头',FI('','Bearer token',200),'可选'),ROWF('JSON 数据路径',FI('','data.temperature',200,'mono'),'留空取整个响应；点击上方字段自动填入'),title='参数'),ft())
board('SheetsTypes','数据源与动作模板：各类型参数区 目标稿',two(rest+ws+sio+varsrc,a_cli+a_ssh+a_led+a_log+a_var+a_web))
# ---------- 页脚：只有 “TianshanOS · RMinte® AI” 用 Quantify
import glob,re
for f in glob.glob(P+'*.dc.html')+glob.glob('dc/t-*.html'):
    s=open(f).read()
    n=s.replace('© 2026 TianshanOS · RMinte® AI Technology Co., Ltd. · www.RMinte.com','© 2026 <span class="q">TianshanOS · RMinte® AI</span> Technology Co., Ltd. · www.RMinte.com')
    if n!=s: open(f,'w').write(n)
css=open(P+'spec.css').read()
css=css.replace('.foot{font-family:var(--display);font-weight:400;font-size:13px;line-height:18px;color:var(--ink-2);text-align:center}','.foot{font-size:13px;line-height:18px;color:var(--ink-2);text-align:center}.foot .q{font-family:var(--display);font-weight:400}')
open(P+'spec.css','w').write(css)
# ---------- 画布自动排版
c=json.load(open(P+'canvas.json'))
rows=[['Main','Modal'],['Net','Files','Term'],['Auto','Cmds','Ota'],['Sec','Spec'],['Sheets1','Sheets2','SheetsAuto'],['SheetsLed','SheetsSec','SheetsTypes'],['SheetsMore','SheetsStates']]
Hs={k[:-8]:v['h'] for k,v in c['boards'].items()}
Hs.update(BOARDS)
y=0
for r in rows:
    for i,k in enumerate(r):
        b=c['boards'][k+'.dc.html'];b['x']=i*1520;b['y']=y;b['h']=Hs[k]
    y+=max(Hs[k] for k in r)+140
c['order']=[k+'.dc.html' for r in rows for k in r]
json.dump(c,open(P+'canvas.json','w'),ensure_ascii=False,indent=1)
