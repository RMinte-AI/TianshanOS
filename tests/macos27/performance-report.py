"""Summarize raw samples without inventing a permissible regression budget."""
import json,math,statistics,datetime
from pathlib import Path
b=Path('output/macos27-20260928');raw=json.loads((b/'performance/final-v3/raw.json').read_text())
def stats(v):
 v=sorted(v);return {'n':len(v),'median':statistics.median(v),'p95':v[math.ceil(.95*len(v))-1],'min':min(v),'max':max(v)} if v else None
result={'environment':raw['environment'],'completed':raw.get('completed',False),'configurationUnchanged':raw.get('configurationUnchanged'),'errors':raw['errors'],'labels':{},'scope':'Local Chromium, final ordinary glass. Synthetic device responses. No zero-cost/all-device claim.'}
for label in ['A','B']:
 cold=[x for x in raw['cold']if x['label']==label];r=raw['residency'][label];samples=r['samples'];last=samples[-1]
 metrics=lambda s:{x['name']:x['value']for x in s['metrics']}
 settled=samples[11:] # after all 50 cycles; first snapshots are deliberately not called steady state
 result['labels'][label]={'coldReadyMs':stats([x['readyMs']for x in cold]),'interactionMs':{key:stats([x['ms']for x in raw['interactions']if x['label']==label and(x.get('route')==key or x['type']==key)])for key in ['/network','/files','/automation','/','modal']},'cycles':len(r['cycles']),'durationMs':r.get('durationMs'),'observedDurationMs':last['at']-r['start'],'samples':len(samples),'steadyDOM':stats([s['dom']for s in settled]),'steadyListeners':stats([s['counters']['jsEventListeners']for s in settled]),'steadyDocuments':stats([s['counters']['documents']for s in settled]),'steadyHeapBytes':stats([metrics(s)['JSHeapUsedSize']for s in settled]),'longTasks':stats([x['duration']for x in last['longs']]),'socketsFinal':last['observation']['sockets'],'observedRequests':len(last['observation']['requests']),'observerErrors':last['observation']['errors']}
transfer=json.loads((b/'asset-transfer.json').read_text())
for label in ['A','B']:
 v=[x for x in transfer['rows']if x['label']==label]
 result['labels'][label]['actualArtifactTransfer']={'requests':stats([1+len(x['resources'])for x in v]),'encodedBodyBytes':stats([sum(e['encoded']for e in x['resources']+x['navigation'])for x in v]),'transferSizeIncludingTimingHeaderEstimate':stats([sum(e['transfer']for e in x['resources']+x['navigation'])for x in v]),'navigationGzipBytes':stats([x['navigation'][0]['encoded']for x in v]),'pageErrors':[e for x in v for e in x['errors']]}
scroll=json.loads((b/'performance/scroll.json').read_text());paint=json.loads((b/'performance/paint-summary.json').read_text());solid=json.loads((b/'performance/diagnostic-solid-paint-summary.json').read_text())
for label in ['A','B']:
 result['labels'][label]['scrollFrameMs']=stats([v for x in scroll if x['label']==label for v in x['frames']['deltas']])
 for key,data in [('glassTrace',paint),('solidDiagnosticTrace',solid)]:result['labels'][label][key]={event:stats([x['summary'].get(event,{}).get('durationUs',0)/1000 for x in data if x['label']==label])for event in ['Paint','PrePaint','Layerize','RasterTask']}
