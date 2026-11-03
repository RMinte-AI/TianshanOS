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

## ETH 主机配置与运行意图

ETH 主机 IP、掩码和网关由网络管理器保存；DHCP 池的 gateway 不会改变主机 IP。网络管理器先校验保存的静态配置，再将地址传给真正的 netif 创建函数，随后恢复 DHCP。ETH 当前是 DHCP server netif，不支持客户端 DHCP 模式。无记录使用默认；非法地址、非法掩码、不支持模式或实际 NVS 读错误不回退：ETH 不创建、不启动，网络状态/API 报配置错误，DHCP 状态显示接口不可用。AP 按原策略继续初始化。

运行中改址与网络 restart 共用完整 DHCP 应用事务。先按候选主机地址校验池、掩码和全部绑定；校验失败时接口、DHCP 服务及运行请求不变。普通绑定编辑仍只作用于当前网段；换网段前应显式处理旧绑定，不自动搬迁或删除。

ETH 的运行请求只在模块初始化和用户 start/restart/stop 时写入，不保存到 NVS。链路断开使用 suspend，重连延迟任务使用 resume；reload、改址和 net restart 保留请求。链路 resume 使用最后成功应用的配置，保留尚未应用的 pending 编辑。resume 查询接口、链路、请求、存储恢复错误和应用错误，任何阻断都不清错、不重试、不发送虚假的 STARTED 通知。TCP/IP 事务不写意图、不发送通知；通知在项目锁外发送。查询返回真实状态字段，不缓存另一份 blocked 状态。

完整创建探针额外比较 `c55643827`（ETH 仍按默认地址创建，预期退出 12）与当前源码（保存地址、绑定和首次 OFFER 一致，退出 0），结果为 `eth-boot-regression-probes.json`。它执行抽取的 manager init/start、netif 创建、DHCP init、ETH 事件与延迟任务。GPIO/SPI 驱动与事件调度仍为 host 模型。独立 NVS namespace、真实 cJSON 和 CLI 输出用于检查错误能到达查询结果。

恢复错误时，先通过可用的 AP 或串口查看网络配置及具体错误。若主机网络配置写错，修正并保存网络配置后重启；原 DHCP 池和绑定仍留在 NVS，重新启动会再次恢复。若确实要更换 DHCP 数据，完整设置池并按接口显式清空/重建绑定，确认后单独执行 DHCP 保存。清空绑定只改变指定接口内存和生效表，不自动保存。启动时未创建 ETH 的情况需要在修正、保存后重启设备。

真机 AP/ETH 首次 OFFER、option 6/51 和重连仍为待验；CI/host 通过不能替代此项。
