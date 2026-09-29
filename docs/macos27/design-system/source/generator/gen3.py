exec(open('gen2.py').read())
# ---------- CSS: 分组表单
css=open(P+'spec.css').read()
if '/*FORM*/' not in css:
    css+='''
/*FORM*/
.gt{font-size:13px;line-height:18px;color:var(--ink-2);margin:18px 4px 6px}.gt:first-child{margin-top:0}
.grp{background:rgba(255,255,255,.62);border-radius:var(--r-inner);padding:0 16px}
.grp>.row{display:flex;align-items:center;justify-content:space-between;gap:16px;min-height:46px;border-top:1px solid var(--hair)}
.grp>.row:first-child{border-top:0}
.row .rl{font-size:14px;line-height:20px;flex:none}.row .rl small{display:block;font-size:12px;line-height:16px;color:var(--ink-3)}
.row .rc{display:flex;align-items:center;justify-content:flex-end;gap:8px;min-width:0}
.seg.full{display:flex;width:100%}.seg.full button{flex:1;padding:0 6px}
.radio{width:16px;height:16px;border-radius:50%;border:1px solid var(--hair-strong);background:#fff;display:inline-block;flex:none;position:relative}.radio.on{border:5px solid var(--accent-text)}
.sw{width:30px;height:30px;border-radius:50%;box-shadow:inset 0 0 0 .5px rgba(0,0,0,.2);flex:none}
'''
    open(P+'spec.css','w').write(css)