follow=json.loads((b/'performance/interaction-followup.json').read_text())
result['interactionFollowup']={label:{route:{metric:stats([x[metric]for x in follow if x['label']==label and x['route']==route])for metric in ['browserMs','driverMs']}for route in ['/network','/files','/automation','/','modal']}for label in ['A','B']}
result['interpretation']={
'load':'10 alternating cold samples each; ready = primary system LED control present, not all asynchronous widgets completed.',
'latency':'50 samples per type each (250 interactions each), wall-clock includes Playwright actionability/transport and two rAF. Frame quantization and shared-host work prevent claiming a small median delta as either proven regression or zero cost.',
'paint':'Chromium CPU trace durations; no complete GPU/compositor DrawFrame duration returned. Layerize samples increase in B, also in solid diagnostic. Ordinary-glass result is not replaced by diagnostic. Cannot certify zero rendering cost.',
'residency':'Observer adds a 30s interval, request/sample arrays and JSON display. Same injection on A/B; growing heap includes test observer and GC. Steady DOM/listener/socket counts are evidence of those counters only, not proof of no leaks.',
'device':'BLOCKED: no real device heap/PSRAM/CPU/network, physical phone, or native Safari evidence.',
'budget':'No 5%/10% regression allowance adopted. Overall performance acceptance INCONCLUSIVE.'}
(b/'performance/summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
lines=['# 最终配置本地 A/B 性能','',f"运行状态：{'已完成' if result['completed']else'30分钟驻留仍在进行'}；最终配置哈希不变：{result['configurationUnchanged']}。总体 **INCONCLUSIVE**，不能宣称全设备或零成本通过。",'', 'A为冻结原版；B为最终正常玻璃。Chrome '+raw['environment']['browser']+'，macOS，1440×1000，headless，同一严格本地接口替身。驻留两端同时运行；前段与UI截图回归共享主机，后段空闲驻留。','', '| 指标 | A | B |','|---|---:|---:|']
for name,key in [('冷加载可用 ms','coldReadyMs'),('首访实际gzip/响应体字节','actualArtifactTransfer'),('滚动帧间隔 ms','scrollFrameMs')]:
 def f(label):
  d=result['labels'][label][key];d=d['encodedBodyBytes']if key=='actualArtifactTransfer'else d;return f"{d['median']:.2f} / {d['p95']:.2f} (n={d['n']})"
 lines.append('| '+name+' 中位/p95 | '+f('A')+' | '+f('B')+' |')
for key in ['/network','/files','/automation','/','modal']:
 def f(label):
  d=result['labels'][label]['interactionMs'][key];return f"{d['median']:.2f} / {d['p95']:.2f} (n={d['n']})"
 lines.append('| '+key+' 响应ms 中位/p95 | '+f('A')+' | '+f('B')+' |')
for key in ['Paint','PrePaint','Layerize','RasterTask']:
 def f(label):
  d=result['labels'][label]['glassTrace'][key];return f"{d['median']:.3f} / {d['p95']:.3f}"
 lines.append('| 120帧Trace '+key+'累计ms 中位/p95 | '+f('A')+' | '+f('B')+' |')
lines+=['','每端冷加载10次；每类交互50次；每端50完整路由/弹窗循环。滚动单独10次×59帧间隔/端；Trace每端5次×120帧。p95用nearest-rank。Trace n=5只作诊断，不能当充分样本的设备GPU结论。','']
for label in ['A','B']:
 d=result['labels'][label];lines.append(f"- {label}：驻留 {d['durationMs'] if d['durationMs'] is not None else d['observedDurationMs']} ms，{d['cycles']}循环；稳定期DOM {d['steadyDOM']}；监听 {d['steadyListeners']}；socket {d['socketsFinal']}；长任务 {d['longTasks']}。")
lines+=['','首访字节来自 `asset-transfer.json`，实际传输未注入harness的index.html.gz；会话替身通过浏览器初始化脚本注入。API只转发127.0.0.1固定替身。`encodedBodySize`为实际响应体；`transferSize`含浏览器Timing头部估计，不能当抓包精确线速字节。完整资源gzip总量和SPIFFS页账另见build-evidence.json。','']
lines += ['- '+v for v in result['interpretation'].values()]
lines += ['','## 时延补测','', '针对首轮/files中位数增量，另执行交替50循环/端，用浏览器捕获真实click事件至两个rAF的时间，分开记录自动化驱动往返。该补测没有修改生产文件或最终驻留配置。','', '| 路由 | A 浏览器内中位/p95 ms | B 浏览器内中位/p95 ms |','|---|---:|---:|']
for route in ['/network','/files','/automation','/','modal']:
 a=result['interactionFollowup']['A'][route]['browserMs'];d=result['interactionFollowup']['B'][route]['browserMs'];lines.append(f"| {route} | {a['median']:.2f} / {a['p95']:.2f} | {d['median']:.2f} / {d['p95']:.2f} |")
lines += ['', '补测/files的驱动中位数A=100ms、B=83ms，与首轮A=84ms、B=99ms方向相反；浏览器内A=63.90ms、B=48.10ms。说明首轮单个分位增量不能直接归因于换肤。保留两轮全部样本，不挑有利结果，也不把该诊断等同所有环境零回退。', '实体诊断中B的Layerize累计中位仍高于A，不能只把差异归为blur；帧间隔样本未显示对应退化，但完整GPU/设备证据不足。']

lines += ['','原始证据：`performance/final-v3/raw.json`、`performance/scroll.json`、`performance/trace-*.json`、`performance/diagnostic-solid-trace-*.json`、`asset-transfer.json`。旧批次、选择器失败及中途改变配置的结果均不进入最终驻留结论。']
Path('docs/macos27/PERFORMANCE.md').write_text('\n'.join(lines)+'\n')
print(json.dumps({'completed':result['completed'],'configurationUnchanged':result['configurationUnchanged'],'labels':{l:{k:result['labels'][l][k]for k in ['coldReadyMs','cycles','durationMs','actualArtifactTransfer']}for l in ['A','B']}},indent=2))
