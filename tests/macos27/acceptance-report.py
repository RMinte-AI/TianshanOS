"""Explicit, conservative disposition of every retained legacy requirement."""
import json
from pathlib import Path
root=Path('docs/macos27');base='output/macos27-20260928/'
rows=json.loads((root/'acceptance-migration.json').read_text())
groups={
'B':('基线身份、完整模板/字段/事件/条件/存储逐项映射；运行证据仅限规定替身。',['baseline/identity.json','protected-final.json','../..//docs/macos27/field-mapping.json','after/contract.json']),
'F':('原入口、所属区域、顺序和业务AST保持；实际生产UI分支与合成后端回归。真实设备执行结果未验收。',['runtime-entry-summary.json','after/contract.json','after/responsive-replay.json']),
'V':('按批准第三版实际浏览器样板和逐页对照；非全平台视觉认证。',['stage1/material-audit.json','screenshot-catalog.json','control-contrast.json']),
'T':('系统字体、实际文字节点字号/长文本/技术值与逐块diff检查。',['after/responsive-replay.json','after/nested-surfaces-replay.json','after/error-long-replay.json']),
'L':('冻结参考哈希、入口精确SVG字符串及实际截图一致。',['reference/hash-verification.json','after/contract.json','stage1/system-1440.png']),
'G':('普通Chromium交付真实backdrop玻璃；媒体回退和原入口/布局保持。',['final-browser.json','stage1/material-audit.json','after/contract.json']),
'I':('原条件、确认/事件/API/校验保持；代表性实际UI回归与状态样本。',['after/contract.json','keyboard-auth.json','after/imports-replay.json']),
'N':('运行的是入口内联i18n，语言包及key/参数/存储/业务均保持。两种实际语言回归。',['after/contract.json','after/responsive-replay.json','language-failure.json','lifecycle-input.json']),
'R':('实际记录CSS视口；设备模拟与真实手机明确区分。',['after/responsive-replay.json','after/zoom-profile.json','keyboard-auth.json']),
'S':('完整AST/保护文件哈希及测试工具审查；只连接本地显式替身。',['after/contract.json','protected-final.json']),
'P':('同配置本地A/B实际压缩资源；原始样本与限制独立记录。',['performance/final-v3/raw.json','build-evidence.json']),
'D':('真实构建、最终身份、全量映射、完整diff与精确逆补丁。',['final-configuration.json','final-integrity.log','rollback-proof.json'])}
# Broader requirements stay unpassed when the available evidence is narrower.
exceptions={
'F-11':('INCONCLUSIVE','列表/多选/上传表单/重命名/路径/配置包分支通过；真实设备上传下载/取消结果未执行。'),
'F-12':('INCONCLUSIVE','原xterm实际输入、合成WebSocket输出、ANSI和API选择通过；系统剪贴板/IME/真实SSH会话未验证。'),
'F-14':('INCONCLUSIVE','全部指令表单/服务/孤儿/变量/日志入口及契约保持；实际SSH执行取消和远端成功判定未验证。'),
'F-15':('INCONCLUSIVE','账户/密钥/证书/配置包全部UI入口和合成角色分支检查；真实认证、签名、mTLS及设备持久化BLOCKED。'),
'V-08':('N/A','当前生产入口、事件与存储无浅深色主题切换；未新增此功能。终端原深色绘图区保留。'),
'V-09':('N/A','基线无承载主题选择的data-theme；没有新增或覆盖主题属性。'),
'V-11':('INCONCLUSIVE','实际合成9个玻璃文字样本及12个按钮正常/悬停/焦点样本通过；非全部任意数据/背景/禁用组合认证，保护的数据颜色未统一改色。'),
'G-05':('INCONCLUSIVE','Chromium减少透明度/高对比/强制色实际得到实体回退；无backdrop-filter的原生引擎与Safari未运行。手机默认关闭已被GOAL替换。'),
'G-06':('N/A','没有玻璃开关或产品偏好。诊断实体注入仅测试浏览器材质，不进入生产。'),
'I-01':('INCONCLUSIVE','代表正常/悬停/焦点已采样；原禁用/加载条件保留、导入禁用实际检查，但没有穷尽所有控件与状态组合。'),
'N-05':('INCONCLUSIVE','system.language的原调用、参数和触发条件逐字保持；实际设备语言写入与读回未授权执行。'),
'N-08':('INCONCLUSIVE','全部可达能力入口已运行，两语言主路由/角色/主要表单覆盖；没有穷尽全部错误文案、Toast、辅助技术播报组合。'),
'N-11':('INCONCLUSIVE','语言切换/持久化/失败重试实际通过；全部未提交表单/子标签恢复组合未穷尽，业务代码保持。'),
'R-04':('BLOCKED','无真实手机/软键盘会话，桌面窄视口不能证明软键盘可达性。'),
'R-05':('INCONCLUSIVE','Tab/focus及点击关闭实际对照；两端内存弹窗都没有Esc关闭、焦点陷阱、焦点返回，属于未越界修复的基线缺陷。'),
'R-06':('INCONCLUSIVE','32个桌面页面/宽度/版本组合已测命中几何：A有38个、B有32个小于24px候选（含原生range/checkbox和既有紧凑控件）；不能由包围盒推导所有命中区域/间距例外或真实触控通过。'),
'R-07':('INCONCLUSIVE','Chromium高对比/forced-colors通过；标签/ARIA无业务变更，原生读屏未运行。'),
'R-08':('BLOCKED','键盘输入已运行；Mac锁屏且无真实IME/自动填充/系统剪贴板会话。'),
'P-01':('PASS','本地A/B使用完全相同严格接口替身/数据/配置。无可访问设备固件A/B，因此结论范围仅本地浏览器。'),
'P-07':('FAIL','原零增长要求尚未获得豁免：生成器数据/索引页占用2592000→2603264字节，增加11264字节；分区未改。剩余345856字节是离线页账，不是设备运行可写容量。'),
'P-08':('NOT_RUN','资源版本机制和HTTP服务代码保持；fixture未复制真实设备缓存头，未冒充真实热缓存验证。'),
'P-10':('INCONCLUSIVE','各端10次冷加载与每类50次交互原始数据齐备；浏览器自动化/帧量化/共享主机噪声存在，不用任意百分比预算判零回退。'),
'P-11':('INCONCLUSIVE','滚动rAF及Paint/PrePaint/Layerize/RasterTask实际采集；合成成本样本有增量，实体诊断与平台限制见性能报告，未宣称零成本。'),
'P-12':('PASS','最终配置哈希不变：A驻留1827506ms、B驻留1827313ms，各50循环/250交互；稳定期DOM A839/B841、监听均169、活动socket均1，堆在GC范围内波动。此PASS仅限该本地观察窗口，观察器自身开销和设备缺口单列。'),
'P-13':('BLOCKED','无设备堆/PSRAM/CPU/网络和控制响应会话；未刷机或执行真实控制。'),
'P-14':('BLOCKED','无真实手机浏览器，桌面390/320和横屏模拟不代替。'),
'P-15':('INCONCLUSIVE','GOAL取代旧默认关玻璃决定；普通配置保持批准玻璃，回退仅诊断。性能尚不能全项通过，未自设5%/10%额度。'),
'D-04':('INCONCLUSIVE','本地证据完整登记，关键设备/平台/输入和性能缺口明确保留；发布验收未完成。'),
'D-08':('PASS','本地玻璃默认开启；无提交/推送/部署/刷机授权。总体仅报告本地实施状态与验收缺口。')}
for x in rows:
 note,paths=groups[x['id'][0]];x['status']='PASS';x['result']=note;x['evidence']=[base+p if not p.startswith('../') else 'docs/macos27/field-mapping.json' for p in paths]
 if x['id'] in exceptions:x['status'],x['result']=exceptions[x['id']]
 if x['id']=='R-06':x['evidence'].append(base+'target-size.json')
 if x['id']=='F-17':x['evidence'].append(base+'lifecycle-input.json')
 if x['id']=='F-12':x['evidence'] += [base+'after/terminal-replay.json',base+'final-browser.json']
 if x['id']=='D-06':x['result']='当前代理在作者阶段后完整逐块复查生产diff、全部新增工具和文档；不声称独立人工/第二代理审查。';x['evidence'].append('docs/macos27/REVIEW.md')
 if x['id']=='P-16':x['result']='缺设备和不确定项目未算PASS；以逐项实际边界报告。'
 if x['id']=='S-02':x['status']='PASS';x['result']='清理与监听源码保持；最终本地50循环/30分钟窗口内稳定期DOM/监听/活动socket计数稳定。不是任意时长或所有路径无泄漏证明。';x['evidence'].append(base+'performance/summary.json')
 for p in x['evidence']:assert Path(p).exists(),p
(root/'acceptance-migration.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2))
md=['# 旧验收清单迁移及本轮结果','', '全部133项保留；REPLACE_VISUAL项按GOAL替换旧视觉结论。PASS只对应result明示范围，不把合成接口当设备验收。最新阶段总结见ACCEPTANCE.md。','', '| ID | 迁移 | 状态 | 本轮结果与边界 | 证据 |','|---|---|---|---|---|']
for x in rows:md.append('| '+x['id']+' | '+x['disposition']+' | '+x['status']+' | '+x['result'].replace('|',' / ')+' | '+', '.join('`'+p+'`' for p in x['evidence'])+' |')
(root/'acceptance-migration.md').write_text('\n'.join(md)+'\n')
print({s:sum(x['status']==s for x in rows)for s in ['PASS','FAIL','NOT_RUN','BLOCKED','INCONCLUSIVE','N/A']})
