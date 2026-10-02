"""Summarize this revision's actual samples without a invented regression budget."""
import json,math,statistics
from pathlib import Path
root=Path('output/macos27-20260929-card-revision')
read=lambda p:json.loads((root/p).read_text())
def stats(values):
    values=sorted(values)
    return dict(n=len(values),median=statistics.median(values),p95=values[math.ceil(.95*len(values))-1],min=values[0],max=values[-1]) if values else None
raw=read('performance/raw.json')
result={'status':'INCONCLUSIVE','completed':raw.get('completed',False),'configurationUnchanged':raw.get('configurationUnchanged'),'environment':raw['environment'],'errors':raw['errors'],'labels':{},'limits':['Strict localhost synthetic backend; no new UI deployed to real device.','Cold readiness is the primary LED control, not all asynchronous widgets.','A is the original built baseline. B includes the user-approved two-line CSS build correction.','A/B share host resources; initial UI regression overlapped sampling. Small deltas cannot establish causality.','No allowed 5% or 10% regression budget. Chromium CPU traces do not certify GPU or physical-device cost.','Observer instrumentation adds identical sampling and retained arrays on both sides. Heap growth is not by itself a leak diagnosis.']}
transfer=read('asset-transfer.json')['rows']
scroll=read('performance/scroll.json')
paint=read('performance/paint-summary.json')
follow=read('performance/interaction-followup.json')
for label in ['A','B']:
    residency=raw['residency'][label];samples=residency['samples'];steady=samples[11:];last=samples[-1]
    metric=lambda s,name:next(v['value'] for v in s['metrics'] if v['name']==name)
    result['labels'][label]={
        'coldReadyMs':stats([x['readyMs'] for x in raw['cold'] if x['label']==label]),
        'interactionMs':{key:stats([x['ms'] for x in raw['interactions'] if x['label']==label and (x.get('route')==key or x['type']==key)]) for key in ['/network','/files','/automation','/','modal']},
        'browserInteractionMs':{key:stats([x['browserMs'] for x in follow if x['label']==label and x['route']==key]) for key in ['/network','/files','/automation','/','modal']},
        'cycles':len(residency['cycles']),'durationMs':residency.get('durationMs'),'observedDurationMs':last['at']-residency['start'],
        'steadyDOM':stats([s['dom'] for s in steady]),'steadyListeners':stats([s['counters']['jsEventListeners'] for s in steady]),'steadyDocuments':stats([s['counters']['documents'] for s in steady]),'steadyHeapBytes':stats([metric(s,'JSHeapUsedSize') for s in steady]),
        'longTasks':stats([v['duration'] for v in last['longs']]),'socketsFinal':last['observation']['sockets'],
        'encodedBodyBytes':stats([sum(v['encoded'] for v in x['navigation']+x['resources']) for x in transfer if x['label']==label]),'requests':stats([len(x['resources'])+1 for x in transfer if x['label']==label]),
        'scrollFrameMs':stats([v for x in scroll if x['label']==label for v in x['frames']['deltas']]),
        'traceMs':{key:stats([x['summary'].get(key,{}).get('durationUs',0)/1000 for x in paint if x['label']==label]) for key in ['Paint','PrePaint','Layerize','RasterTask']}}
(root/'performance/summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
lines=['# 当前修订版性能记录','',f"30 分钟驻留完成：{result['completed']}；配置哈希不变：{result['configurationUnchanged']}。总体 **INCONCLUSIVE**，不宣称零成本或设备通过。",'','| 指标 | A 中位 / p95 | B 中位 / p95 |','|---|---:|---:|']
def pair(key):
    return [f"{result['labels'][l][key]['median']:.2f} / {result['labels'][l][key]['p95']:.2f}" for l in ['A','B']]
for label,key in [('冷启动 ms','coldReadyMs'),('首访响应体字节','encodedBodyBytes'),('请求数','requests'),('滚动帧间隔 ms','scrollFrameMs')]:lines.append('| '+label+' | '+' | '.join(pair(key))+' |')
for route in ['/network','/files','/automation','/','modal']:
    values=[result['labels'][l]['browserInteractionMs'][route] for l in ['A','B']]
    lines.append('| '+route+' 浏览器内响应 ms | '+' | '.join(f"{v['median']:.2f} / {v['p95']:.2f}" for v in values)+' |')
lines+=['','冷加载每端 10 次，交互每类型每端 50 次，驻留目标每端 30 分钟且 50 次完整循环。滚动每端 10 次，Trace 每端 5 次仅作诊断。分位采用 nearest rank。', '']
for label in ['A','B']:
    v=result['labels'][label];lines.append(f"- {label}：驻留 {v['durationMs']} ms，{v['cycles']} 次循环，稳定 DOM {v['steadyDOM']}，监听 {v['steadyListeners']}，socket {v['socketsFinal']}。")
lines+=['','## 限制','']+['- '+s for s in result['limits']]
lines+=['','原始数据均在本目录；superseded 批次不进入汇总。首访数据为实际未注入 HTML 的 gzip 响应体，不把 transferSize 的协议头估算当成抓包字节。SPIFFS 全量占用另见 ../build-evidence.json；首访变小不代表 SPIFFS 占用不增长。']
(root/'performance/REPORT.md').write_text('\n'.join(lines)+'\n')
print(json.dumps({'completed':result['completed'],'configurationUnchanged':result['configurationUnchanged'],'labels':{l:{k:result['labels'][l][k] for k in ['coldReadyMs','cycles','durationMs','encodedBodyBytes']} for l in ['A','B']}},indent=2))