def ROWF(l,c,sub=''): return f'<div class="row"><div class="rl">{l}'+(f'<small>{sub}</small>' if sub else '')+f'</div><div class="rc">{c}</div></div>'
def GRP(*rows,title=''): return (f'<div class="gt">{title}</div>' if title else '')+'<div class="grp">'+''.join(rows)+'</div>'
def TABS(on): return SEGX(['程序动画','图像/QR码','文本显示','后处理滤镜','色彩校正'],on,'').replace('class="seg"','class="seg full" style="margin-bottom:6px"')
FI=lambda v='',ph='',w=160,cls='':f'<input class="field {cls}" value="{v}" placeholder="{ph}" aria-label="{ph or v}" style="width:{w}px">'
SWATCH=lambda c:f'<span class="sw" style="background:{c}"></span>'
def SLR(l,pct,val,sub=''): return ROWF(l,f'<div class="sl" style="width:200px;flex:none"><i style="width:{pct}%"></i><b style="left:calc({pct}% - 10px)"></b></div><span class="t-value num" style="width:40px;text-align:right">{val}</span>',sub)
FT=lambda *a:''.join(a)
# 程序动画
ledA=flow('LED 设置 · Board',TABS(0)+'<div class="gt">动画</div><div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">'+''.join(eff(x,i==1) for i,x in enumerate(['彩虹','火焰','呼吸','流星','波浪','闪烁','渐变','雨滴']))+'</div>'+GRP(ROWF('当前','<span class="t-value">火焰</span>','未选择时提示「请先选择一个动画」'),SLR('速度',50,'50'),ROWF('颜色',SWATCH('#ff6600')),title='设置'),B('停止','stop','lg')+B('重置','','lg')+B('保存','','lg primary'),close=True)
# 图像
ledI=flow('LED 设置 · Matrix',TABS(1)+'<div style="display:flex;justify-content:center;margin:12px 0 4px">'+SEGX(['图像','QR 码'],0)+'</div>'+GRP(ROWF('图像路径',FI('','/sdcard/images/...',200,'mono')+B('浏览','','sm')),ROWF('居中显示',SW(True,'居中显示'))),B('显示图像','','lg primary'),close=True)
ledQ=flow('LED 设置 · Matrix',TABS(1)+'<div style="display:flex;justify-content:center;margin:12px 0 4px">'+SEGX(['图像','QR 码'],1)+'</div>'+f'<div class="gt">内容</div>{IN("","输入文本或URL","","width:100%")}'+GRP(ROWF('纠错',SELECT('M · 15%',130)),ROWF('前景色',SWATCH('#000000')),ROWF('背景图',FI('','无',150)+B('浏览','','sm')+B('清除','','sm')),title='样式'),B('生成 QR 码','','lg primary'),close=True)
# 文本
ledT=flow('LED 设置 · Matrix',TABS(2)+f'<div class="gt">文本</div>{IN("","输入要显示的文本","","width:100%")}'+GRP(ROWF('字体',SELECT('默认',150)+IB('refresh','刷新字体','sm')),ROWF('对齐',SEGX(['左','中','右'],1)),ROWF('颜色',SWATCH('#ffffff')),title='字体与样式')+GRP(ROWF('自动位置',SW(True,'自动位置')),ROWF('X',FI('0','X',80,'num')+'<span class="t-note">打开自动位置时不可用</span>').replace('class="row"','class="row" style="opacity:.4"'),ROWF('Y',FI('0','Y',80)).replace('class="row"','class="row" style="opacity:.4"'),title='位置')+GRP(ROWF('方向',SELECT('无',110)),ROWF('速度',FI('50','速度',80)),ROWF('循环滚动',SW(False,'循环滚动')),title='滚动'),B('停止','stop','lg')+B('显示','','lg primary'),close=True)
# 滤镜
ledF=flow('LED 设置 · Matrix',TABS(3)+'<div class="gt">滤镜</div><div style="display:grid;grid-template-columns:repeat(5,1fr);gap:8px">'+''.join(eff(x,i==2) for i,x in enumerate(['脉冲','呼吸','闪烁','波浪','扫描线','故障艺术','彩虹','闪耀','等离子体','怀旧','色阶分离','对比度','反色','灰度']))+'</div>'+GRP(ROWF('当前','<span class="t-value">闪烁</span>','未选择时提示「请先选择一个滤镜」'),ROWF('参数','<span class="t-note">随滤镜变化</span>'),title='设置'),B('停止','stop','lg')+B('应用','','lg primary'),close=True)
# 色彩校正
ledC=flow('LED 设置 · 色彩校正',TABS(4)+GRP(ROWF('启用全局色彩校正',SW(True,'启用全局色彩校正')),title='')+GRP(SLR('红 R',50,'1.00'),SLR('绿 G',50,'1.00'),SLR('蓝 B',50,'1.00'),title='白平衡（RGB 缩放）· 默认 1.0，小于 1.0 减弱该通道')+GRP(SLR('Gamma',40,'1.00','1.0 线性；大于 1.0 中间调变暗'),SLR('亮度',50,'1.00','整体亮度缩放'),SLR('饱和度',50,'1.00','0 为灰度，1.0 不变'),title='调整')+GRP(ROWF('配置文件',B('导出到 SD 卡','','sm')+B('从 SD 卡导入','','sm')),title='备份'),B('重置','','lg')+B('保存','','lg primary'),close=True)
board('SheetsLed','LED · 网络 · 日志 · 登录 弹窗 目标稿',two(ledA+ledT+ledC+login+logs,ledI+ledQ+ledF+scan+wpw+ap+aps+dhcp+pick))

