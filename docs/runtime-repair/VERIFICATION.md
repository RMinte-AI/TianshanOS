# 验证记录

日期：2026-09-24。PASS 只针对列出的环境；PARTIAL 表示已有局部证据但该验收项尚未全部验收。NOT RUN 表示未执行。没有连接板卡或现场服务器。

## 可复现命令与测试模型

```sh
./tests/runtime/run_all.sh
./tests/certificate/run_host.sh
# 本机 SDK /Users/massif/esp/v5.5.2/esp-idf，使用其 Python 3.12 环境：
./tools/build.sh
```

- runtime：9 个套件 PASS，输出在 `evidence/host-tests.log`。源 JS 与中英语言文件语法检查通过。
- certificate：此前 8 组回归再次运行，输出在 `evidence/certificate-tests.log`。服务停止回归先运行旧实现故障复现，再运行修复实现；旧实现失败为预期结果。
- 构建：PASS，完整输出在本地工件目录 `target-build.log`；构建结果摘要及工件哈希保存在 evidence。没有执行构建日志给出的 flash 命令。
- C 使用 ASan/UBSan；并发由 pthread/屏障模拟，不是 ESP32 双核调度。核心算法来自实际源码；I/O 与 RTOS 边界 stub。引擎套件模拟执行/存储接口，分别由 SSH/持久化套件验证这些实现，不能声称端到端固件运行。
- store：188 次单点失败位置扫描（NVS、SD 两种来源各 94 次）和 79 个模拟崩溃位置。每次期望完整旧版/新版或显式拒绝恢复；部分编号超过实际路径调用次数，是无故障控制，不能全部称作独立失败分支。NVS mock 不模拟实际 flash 页/GC/电源中断。
- codec：9 类动作、条件、repeat/动作条件、显式 false、整数值 FLOAT 类型保留、缺失模板保留；条件/动作内存失败，以及 encoder 每个 cJSON 分配位置失败。没有枚举所有字段组合的笛卡尔积。
- SSH：未知/变更主机认证调用数为 0；合法确认可认证；握手、认证、开 channel、exec、读输出、close 六阶段永久 EAGAIN/取消；stderr-only 释放。真实 libssh2 和 lwIP 的失败释放仍需实机。
- UI：实际函数在 VM 中运行；草稿/冲突/不确定写、双语、模板优先、日志请求防重入、旧 DOM 响应丢弃。没有用静态字符串搜索冒充页面交互测试，也没有把 VM 当浏览器渲染。

## 按交接文档映射

