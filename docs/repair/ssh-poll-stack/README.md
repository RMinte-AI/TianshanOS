# PR42 ssh_poll 栈预算与 WithCaps 释放修复（2026-09-28）

## 状态与范围

本地回归、固定 SDK 正式/诊断构建及两版各10轮真机 SSH 验收已完成，最后的设备离线首次加载与 J1 真通知也已补齐。正式候选已恢复。最低观测剩余栈分别为3876/3644 bytes，所测负载未复现原溢出；不扩张为任意负载安全或绝对零泄漏。详见文末最新结果与 `device-final/`。以下阶段记录保留历史顺序。

基线 HEAD `3a19128d8638771c958fe47df1d6a7d339a10a4c`，分支 `fix/v0.5.2-reliability-and-prompts`。起始无 tracked 修改；已有 `output/`、`tmp/`、`docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt` 保留。适用用户提供 AGENTS 规则；本仓及所改目录未发现额外 AGENTS 文件。

生产仅修改 `components/ts_webui/src/ts_webui_ws.c`：定义 `SSH_POLL_STACK_SIZE 8192`，Shell poller 创建参数使用该常量，退出改 `vTaskDeleteWithCaps(NULL)`。输出缓冲仍 2048，优先级仍 5，PSRAM 分配不变。没有修改前端、协议、operation、TX、SSH 驱动、保护、版本或发布配置。额外栈成本为单实例 4096 bytes PSRAM。

## 原始故障与静态检查

`crash.txt` 是本轮修改前真实设备故障摘录：Shell opened 后 `ssh_poll` stack overflow，继而软件重启。使用原 ELF 解析为 FreeRTOS `vApplicationStackOverflowHook`；回溯尾部 CORRUPTED，不能从该回溯推出最早出错的业务帧。原源码快照 `/tmp/ssh-poll-before.c`，哈希见 manifest.json，可由上述 HEAD 对照。

已检查：

- poller 以 `sizeof(buf)-1` 即 2047 传入 `ts_ssh_shell_read`。
- `ts_ssh_shell_read` 原样把 size 传给 libssh2；项目内 channel.c 的 `_libssh2_channel_read` 循环限定 `bytes_read < buflen`，复制长度不超过 `buflen-bytes_read`。
- `ssh_send_output` 先取得原 operation 输出票据，再分配 len+1，复制 len 字节，在 buf[len] 放终止符；该 poller 的 len 来源受上面的 read 长度限制。
- cJSON 使用终止字符串；output_finish 复制 JSON 到原有消息对象，再按原投递身份结算。不修改 caller buf。资源关闭、executor_done、task delete 顺序未变。
- 未发现这一直接路径针对 caller buf 的明确越界证据。这不是对整个 libssh2 或全部运行时内存安全的证明。

因此实施用户指定的 8192 预算方案，而不是声称静态检查已排除所有内存破坏。

## 官方 SDK 删除契约与限制

固定 `/Users/massif/esp/v5.5.2/esp-idf`：`components/freertos/esp_additions/include/freertos/idf_additions.h` 明确 WithCaps 栈单位为 bytes，必须由 vTaskDeleteWithCaps 删除。既有 FreeRTOS 头链提供声明，两份完整构建均无需增加 include。

SDK `esp_additions/idf_additions.c` 表明普通 vTaskDelete 无法释放 WithCaps 静态分配的 TCB/stack。本次修正调用配对。self-delete 在 SDK 内创建短暂清理任务后挂起原任务；失败时 SDK abort。这是官方实现，不是本轮新增自定义任务或 wrapper。真机采样需允许清理任务得到调度后再判断残留；不能把主机删除替身当成 SDK 内存释放证明。

## 测试变更与结果

复用 `tests/ws_subscriptions/test_operation_adapter.c`，包含实际生产 WS/control/TX 实现：

- 静态断言常量 8192，创建边界断言 WithCaps、8192、PSRAM。
- WithCaps 删除边界断言 self-delete、executor 已退出、仍可取得的原上下文 shell/session 已清空。
- 新增 10 轮 Shell 创建/退出，逐轮必须调用 WithCaps delete；排空后 OP_FREE、零 refs/executors 和零 tracked allocation。
- 保留原创建失败/分配失败扫描与所有生命周期断言。

