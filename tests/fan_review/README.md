# 0.6.0 回落复审的有限返修回归

不访问任何设备。`run_host.py` 从所选生产源码提取驱动控制、手动/紧急/初始化/恢复/使能、状态 API、旧 device.fan.status 和控制台函数。时钟、温度输入、HAL 和持久化替换为受控接口，真实 cJSON 负责生成 JSON；启用 ASan/UBSan。`--check` 检查最终实现；`--source` 与各接口源码参数可单独选择原版或上一轮候选，默认 probe 只运行复现，不宣称缺陷通过。

```sh
python3 tests/fan_review/run_host.py --check --output /tmp/fan-review.jsonl
node tests/fan_review/test_ui.cjs /tmp/fan-review.jsonl
bash tests/fan_monitor/run_host.sh
python3 tests/fan_decay/run_host.py --csv /tmp/fan-decay.csv
```

cJSON 默认使用本地 ESP-IDF；无本地 IDF 时，用 `--cjson-dir /absolute/path/to/cJSON` 指定，目录须含 cJSON.c/h。交付包提供这两个测试依赖文件。

38 个记录覆盖正常缓存、长/短观测断段、源/绑定变化、时钟异常、失温/重新保护、手动成功与失败、紧急、初始化未知、恢复失败、OFF、使能、反相逻辑 PWM、两个状态接口、控制台、实际 fan.set 处理器、分数/长间隔回落与再次升温。JSONL 中的 `hal_output` 是 HAL 替身接受的参数；反相时逻辑百分比为 `100-hal_output`，不是物理风扇机械速度。

`--cadence-only --period-ms 250/5000/60000` 检查实际配置周期影响观测连续性；保护观测间隔上限为两个配置周期与现有10秒输入有效期中的较小值。连续有效缓存不强制要求新报告；60秒周期不能把两个低温端点算成连续30秒。温度源绑定→身份版本→驱动保护的真实接线另由 fan_monitor 验证。

`test_ui.cjs` 在生产 JS 和轻量 DOM 替身中消费驱动生成的 JSON；`browser_check.js` 由 Playwright CLI `run-code` 执行，在本地服务的源码 `/` 与编译网页 `/candidate/` 上检查实际 DOM、输入/提交、轮询、明确失败、未知结果、停用设置及390px布局。接口为模拟，外部网络阻断，不是实机或真实用户测试。服务还需 `/evidence/repaired-repro.jsonl` 路由。

27 场景和12假设热模型的完整轨迹仍由 fan_decay 工具独立生成。`compare_replay.py` 只允许预测显示在跨工具链出现≤0.1°C差异，斜率/增益允许1e-6浮点差异；需求、请求、成功输出、保护、失温和其他字段必须一致。它不能把热模型运行完成写成实际散热效果通过。