# ---------- 安全弹窗（换成真实内容）
ckp2=flow('生成 HTTPS 密钥对',f'<div class="t-body" style="color:var(--ink-2)">为设备生成 ECDSA P-256 密钥对，用于 mTLS 身份验证。</div><div style="margin-top:12px;display:flex;gap:8px;align-items:center;color:var(--warn)">{IC["warn"]}<span class="t-body" style="color:var(--ink)">已存在密钥对，继续将覆盖现有密钥！<span class="t-note">（仅在已有密钥对时显示）</span></span></div>',B('取消','','lg')+B('生成','','lg primary','style="background:var(--bad);border-color:var(--bad)"'),w=520)
mis2=flow('安全警告：主机指纹不匹配！',f'<div class="t-body" style="margin-bottom:8px">主机密钥已更改！这可能表明：</div><div class="t-body" style="color:var(--ink-2);line-height:22px;margin-bottom:14px">• 中间人攻击（Man-in-the-Middle Attack）<br>• 服务器重新安装或密钥重新生成<br>• IP 地址被分配给了不同的服务器</div>'+GRP(ROWF('主机','<span class="mono">192.0.2.30</span>'),ROWF('存储的指纹','<span class="mono t-label">SHA256:aB…</span>'),ROWF('当前指纹','<span class="mono t-label">SHA256:xY…</span>'))+f'<div class="t-note" style="margin-top:12px;line-height:18px">建议：如果您确认服务器已重装或密钥已更新，可以点击「更新主机密钥」移除旧记录，然后重新连接以信任新密钥。</div>',B('取消','','lg')+B('更新主机密钥','','lg primary','style="background:var(--bad);border-color:var(--bad)"'),w=560)
imph2=flow('导入 SSH 主机配置',f'<div class="t-label" style="margin-bottom:10px">选择 .tscfg 配置包文件以导入 SSH 主机配置</div><div style="display:flex;align-items:center;gap:12px">{B("选择文件","ul")}<span class="t-note">未选择任何文件</span></div>'+GRP(ROWF('配置内容','<span class="t-note">选择文件后显示预览</span>'),ROWF('覆盖已存在的配置',SW(False,'覆盖')),title='配置包内容'),B('取消','','lg')+B('确定要导入','','lg primary'),w=520)
board('SheetsSec','安全页弹窗 目标稿',two(csr+ic_('安装证书','-----BEGIN CERTIFICATE-----')+ic_('安装 CA','-----BEGIN CERTIFICATE-----')+certv+ckp2,pexp+pimp+plist+rev+mis2+imph2))