`before.txt`：修改生产前，创建参数断言在实际 4096 参数处失败（不是编译/依赖错误）。之后补充常量静态断言。`after.txt`：同一创建参数断言及新增删除/重复退出断言通过。它证明参数与资源流程，不是模拟 ESP32 栈安全。

执行环境：`IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf`，`DEVELOPER_DIR=/Library/Developer/CommandLineTools`。

| 入口 | 本轮结果与行为边界 |
| --- | --- |
| `python3 tests/ws_subscriptions/run_project_v3.py`（V3_RESULTS 指向 results） | 全部入口 exit 0；host/operation/adapter/control/reviewer/F1-F2/runtime/certificate/shell_driver/Node/双语跨层。覆盖 R1–R5、F1/F2、G1/G2、V1/V2、H1/J1 有效回归；见 results/host.json 和逐项日志 |
| `npm test --prefix tests/prompts`（上述 runner 内） | 40 通过，含 J1 实际输入/显示状态测试 |
| `python3 tests/ws_subscriptions/run_project_v3.py --browser` | Chrome 38 通过，双语 C→JS→C Chrome 跨层通过，见 results/browser.json |
| `npm run test:xterm --prefix tests/prompts` | 4 通过；真实 xterm，禁止外部 origin，网络记录 requests-source-*.json |
| 同一 browser runner，PROJECT_WEB_ROOT 指向候选 web_optimized | 浏览器及双语跨层通过，见 optimized/ |
| `PROJECT_WEB_ROOT=.../web_optimized node --test tests/prompts/xterm-offline.test.cjs` | 双语真实 xterm 首次离线加载/输入/J1/退出通过，见 optimized-xterm.txt 和 candidate/requests-*.json |
| `git diff --check` | 通过 |

首次构建因沙箱拒绝 SDK psutil 进程枚举、Chrome 因沙箱启动失败；随后在获准的沙箱外本地环境重跑通过，没有更换 SDK/测试依赖。源码和构建输入自构建开始后未再修改；后续只补测试常量断言并重跑受影响主机/跨层测试。

## 两份构建与静态资源

- 正式候选：`/tmp/tianshan-ssh-stack-candidate/build`；App 0x215120，分区 31% 空余。
- 诊断版：`/tmp/tianshan-ssh-stack-diagnostic/build`；App 0x259ee0，分区 22% 空余。
- SDK 均 v5.5.2、ESP32-S3，独立完整 build；各目录 build.sh 保存精确构建命令。
- 正式配置保留原 canary；诊断临时 sdkconfig 启用 `FREERTOS_CHECK_STACKOVERFLOW_CANARY`、`FREERTOS_WATCHPOINT_END_OF_STACK`、`COMPILER_STACK_CHECK_MODE_ALL`、`HEAP_POISONING_LIGHT`。正式 sdkconfig 未改。此处 sdkconfig-evidence.txt 仅证据，不是默认构建配置。
- 固定 SDK 的 Kconfig/端口实现未发现上述 watchpoint 对本目标 PSRAM stack 的禁用条件，诊断配置实际进入 sdkconfig.h 并编译通过；硬件触发有效性未实测。watchpoint 只监测栈尾区域，且最多占用约 60 bytes，不是任意越界探测器。
- 两份 TianShanOS.bin/www.bin/ELF 哈希见各自 assets.json。两份 xterm JS/CSS/fit 的优化原文件逐字节等于 vendor 源，gzip 解压也相等，详见 manifest.json。
- 两份均执行 `tests/ws_subscriptions/verify_project_assets.py`，由最终 web_optimized 重建 SPIFFS 与实际 www.bin 逐字节一致，打包无容量溢出。
- flasher_args.json 均含 0x6a0000:www.bin。构建日志中的 flash 用法仅 SDK 提示，本轮未执行 flash。

## 初始待授权真机验收计划（历史，后续执行结果见下文）

