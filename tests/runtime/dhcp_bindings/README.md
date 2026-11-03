# DHCP 归属与生命周期离线验证

普通回归：

```sh
python3 tests/runtime/test_dhcp_bindings.py
```

包含生产函数抽取、构建私有 SDK 输入生成、真实 SDK 解析器和 OFFER 选项生成、同步 TCP/IP pthread 调度，以及受控 NVS、esp-netif 三态与接口 up/down 模型。编译启用 ASan/UBSan。SDK 默认使用 IDF_PATH；也可传入 `--idf-dir`。

带源码对照的启动反证（需要本地 Git 历史）：

```sh
python3 tests/runtime/test_dhcp_bindings.py --output /tmp/pr62-dhcp-review --regression-probes
```

相同启动探针分别编译并执行三个版本的生产函数：

- `aad8d1a59`：复现原 P2，合法保存绑定未能恢复，预期探针退出码 10。
- `f43f3b742`：复现 P1，AP up 后 DHCP 仍 STOPPED，预期探针退出码 11。
- 当前源码：恢复绑定并启动 DHCP，预期退出码 0。

探针的预期失败属于回归反证；整个测试命令应退出 0。结果写入 `regression-probes.json`，每个版本保留编译和执行日志。CI 的普通套件不依赖这些历史对象。

模型依据是 ESP-IDF 5.5.2 `esp_netif_lwip.c` 的 `esp_netif_start_api`、`esp_netif_dhcps_start_api`、`esp_netif_dhcps_stop_api`、`esp_netif_set_ip_info_api` 和 `esp_netif_dhcps_option_api`。INIT 允许随接口自动启动，显式 STOPPED 不允许；start 在 down 时只恢复 INIT，up 时才实际运行。项目锁只能由调用线程持有，TCP/IP 回调不可取该锁、调用 NVS 或同步通知用户；测试对这些边界有断言。

新增验证覆盖三态与 up/down 的两接口组合、排队的 AP up、保存和重启、AP 默认及旧默认空表兼容、DNS 自定义后恢复默认、pending 配置的停服前校验、恢复失败后的保存保护与按接口清空、SDK 各应用阶段失败、NVS 读取/类型/写入/commit 错误。仅绑定更新继续保留真实生产解析器中的租约行为。

DNS 测试编译真实 `add_offer_options`，分别启用和关闭 ADD_DNS：专用 DNS offer 关闭不等于报文没有 DNS。当前工程 ADD_DNS=y 时应回退 AP 地址，租约 120 分钟对应 option 51 的 7200 秒。默认池与真实 SDK 的 DHCPS_MAX_LEASE 及派生源码校验，不能只用测试中另写的一组常量证明一致。

这些结果不证明真实 UDP、Wi-Fi/ETH 驱动、RTOS 调度、NVS 硬件或客户端互通。真机仍需另行授权，检查 AP/ETH 首次获取地址、重连以及 OFFER/ACK 的 option 6、51；不得把 host 结果写成真机验收。

## 审查补充回归

同一套件还覆盖存储恢复错误与 SDK 应用错误分离、接口/namespace 确认不存在后的错误清理，以及“仅剩 blob、缺 count”的反例。普通无记录使用 ESP_ERR_NOT_FOUND；孤立 blob 使用 ESP_ERR_NVS_NOT_FOUND，不能混淆。

客户端到期统计执行生产 AP 事件处理函数和 ETH 队列消费函数。测试队列只提供一次事件后结束模型任务，不模拟真实 RTOS。三处统计统一读 SDK 当前租约，覆盖 AP 初始自动启动、ETH 新客户端/续租以及读取失败；待应用租约不参与统计，读取失败的到期时间为 0（未知），无猜值或重试。