# ---------- 数据源 / 动作模板 各类型
srcseg=lambda i:SEGX(['REST API','WebSocket','Socket.IO','指令变量'],i,'display:flex').replace('class="seg"','class="seg full"')
head=lambda:ROW(fld('数据源 ID',IN('','如: agx_temp')),fld('显示名称',IN('','如: AGX 温度')))
enab=f'<span style="margin-right:auto;display:flex;align-items:center;gap:8px">{SW(True,"创建后立即启用")}<span class="t-body">创建后立即启用</span></span>'
ft=lambda: enab+B('取消','','lg')+B('添加数据源','','lg primary')
extract=lambda ph,note:GRP(ROWF('JSON 数据路径',FI('',ph,220,'mono'),note),title='字段')
ws=flow('添加外部数据源 · WebSocket',srcseg(1)+'<div style="height:14px"></div>'+head()+f'<div class="gt" style="margin-top:16px">WebSocket 配置</div>'+fld('WebSocket 地址',f'<div style="display:flex;gap:8px"><input class="field mono" style="flex:1" placeholder="ws://192.168.1.100:8080/ws" aria-label="地址">{B("测试")}</div>')+f'<div style="margin:12px 0 4px">{ST("连接成功，已收到数据","ok")} <span class="t-note">原始数据 ▸</span></div>'+GRP(ROWF('JSON 数据路径',FI('','data.temperature',200,'mono'),'留空取整个消息'),ROWF('断线重连间隔 (ms)',FI('3000','',100)),title='参数'),ft())
sio=flow('添加外部数据源 · Socket.IO',srcseg(2)+'<div style="height:14px"></div>'+head()+f'<div class="gt" style="margin-top:16px">Socket.IO 配置（v4，使用 HTTP/HTTPS 地址）</div>'+fld('服务器地址',f'<div style="display:flex;gap:8px"><input class="field mono" style="flex:1" placeholder="http://10.10.99.99:59090" aria-label="服务器地址">{B("测试")}</div>')+GRP(ROWF('事件名称',FI('','留空自动发现',200),'测试时留空可自动发现事件'),ROWF('超时时间 (ms)',FI('5000','',100)),ROWF('JSON 数据路径',FI('','cpu.avg_usage',200,'mono'),'留空取整个事件数据'),ROWF('自动发现所有 JSON 字段',SW(True,'自动发现'),'关闭后仅使用选中的字段作为变量'),title='参数'),ft())
varsrc=flow('添加外部数据源 · 指令变量',srcseg(3)+'<div style="height:14px"></div>'+head()+GRP(ROWF('SSH 主机',SELECT('选择主机',190),'在安全页添加'),ROWF('指令',SELECT('选择指令',190),'在指令页创建'),ROWF('检测间隔 (秒)',FI('10','',80),'定期读取变量值的间隔'),title='SSH 指令变量')+GRP(ROWF('命令','<span class="mono t-label">systemctl is-active nginx</span>'),ROWF('描述','<span class="t-label">检查服务状态</span>'),ROWF('超时','<span class="t-label">10 秒</span>'),title='指令详情（选择指令后显示）')+f'<div class="t-note" style="margin-top:10px">将监视以下变量（需先执行指令）：<span class="mono">nginx_state</span> · 未选择时提示「请先选择 SSH 主机和指令」</div>',ft())
atile=lambda t,d,on=False:tile(t,d,on)
atypes=lambda i:'<div class="gt">动作类型</div><div style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px">'+''.join(f'<div class="tile{" on" if k==i else ""}" style="padding:10px 12px"><span class="t-body" style="font-weight:600">{t}</span></div>' for k,t in enumerate(['CLI 命令','SSH 命令','LED 控制','日志记录','设置变量','Webhook']))+'</div>'
basic=lambda:GRP(ROWF('动作 ID',FI('','如: restart_agx',180,'mono'),'字母、数字和下划线'),ROWF('显示名称',FI('','如: 重启 AGX',180),'留空则使用 ID'),ROWF('描述',FI('','可选',180)),ROWF('执行延迟',FI('0','',80)),ROWF('异步执行',SW(False,'异步执行'),'API 立即返回，动作在后台队列执行'),title='基本信息')
aft=lambda:B('取消','','lg')+B('保存动作','','lg primary')
a_cli=flow('新建动作模板 · CLI 命令',atypes(0)+GRP(ROWF('命令行',FI('','如: gpio --set 48 1',230,'mono'),'支持 gpio、device、fan、led、net 等'),title='CLI 命令配置')+'<div style="display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 0">'+''.join(B(x,'','sm') for x in ['GPIO','AGX 开机','AGX 重启','风扇','LED 动画'])+'</div>'+GRP(ROWF('结果变量',FI('','如: cli.result',180,'mono'),'存储命令输出到变量'),ROWF('超时时间',FI('','',80)),title='高级选项')+basic(),aft())
a_ssh=flow('新建动作模板 · SSH 命令',atypes(1)+GRP(ROWF('命令',SELECT('选择命令',200),'选择已在 SSH 管理页面配置的命令'),title='SSH 命令配置')+GRP(ROWF('主机','<span class="t-label">fixture-host</span>'),ROWF('命令','<span class="mono t-label">sudo systemctl restart nginx</span>'),ROWF('变量','<span class="mono t-label">-</span>'),title='命令详情（选择后显示）')+basic(),aft())
a_led=flow('新建动作模板 · LED 控制',atypes(2)+GRP(ROWF('设备',SELECT('-- 选择设备 --',190),'选择要控制的 LED 设备'),ROWF('控制类型',SELECT('填充',190),'灯带：填充 · 动画 · 亮度 · 关闭；矩阵屏另有文本 · 图像 · QR 码 · 滤镜 · 停止滤镜 · 停止文本'),ROWF('参数','<span class="t-note">随控制类型变化</span>'),title='LED 控制配置')+basic(),aft())
a_log=flow('新建动作模板 · 日志记录',atypes(3)+GRP(ROWF('级别',SELECT('INFO',110)),ROWF('消息',FI('','如: 设备状态变更: ${device.status}',230),'支持变量：${变量名}'),title='日志配置')+basic(),aft())
a_var=flow('新建动作模板 · 设置变量',atypes(4)+GRP(ROWF('变量名',FI('','例如：system.flag',200,'mono')),ROWF('值',FI('','例如：true、123、${other_var}',200,'mono'),'支持表达式和变量引用'),title='变量配置')+basic(),aft())
a_web=flow('新建动作模板 · Webhook',atypes(5)+GRP(ROWF('URL',FI('','https://…',230,'mono')),ROWF('方法',SELECT('POST',110)),title='Webhook 配置')+f'<div class="gt">请求体</div>{TA("JSON 格式，支持变量",72)}'+basic(),aft())
board('SheetsTypes','数据源与动作模板：各类型参数区 目标稿',two(ws+sio+varsrc,a_ssh+a_led+a_log+a_var+a_web))