1. 先确认可输入命令的 UART0 通道。当前 USB Serial/JTAG 已用于收日志，但当前固件 CLI stdin 为 UART0；装了 CH344 驱动不等于 CH344 通道已连接。不能在活动远端 Shell 中输入 system --tasks 冒充设备 CLI 查询。
2. 获得刷机授权后分别记录候选/诊断固件哈希，完整刷 App+www，保持串口日志。
3. welcome 后、有限 200 行输出过程中与结束后、pwd/uname 输入后、resize 后、退出前采样 system --tasks 的 ssh_poll STACK。
4. 本 SDK Xtensa `StackType_t` 为 uint8_t，high-water 计算除 sizeof(StackType_t)，本项目直接打印 usStackHighWaterMark，故单位为 **bytes**。记录全程最低值，目标至少约 2048 bytes；仅几百 bytes 或继续溢出则停止并报告，不自行再增栈。
5. 至少 10 轮 connect/welcome/input/Ctrl+\\/closed，每轮退出清理后采集 tasks 与 memory，检查 ssh_poll 消失及 PSRAM/heap 趋势。当前产品没有公开 operation ledger 查询接口：OP_FREE 直接断言目前仅有主机证据，不能用 tasks 消失冒充真机 ledger 读取。若要求真机直接读 ledger，需要另行确定观测手段，本轮不加生产接口。
6. 诊断版重复全部负载，检查 canary/watchpoint/heap poison/assert/panic/watchdog/load-store 错误；出现任何项均不能宣布关闭。

**本轮没有新真机结果，没有 high-water 数值，也没有真机 10 轮 PSRAM 释放证据。** 本地交付已准备好；只有完成上述授权验收后才可判断 8192 预算是否足够以及是否支持原始栈预算不足的根因解释。未提交、推送或更新 PR。

## 后续用户授权真机执行（阶段记录）

用户在本地交付后明确授权刷机验收。候选版完整 flash App+www 成功，启动 ELF 前缀 2c29402b1 与候选 ELF 一致，12 服务正常。用户通过真实 Safari 完成 welcome、200 行有限输出、pwd/uname、窗口调整及 Ctrl+\ 退出；10 轮连接/输入/退出均有用户确认及逐轮 API 观测，见 device-candidate。

UART0 尚无输入通道，复用现有只读 system.tasks API：与 system --tasks 同为 uxTaskGetSystemState/usStackHighWaterMark，单位 bytes，不改变生产接口。全程最低值 3876 bytes（第 10 轮）；每轮退出后 ssh_poll 数为 0，无检测到的 panic/stack overflow/assert/watchdog。PSRAM 空闲从第 1 轮 7614416 到第 10 轮 7613736 bytes，中间第 8 轮下降后第 9 轮回升；未出现每轮一个任务栈大小的泄漏，但小幅净差仍不能证明零泄漏，也不能直接读取真机 operation FREE。此轮未作堆分配归因。

诊断版验收正在进行，最终根因关闭结论仍保留。

## 诊断真机结果（2026-09-28，自动阶段完成）

正式候选 10 轮由用户通过 Safari 完成。诊断版第 1 轮用户完成 welcome、200 行输出、pwd/uname、resize、Ctrl+\\ 和本地 help；用户随后明确授权代理自动完成余下测试。第 2–10 轮使用 Node 原生 WebSocket 连接设备原有 WS handler，真实设备再连接 10.10.99.98，只执行独立 echo 标记并发送既有 ssh_disconnect。该九轮是**真实设备/真实 SSH 的协议自动验收，不是九轮浏览器键盘验收**；前述人工轮覆盖了实际 UI。

自动脚本第一尝试因未剥离 bracketed-paste ANSI 控制字符而未匹配 echo 输出，正常断开后停止，没有计为通过。后续脚本保存原始输出并剥离 CSI，仅接受一整行等于唯一 marker（不能将命令回显当成执行结果）。九轮均收到 connecting→connected→disconnecting→closed，验证了 echo 返回与 poller 消失。脚本见 device-diagnostic/rounds.mjs；凭据从环境读取，未写入脚本或证据。

