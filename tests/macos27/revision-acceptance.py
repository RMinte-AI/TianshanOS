"""Rebind every legacy criterion to current evidence; never reuse historical PASS."""
import json,collections
from pathlib import Path
root=Path('output/macos27-20260929-card-revision')
source=Path('docs/macos27/acceptance-migration.json')
history=root/'rejected/acceptance-migration.json'
if not history.exists():history.write_bytes(source.read_bytes())
rows=json.loads(history.read_text())
prefix=str(root)+'/'
proof={
 'B':['source-integrity.json','contract.json','regression-summary.json'],
 'F':['contract.json','built/regression-summary.json','lifecycle-input.json'],
 'V':['built/stage1.json','built/stage1/material-audit.json','control-contrast.json','built-form-parity.json'],
 'T':['built/regression-summary.json','built-form-parity.json','contract.json'],
 'L':['source-integrity.json','contract.json','built/stage1.json'],
 'G':['built/stage1.json','built/material-audit.json','final-browser.json'],
 'I':['contract.json','keyboard-auth.json','built/regression-summary.json'],
 'N':['contract.json','language-failure.json','lifecycle-input.json','built/regression-summary.json'],
 'R':['built/regression-summary.json','after/zoom-profile.json','target-size.json','keyboard-auth.json'],
 'S':['contract.json','source-integrity.json','lifecycle-input.json'],
 'P':['performance/summary.json','asset-transfer.json','build-evidence.json'],
 'D':['source-integrity.json','build-evidence.json','built-contract.json','built/regression-summary.json']}
# Criteria requiring environments or exhaustive behavior not established by local samples.
uncertain=set('F-11 F-12 F-14 F-15 V-11 G-05 I-01 N-05 N-08 N-11 R-05 R-06 R-07 P-10 P-11 P-12 P-13 P-15 D-04 D-05'.split())
notrun=set('R-04 R-08 P-08 P-14'.split())
na={'V-08':'基线无深色主题入口或主题存储，本次不新增主题。','V-09':'基线没有主题 data-theme 合同；没有引入 mist 属性。','G-06':'未新增玻璃开关，因此不存在产品开关布局比较。'}
for row in rows:
    ident=row['id'];group=ident.split('-')[0]
    row['status']='INCONCLUSIVE' if ident in uncertain else 'NOT_RUN' if ident in notrun else 'N/A' if ident in na else 'FAIL' if ident=='P-07' else 'PASS'
    row['evidence']=[prefix+p for p in proof[group]]
    assert all(Path(p).exists() for p in row['evidence'])
    row['result']='当前修订版源码契约、实际本地浏览器和构建检查范围内成立；不代表设备部署或未执行的业务路径。'
    if group in ['V','T','G']:row['result']='按已批准第三版替换旧视觉约束；当前实际生产截图及对应范围检查为依据，不沿用旧版本 PASS。'
    if ident in uncertain:row['result']='已有本地证据，但该条完整要求包含未覆盖状态、真实设备行为、噪声归因或长期内存判断，不能整体判 PASS。'
    if ident in notrun:row['result']='实际移动软键盘／IME／手机或设备热缓存环境未运行；桌面视口及本地 fixture 不代替。'
    if ident in na:row['result']=na[ident]
    if ident=='P-07':row['result']='实际 SPIFFS 数据与索引页占用比原版增加 14592 字节；分区未改，未获得放宽不增长要求的授权。'
    if ident=='P-13':row['evidence'].append(prefix+'device-readonly.json');row['result']='Safari 已只读观察实机 CPU、DRAM、PSRAM、任务数；新版未部署，固件身份不同，不能推导新版设备性能。'
    if ident=='D-01':row['result']='保留 minify/gzip/SPIFFS 构建链；用户 2026-09-29 明确授权修复 minify_web.py 两行 CSS 错误。'
    if ident=='D-06':row['result']='分阶段完整读取生产 diff 及后续差异，审查新增测试/报告工具和授权的两行构建修复；当前代理自查，不冒充独立人工审签。'
    if ident=='D-07':row['result']='视觉逆补丁与构建修复逆补丁分开；组合逆应用仅在临时副本执行并恢复全部四文件基线哈希。'
    if ident=='B-09':row['result']='原模态 Esc/焦点问题保留记录；构建选择器和 calc 错误已明确定位，并在用户授权后修复。'
    if ident=='S-02':row['status']='INCONCLUSIVE';row['result']='生命周期测试与驻留 DOM/监听计数提供局部证据，不能覆盖所有分支或证明完全无泄漏。'
    if ident in ['P-01','P-02']:row['result']='A/B 同一严格本地接口替身、数据、视口、Chrome；A 原产物，B 包含授权的构建修复。共享主机影响已记录，未以其代表硬件环境。'
source.write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n')
counts=dict(collections.Counter(r['status'] for r in rows))
(root/'acceptance-counts.json').write_text(json.dumps(counts,indent=2)+'\n')
lines=['# 当前修订版逐项验收','', '旧条目全部保留；视觉替换关系仍依据 GOAL-PROMPT。结果绑定本次证据，不继承历史 PASS。', '', '| ID | 状态 | 当前结论 |','|---|---|---|']
lines += [f"| {r['id']} | {r['status']} | {r['result']} |" for r in rows]
Path('docs/macos27/acceptance-migration.md').write_text('\n'.join(lines)+'\n')
print(counts)