# ---------- 指令编辑 + 单选框
presets=[IC[k] for k in ['play','refresh','stop','trash','box','grid','key','doc','term','eye','shield','lock']]
icogrid='<div style="display:grid;grid-template-columns:repeat(6,1fr);gap:8px">'+''.join(f'<div class="btn icon" style="width:100%;height:40px{";box-shadow:inset 0 0 0 2px var(--accent)" if i==0 else ""}">{p}</div>' for i,p in enumerate(presets))+'</div>'
cmdfull=flow('新建指令',ROW(fld('指令 ID',IN('','例如：restart_nginx')),fld('名称',IN('','例如：重启服务')))+'<div style="height:12px"></div>'+fld('命令',TA('例如：sudo systemctl restart nginx',64))+'<div style="height:12px"></div>'+fld('描述',IN('','简要说明这个指令的作用','','width:100%'))
 +'<div class="gt" style="margin-top:16px">图标</div><div style="display:flex;justify-content:center;margin-bottom:10px">'+SEGX(['图标','图片'],0)+'</div>'+icogrid+f'<div class="t-note" style="margin-top:8px">选「图片」时：路径 <span class="mono">/sdcard/images/…</span> + 浏览 / 清除</div>'
 +GRP(ROWF('后台运行 (nohup)',SW(False,'nohup')),ROWF('服务模式',SW(False,'服务模式'),'开启后可设置下方就绪判定'),title='运行方式')
 +GRP(ROWF('就绪匹配',FI('','例如：Running on',200,'mono')),ROWF('失败匹配',FI('','例如：error|failed',200,'mono')),ROWF('就绪超时 (秒)',FI('30','',80)),ROWF('检测间隔 (秒)',FI('1','',80)),title='服务模式（仅开启服务模式时显示）')
 +GRP(ROWF('变量名',FI('','例如：ping_test',200,'mono')),ROWF('期望匹配',FI('','例如：active (running)',200,'mono')),ROWF('失败匹配',FI('','例如：error|failed',200,'mono')),ROWF('提取正则',FI('','例如：version: (.*)',200,'mono')),ROWF('命中即停止',SW(False,'命中即停止')),ROWF('超时 (秒)',FI('30','',80)),title='输出匹配与变量'),B('取消','','lg')+B('保存','','lg primary'),w=660)
radios=f'<div class="sheet m-float" style="position:relative;width:660px;padding:24px"><div class="st" style="font-size:20px;margin-bottom:14px">单选框与分组表单</div>'+GRP(ROWF('低电压处理','<span style="display:flex;gap:16px"><span style="display:flex;gap:6px;align-items:center"><span class="radio on"></span>倒计时关机</span><span style="display:flex;gap:6px;align-items:center"><span class="radio"></span>仅告警</span></span>'),ROWF('禁用行',SW(False,'禁用')).replace('class="row"','class="row" style="opacity:.4"'),title='单选框（同一组只选一项）')+'<div class="t-note" style="margin-top:14px;line-height:18px">分组表单：一个圆角分组（12px）放若干行，每行 46px，左侧标签（可带 12px 说明），右侧控件；行之间用 1px 发丝线分隔。同一个弹窗里所有选项、开关、数值都用这种行，不再混用「上标签下输入框」和「行内散排」。文本类长输入（命令、PEM、URL）仍用上标签的整宽输入框。</div></div>'
board('SheetsMore','指令编辑与表单规范 目标稿',two(cmdfull,radios))

# ---------- canvas 追加
c=json.load(open(P+'canvas.json'))
for k,t,(x,y) in [('SheetsTypes','数据源与动作模板：各类型参数区 目标稿',(0,11000)),('SheetsMore','指令编辑与表单规范 目标稿',(1520,11000))]:
    c['boards'][k+'.dc.html']={'x':x,'y':y,'w':1440,'h':BOARDS[k],'title':t}
    if k+'.dc.html' not in c['order']: c['order'].append(k+'.dc.html')
for k in ['SheetsAuto','SheetsLed','SheetsSec','SheetsStates']:
    c['boards'][k+'.dc.html']['h']=BOARDS[k]
json.dump(c,open(P+'canvas.json','w'),ensure_ascii=False,indent=1)
