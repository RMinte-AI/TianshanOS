> 2026-09-29：本文记录的是用户已否决版本的历史检查，不是当前修订版验收。当前实施及未解决项见 [CARD-REVISION.md](CARD-REVISION.md)。

# 最终配置本地 A/B 性能

运行状态：已完成；最终配置哈希不变：True。总体 **INCONCLUSIVE**，不能宣称全设备或零成本通过。

A为冻结原版；B为最终正常玻璃。Chrome 152.0.7977.65，macOS，1440×1000，headless，同一严格本地接口替身。驻留两端同时运行；前段与UI截图回归共享主机，后段空闲驻留。

| 指标 | A | B |
|---|---:|---:|
| 冷加载可用 ms 中位/p95 | 85.50 / 88.00 (n=10) | 82.50 / 86.00 (n=10) |
| 首访实际gzip/响应体字节 中位/p95 | 497930.00 / 497930.00 (n=10) | 496437.00 / 496437.00 (n=10) |
| 滚动帧间隔 ms 中位/p95 | 16.70 / 16.80 (n=590) | 16.70 / 16.70 (n=590) |
| /network 响应ms 中位/p95 | 90.50 / 102.00 (n=50) | 85.50 / 95.00 (n=50) |
| /files 响应ms 中位/p95 | 84.00 / 101.00 (n=50) | 99.00 / 101.00 (n=50) |
| /automation 响应ms 中位/p95 | 98.00 / 101.00 (n=50) | 100.00 / 101.00 (n=50) |
| / 响应ms 中位/p95 | 100.00 / 102.00 (n=50) | 100.00 / 101.00 (n=50) |
| modal 响应ms 中位/p95 | 67.00 / 69.00 (n=50) | 67.00 / 69.00 (n=50) |
| 120帧Trace Paint累计ms 中位/p95 | 68.176 / 70.774 | 64.660 / 84.297 |
| 120帧Trace PrePaint累计ms 中位/p95 | 25.772 / 28.804 | 28.620 / 41.329 |
| 120帧Trace Layerize累计ms 中位/p95 | 5.497 / 6.247 | 9.274 / 11.971 |
| 120帧Trace RasterTask累计ms 中位/p95 | 12.317 / 16.787 | 11.308 / 14.429 |

每端冷加载10次；每类交互50次；每端50完整路由/弹窗循环。滚动单独10次×59帧间隔/端；Trace每端5次×120帧。p95用nearest-rank。Trace n=5只作诊断，不能当充分样本的设备GPU结论。

- A：驻留 1827506 ms，50循环；稳定期DOM {'n': 59, 'median': 839, 'p95': 839, 'min': 839, 'max': 839}；监听 {'n': 59, 'median': 169, 'p95': 169, 'min': 169, 'max': 169}；socket {'created': 1, 'closed': 0, 'active': 1}；长任务 None。
- B：驻留 1827313 ms，50循环；稳定期DOM {'n': 59, 'median': 841, 'p95': 841, 'min': 841, 'max': 841}；监听 {'n': 59, 'median': 169, 'p95': 169, 'min': 169, 'max': 169}；socket {'created': 1, 'closed': 0, 'active': 1}；长任务 None。

首访字节来自 `asset-transfer.json`，实际传输未注入harness的index.html.gz；会话替身通过浏览器初始化脚本注入。API只转发127.0.0.1固定替身。`encodedBodySize`为实际响应体；`transferSize`含浏览器Timing头部估计，不能当抓包精确线速字节。完整资源gzip总量和SPIFFS页账另见build-evidence.json。

- 10 alternating cold samples each; ready = primary system LED control present, not all asynchronous widgets completed.
- 50 samples per type each (250 interactions each), wall-clock includes Playwright actionability/transport and two rAF. Frame quantization and shared-host work prevent claiming a small median delta as either proven regression or zero cost.
- Chromium CPU trace durations; no complete GPU/compositor DrawFrame duration returned. Layerize samples increase in B, also in solid diagnostic. Ordinary-glass result is not replaced by diagnostic. Cannot certify zero rendering cost.
- Observer adds a 30s interval, request/sample arrays and JSON display. Same injection on A/B; growing heap includes test observer and GC. Steady DOM/listener/socket counts are evidence of those counters only, not proof of no leaks.
- BLOCKED: no real device heap/PSRAM/CPU/network, physical phone, or native Safari evidence.
- No 5%/10% regression allowance adopted. Overall performance acceptance INCONCLUSIVE.

## 时延补测

针对首轮/files中位数增量，另执行交替50循环/端，用浏览器捕获真实click事件至两个rAF的时间，分开记录自动化驱动往返。该补测没有修改生产文件或最终驻留配置。

| 路由 | A 浏览器内中位/p95 ms | B 浏览器内中位/p95 ms |
|---|---:|---:|
| /network | 64.25 / 66.00 | 63.75 / 65.00 |
| /files | 63.90 / 64.90 | 48.10 / 64.70 |
| /automation | 64.70 / 65.20 | 64.40 / 64.90 |
| / | 64.70 / 99.00 | 64.40 / 65.00 |
| modal | 30.85 / 33.00 | 31.30 / 33.20 |

补测/files的驱动中位数A=100ms、B=83ms，与首轮A=84ms、B=99ms方向相反；浏览器内A=63.90ms、B=48.10ms。说明首轮单个分位增量不能直接归因于换肤。保留两轮全部样本，不挑有利结果，也不把该诊断等同所有环境零回退。
实体诊断中B的Layerize累计中位仍高于A，不能只把差异归为blur；帧间隔样本未显示对应退化，但完整GPU/设备证据不足。

原始证据：`performance/final-v3/raw.json`、`performance/scroll.json`、`performance/trace-*.json`、`performance/diagnostic-solid-trace-*.json`、`asset-transfer.json`。旧批次、选择器失败及中途改变配置的结果均不进入最终驻留结论。