- 诊断全程最低观测剩余栈 **3644 bytes**（第 6 轮主动阶段采样）；后台每 2 秒采样最低 3984 bytes。必须合并两种采样，不能只取定时器数据。candidate 最低 3876 bytes。8192 减上述水位分别约 4548/4316 bytes，支持实际用栈超过原 4096 预算；水位不是对每条指令的精确峰值追踪。
- 所有诊断轮次退出后无 ssh_poll；最终内部 free 25671 bytes 与第 1 轮相同。PSRAM 第 1 轮后 7596844，自动第 6 轮 7587612、第 7 轮 7593832、第 10 轮 7594840，最终空闲 7595232 bytes。存在波动与回升，未呈逐轮固定下降，未出现 WithCaps 栈/TCB 规模的重复泄漏；最终比初轮少 1612 bytes，未做分配归因，不能宣称绝对零泄漏。
- 本次保存的诊断日志没有 stack/canary/watchpoint/heap corruption/assert/panic/watchdog/load-store 错误。增强检测只覆盖本次负载及工具检测范围，不是形式化排除任意内存破坏。
- 真机无公开 operation ledger 读取入口；FREE/ref 闭合由生产 C 主机回归直接验证，真机以任务消失、重复创建成功和内存趋势间接验证，不混淆证据层级。
- 两版最低余量均超过约 2 KB 门槛，实际负载未再次触发原崩溃，证据支持原 4 KB 栈预算不足。可进入后续最终真机验收；全系统/J1 真机电源告警、完全断公网首载等不因本轮通过自动判定完成。

测试后恢复正式候选 App+www，启动复核记录另见 device-candidate/restore-*。没有提交、推送、更新 PR 或改动额外生产代码。

## 最后两项真机补验（2026-09-28，正式候选）

证据位于 `device-final/`。没有新增生产修改、刷机或配置变更。

1. **禁止公网的首次加载**：Playwright 启动全新 Chrome context，无既有缓存/登录，禁用 Service Worker；路由仅放行 `http://10.10.99.97`，其他 origin 一律 abort。用户仅完成登录，后续操作由代理通过 CUA 执行。未注入 Terminal/FitAddon/WebSocket 或伪造服务器响应。实际三个 vendor URL 均来自设备，构造器存在、终端成功初始化，所有 HTTP 请求无外部 origin，pageerror 为零；设备返回的三个资源字节与源码完全一致。见 network.json、health-assets.json。
2. **J1 真事件、真 SSH**：设备正常电压约 20.5V，保护 NORMAL、触发次数0。通过实际 Web Terminal 执行 `voltprot --status` 后执行 `voltprot --debug`，进入已授权 SSH 主机。真实 `power_event/debug_tick` 在 SSH 中显示，活动阶段无新增 `tianshan>`，sshMode 保持 true。实际键盘输入 `echo TIANSHAN_J1_REAL_PASS` 产生 ssh_input，收到独立远端返回行；随后 Ctrl+\\ 产生 ssh_disconnect，收到 disconnecting→closed 后才回到本地。接着本地 echo 产生 terminal_input 并正常返回。离开页面后终端实例消失，重新进入后连接正常，vendor 请求累计仍仅三个。退出后设备 tasks 无 ssh_poll。
3. **调试结束边界**：本次浏览器操作和取证超过8–10秒提前关闭目标，使用原命令既有30秒自动关闭，没有在自动关闭后再次切换以免重新开启。双 WebSocket 共记录60个 debug_tick（两个连接各接收同一批30秒通知），后续本地命令、离开/重新进入期间计数保持60，未再增加。未触发低压、关机或阈值修改。这里验证安全 debug_tick 显示路径，并不声称真实低压关机链路经过测试。
4. **测试干扰与边界**：最初旧 Safari 与新 Chrome 同时停留终端页，收到明确 `session_closed: Another terminal session requested`；将旧 Safari 移至系统页后再执行 J1。此干扰与原 stack overflow 不同。当前补验没有重新接通串口监控，不能据此声称本段串口无错误；此前两版10轮串口/任务/水位证据仍见对应目录。当前段以浏览器消息、真实echo、设备状态和任务回收为证。

本轮上述实际消息/状态/网络断言通过。此结果补齐了先前列出的离线真机首载和J1真机通知两项，不能扩张为全系统无缺陷、任意负载无泄漏或直接读取真机 operation 账本。尚未提交、推送或合并。