| 编号 | 状态 | 已有证据与仍未覆盖范围 |
|---|---|---|
| P01 | PARTIAL | UI 函数保存手动条件/类型并单次 update；codec round-trip；真实浏览器卡片布局/切换未运行 |
| P02 | PARTIAL | engine 遍历 enabled/manual/allow 组合；UI 显示由 show 决定；真实页面全部组合未运行 |
| P03 | PARTIAL | codec 缺省/显式 false、引擎兼容解析代码审查；完整历史现场配置未提供 |
| P04 | PARTIAL | 缺失模板序列化保留；UI 模板优先于缓存命令、请求失败 unresolved；启动加载顺序实机未运行 |
| P05 | PASS（主机） | 条件真时 manual 不自动准入，合法手动按 allow/enabled 准入 |
| P06 | PARTIAL | 引擎正常评估准入、保存 handler 无执行调用；真实调度循环未运行 |
| P07 | PARTIAL | 实际 service registry 拒绝重复准入，远端 mock；真实 Linux 锁目录/跨规则同时启动未运行 |
| P08 | PARTIAL | engine expected_revision 冲突、UI 单次带版本 update 和保留草稿；两个真实浏览器未运行 |
| P09 | PARTIAL | 主机/命令/模板/规则共同 binding gate 静态审查；已停止重绑定通过；全部 API 并发场景未运行 |
| C01 | PASS（主机范围） | codec 分配位置失败、engine 候选失败后旧配置/版本保留；生产内存压力未测 |
| C02 | PASS（模拟） | 短写、flush/fsync/close/rename、NVS set/commit/readback 故障扫描，错误传播 |
| C03 | PASS（模拟） | 主存储/镜像阶段故障及恢复检查；真实 NVS 满页/GC 未测 |
| C04 | PASS（模拟） | 不确定 guard/回滚路径不能作为成功；恢复失败不加载规则。真实硬件故障未测 |
| C05 | PASS（模拟） | 79 个模拟中断位置恢复为完整旧/新或 fail-closed；板卡断电 NOT RUN |
| C06 | PARTIAL | SD/NVS commit/load 与只读拒绝测试；完整加密包/旧单文件的设备升级回归未运行 |
| C07 | PARTIAL | 9 类动作及代表性非默认字段、条件/repeat round-trip；现场所有动作字段与保存重启未运行 |
| C08 | PARTIAL | 非法操作符、ID、逻辑 enum、引用/内存错误、probe 溢出测试；所有字段极限与 32 条最大实体容量未穷举 |
| O01 | PASS（主机） | variable getter 屏障中更新：旧判断不准入；执行屏障中更新：旧动作快照有效 |
| O02 | PASS（主机） | 活动执行禁止删除、同 ID 重建 instance 改变、统计不继承；旧代 ready 测试另覆盖 service |
| O03 | PARTIAL | 持有旧执行与新读者时第二个退役提交被拒绝，释放后归零；板卡 100 次高频编辑未运行 |
| L01 | PASS（主机） | 510/511/512/1024/4096 字节原始输出完整，自有日志 4096B 完整，缓存明确截断 |
| L02 | PARTIAL | 4 pthread × 1000 条、槽耗尽计数及锁争用降级；ESP32 双核/同核抢占时序未运行 |
| L03 | PASS（主机） | UTF-8/ANSI/末尾 m/空行及完整回传验证 |
| L04 | PASS（主机范围） | 实际 hook、自有入口、capture 开关、回调递归；init/deinit 与外部 IDF logger 极端并发未运行 |
| L05 | PARTIAL | 共同 client 所有日志级别捕获不含合成密码；静态移除私钥/原始命令诊断；全部 vendor 失败路径未动态执行 |
| W01 | PASS（主机） | task 创建失败、创建后立即 READY/结束，槽登记及释放正确 |
| W02 | PARTIAL | 协作停止/自然退出、资源计数；全部 stop_all/deinit 竞态组合及真机循环未运行 |
| W03 | PASS（主机） | 槽复用后旧句柄不能取消新 worker |
| W04 | PARTIAL | 间隔 notify 取消与 client 六阶段 deadline/cancel 测试；真实 TCP/DNS 丢包未运行，DNS 不可立即取消 |
| W05 | PASS（主机） | cancel 后旧 generation READY 不改 unknown/stopped |
| W06 | PASS（主机） | stderr-only/error、session/channel/socket/key 测试计数归零；真实库运输层释放需板卡验证 |
| S01 | PASS（主机） | 共同 client 匹配/未知/变更/明确 verifier 的认证门禁 |
| S02 | PARTIAL | API/CLI/自动/watcher/shell 调用点共同入口静态审查与完整编译；各入口真实服务器握手未运行 |
| S03 | PARTIAL | 永久 EAGAIN 不刷新 deadline、取消停止后续工作；真实丢包/DNS 未运行 |
| S04 | PASS（本机 shell） | 实际 C builder + /bin/sh：单引号、空格、前导 -、shell 元字符按字面量，溢出拒绝 |
| S05 | PASS（主机） | 精确 token、重复/前后缀拒绝、FAIL 优先；异常 exec 不当作 ready |
| U01 | PARTIAL | 日志请求串行、旧弹窗响应丢弃、查询错误保守状态；真实页面完整乱序/断网未运行 |
| U02 | PARTIAL | 启动后 watcher 创建失败、registry unknown 禁止盲启动；真实 exec 后断线未运行 |
| U03 | PARTIAL | watcher cancel 保留远端运行，remote stop mock 证据后 stopped；真实进程组 TERM 未运行 |
| U04 | PARTIAL | 自动模式停止提示/后端无永久抑制静态检查；真实用户交互及自动恢复未运行 |
| E01 | NOT RUN | 未连接现场三模型，不启动/停止真实模型，不刷机或设备重启 |
| E02 | PARTIAL | 优化 JS 语法/gzip、资源版本、SPIFFS 重生成哈希一致；设备浏览器实际下载/缓存未运行 |
| R01 | NOT RUN（运行回归） | 目标 sizeof 与静态容量审计完成；下表实机前后数据均缺失 |

## 同板卡资源表（待设备测试授权/环境）

| 场景/指标 | 修复前 | 修复后 | 判定 |
|---|---|---|---|
| 空闲面板：内部当前/最小空闲/最大块、PSRAM、DMA | 未测 | 未测 | NOT RUN |
| 页面进出、断网恢复：任务/socket/watcher、堆趋势 | 未测 | 未测 | NOT RUN |
| 32 条无匹配规则：评估 p95、allocation instrumentation | 未测 | 未测 | NOT RUN |
| 100 次模式切换：NVS/SD 时延、最大保存容量、碎片 | 未测 | 未测 | NOT RUN |
| 100 轮安全测试进程 start/stop/timeout/cancel：堆、栈 HWM、socket | 未测 | 未测 | NOT RUN |
| 同配置普通 API p95，解释超过 10% 的退化 | 未测 | 未测 | NOT RUN |
| 三模型链及持续观察：任务栈 HWM、内存/连接稳定性 | 未测 | 未测 | NOT RUN |

DNS 原生等待、旧单 PID 实例接管、NVS 双 bank 48KiB 共享容量和真实 Linux process group 行为是下一轮优先核验点。没有从旧 0.5.1 串口日志推断本次修复后的任何运行指标。
