# DHCP 离线验证

普通回归（ASan/UBSan）：

```sh
python3 tests/runtime/test_dhcp_bindings.py
```

从生产源码抽取函数，编译生成后的真实 IDF DHCP 解析器、同步 bridge、DNS setter 和 OFFER 选项生成函数。SDK 使用 `IDF_PATH` 或 `--idf-dir`，当前依据 ESP-IDF 5.5.2。NVS、驱动、事件、RTOS 和 esp-netif 三态/up-down 为 host 模型；TCP/IP 调度使用 pthread，同步事务内禁止取项目锁、访问 NVS、通知用户，通知在锁外验证。

覆盖独立风险：

- MAC-IP 归属与活动租约分离，续租/释放/过期/重绑定/删除/冲突；池内及池外地址。
- AP 保存网段恢复与自动启动；ETH manager → 使用保存主机地址创建 netif → DHCP 恢复 → manager start → 首次 OFFER。
- ETH 非法主机配置不建接口、错误可查询、NVS 保留；损坏 DHCP 记录只阻断 DHCP，不阻断合法静态管理链路。
- 用户启停意图与 suspend/resume 分离；断链期间 stop、延迟任务前 stop、重连、network restart；失败候选在停服前拒绝。
- pending/applied、NVS 长度/count/类型/缺键/读取错误、按接口保存保护、显式清空不自动保存、SDK 分阶段失败。
- 通知与计时只由确认的实际转换驱动；重复 start 不通知，成功 restart 只通知最终 STARTED，失败停服通知 STOPPED，状态不明不猜。
- SDK 查询错误通过 get_status 返回，get_state 为 ERROR，is_running 为 false；CLI/API 不编造停止状态。
- AP 默认池与 SDK 常量/派生规则核对；真实 OFFER 的租约/DNS、custom DNS → 空后的回退；原网络 setter 的 backup DNS 保留。
- SDK 文本依赖的四种漂移必须拒绝生成。

历史反证不进入 `run_all.sh`，需本地 Git 历史：

```sh
python3 tests/runtime/dhcp_bindings/probes/replay.py --output /tmp/pr62-dhcp-probes
```

同一 AP 探针对 `aad8d1a59` / `f43f3b742` / 当前源码预期退出 10 / 11 / 0；真实 ETH 创建探针对 `c55643827` / 当前为 12 / 0；DHCP 错误隔离探针对 `4abf001a3` / 当前为 15 / 0。脚本逐项匹配预期，整个命令应退出 0，保留编译/执行日志及 `probes.json`。历史专用适配仅存在于此入口。

恢复时可通过 AP/串口修正并保存主机网络配置后重启，原 DHCP 数据留在 NVS；明确更换 DHCP 数据则完整设置池、按接口清空/重建绑定，随后单独保存。未创建 ETH 的情况需要重启。

测试减量只针对重复搭建和重复证明，不以行数目标删独立风险。以上不证明硬件和真实客户端互通；真机 AP/ETH 首个 OFFER、option 6/51、重连仍待授权验收。
