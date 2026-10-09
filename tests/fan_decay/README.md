# 0.6.0 风扇额外风量回落回归

在仓库根目录执行：

```sh
git show v0.6.0:components/ts_drivers/src/ts_fan.c > /tmp/ts-fan-v060-baseline.c
python3 tests/fan_decay/run_host.py --source /tmp/ts-fan-v060-baseline.c --csv /tmp/ts-fan-before.csv
python3 tests/fan_decay/run_host.py --csv /tmp/ts-fan-after.csv
python3 tests/fan_decay/compare.py /tmp/ts-fan-before.csv /tmp/ts-fan-after.csv --output /tmp/ts-fan-comparison.json
node tests/fan_decay/test_ui.cjs
bash tests/fan_monitor/run_host.sh
```

基线是实际 `v0.6.0` 标签的驱动，不使用录像审查包中的旧源码。控制函数、PWM 更新、定时器、模式切换和状态查询均从所选源码提取；只替换时钟、温度服务和 HAL。在需求计算之后、退档许可之前插入测试观察点，不重写算法。C 编译启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。

`video_like_*` 是可解释的状态构造，预置了人工指定的降温历史与起始 PWM；不是还原录像缺失的传感器样本。其他确定性输入每秒明确提供一次新报告，没有将重复画面作为新报告。每行记录需求、请求、成功下发与 HAL 结果，包含升温、反复负载、再次升温、限制、硬保护解除、失温、输出失败和模式切换。页面测试复用生产脚本与现有 DOM 替身，检查中英文页面大读数和滑块；不是实机或真实浏览器验收。

闭环使用两节点热模型：热点与铝散热整体。三组假设参数分别为热点热容/整体热容/两者导热系数：`12 J/K / 1200 J/K / 10 W/K`、`8 / 600 / 8`、`24 / 1800 / 16`。环境 30°C，待机发热 55W，短负载 320W×8秒，重复短负载间隔60秒×4次，持续负载300W；散热系数 `1.5 + 0.18×PWM% W/K`，每秒控制、0.1秒积分。噪声是 ±0.1°C 的周期扰动。这些数值没有通过真机辨识，也不声称代表 TGF3600 参数。它们仅检查反馈方向、再次响应和代价；比较结果必须分别报告温度峰值、风量、调速与方向反转，不能以运行完成宣称热控效果通过。
