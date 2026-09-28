# v3 项目影响及端到端契约（先于实施建立）

本轮基线/PR head 568b871；PR base d6ed947，main。只本地修改测试编译，现有三项未跟踪目录/文件保留。累计 PR 差异见 cumulative-files.txt，不把目标分支旧缺陷当本轮反例。

## 功能与依赖覆盖

| 链 | 真实入口和消费者 | 本轮覆盖与理由 |
|---|---|---|
| Shell/普通终端 | router /terminal -> WebTerminal -> /ws ws_handler -> SSH control/terminal handler -> operation/TX -> terminal.js handleMessage/handleSshStatus -> 下一次 onData/exit/restore | 深入。请求 error 与 ssh_status:error 的副作用冲突，模式、提示符、输入和恢复需双向验证 |
| Exec/自动化 | /commands、/automation -> ssh.exec_stream/cancel -> executor -> ts_ssh_client -> match/variables -> app.js handleSshExecMessage；rule/action manager、service/log watch | 保留本轮前状态/取消/匹配；控制与生命周期全套，runtime 检查真正 driver 的取消/总deadline |
| Shell 下层/CLI | ts_webui_ws 与 ts_cmd_ssh -> ts_ssh_shell open/read/write/run/poll/close -> ts_ssh_client disconnect/destroy -> libssh2 | 深入。0 字节不是 EOF、非 RUNNING close 泄漏、EAGAIN 无限等待直接妨碍错误分类与 stop。组合测试不得 stub close/free |
| 遥测、日志、保护 | dashboard/widgets -> api.js subscriptions -> manager -> TX；power event 全局广播、terminal 角色/log level | 相邻共享 TX/角色回归，保护配额、公平性、旧日志代次不改变 |
| 停止销毁 | ts_webui/service/HTTP/HTTPS -> ws_stop -> creators/producers/event/log/worker/timer/TX drain | 深入相关 shell cleanup；R1/R4 及证书生命周期套件覆盖共享依赖 |
| 保存/证书/文件 | router 配置、security/pki/files -> api.js request/requireApiSuccess -> API/service -> app.js | 共享 API 帮助函数不修改；运行时、证书和全部提示 Node/Chrome 回归 |
| 其余硬件/网络/LED/风扇/电源页面 | router.register、app loader、ts_api 注册及 main/ts_services.c 服务注册 | 本轮不改硬件/API策略，保留 host/浏览器冒烟；不宣称硬件验证 |
| 发布物 | ts_webui CMake WEB_ALL_FILES -> web_optimized/minify/gzip -> www.bin；HTTP 静态 no-cache index + immutable JS | cache key 配套更新，核对原始/压缩/产物；版本/工作流不变，release 测试保留 |

## 消息与用户行为

| 事实 | 生产条件/对象 | 兼容消息/接收者 | 前端状态与下一动作 | 资源 |
|---|---|---|---|---|
| 单次请求拒绝/可恢复控制错误 | 非 owner、参数、unsupported、明确可恢复下层错误；实际请求 peer | 既有 type:error,message，只给请求者；不借 ssh_status:error 伪造终态 | SSH active 保持 SSH route，不写本地prompt、不恢复、不重发；connecting 无 active 时结束当前失败尝试；普通终端保留原恢复契约 | 请求 borrower 退出，不关闭有效 op |
| 新启动拒绝 | 参数、另一个 op、容量、创建资源失败 | 请求级 error；已有 active 不变，真正 waiting attempt 返回 local | 只清本次 connecting；可手动重试；不抢别人会话 | 未接受 op discard 或不创建 |
| 创建后真实启动失败 | 原 op connect/shell open/readiness failure | 原 op ssh_status:error（唯一终态） | 清对应 SSH，恢复本地终端路由；无自动命令重放 | v2 close/settle/cleanup |
| 正常退出/真实 EOF | 原 poller/服务 owner | 原 op disconnecting 非终态、closed 终态 | disconnecting 仍等待关闭，不提前写本地prompt；closed 可回到本地 | 票据/投递先结算，真实下层对象释放 |
| 致命 write/read/resize 或不确定写入 | 原 driver state ERROR/CLOSED，部分写或 deadline 不能安全重放 | 原 op ssh_status:error，说明远端结果不确定 | 退出该观察/会话，下一步手动检查/重连；不重发输入 | 有界下层 close，保持原 v2 资源协议 |
| 输出观察失败 | TX done -> 原 op failed | v2 fallback ssh_status:error / ssh_exec_error | Shell 退出观察；Exec 业务不自动取消 | 原引用/终态不改 |
| Exec 取消/业务结果 | 现有 exact ID/first cause/business end | 原 cancel/done/error | app.js 既有身份匹配、不确定提示 | 原 executor/timer/结果排空不变 |
| stopping / 普通终端错误 | 原 WS admission / terminal handler | 既有 error | active SSH 错误不切 local；普通 terminal 的 Not a terminal session 恢复保留，仅 local 模式可触发 | 不重开新业务，不递归提示 |

公共 ssh_request_error 的调用点分为 connect 参数/准入/分配和四控制入口；不会全局改终态 helper ssh_close、startup statuses 或 operation fallback。通用 error 消费点为 terminal.js；真实状态是路由依据，不新增按英文/中文文本识别 SSH 生命周期。已有 Not a terminal session 兼容恢复只保留普通终端上下文。app.js 的 Exec 消费者不复用该 Shell 路由。

前端所有 socket 回调、重连/恢复 timer 和销毁绑定当前 socket/实例；旧 socket onclose/onmessage 不能污染新连接。WS 关闭清掉 SSH mode，重新连接只启动本地 terminal，不重放远端 connect/command。

协议类型字段保持：error 本就是请求错误，ssh_status:error 为已接受操作的终态；本轮更新消费者副作用以匹配这一区分。旧固件会把请求错误发为终态，新页面不能安全猜测旧帧；支持同一构建固件+www、刷新并重连，不声称混合版本全兼容。保留同连接 Shell 缺操作ID边界。

## 反向审查补充的同链依赖

CLI `shell_input_callback` 的 Ctrl+\ 仅登记 console interrupt，原 run 不观察；纠正零字节 EOF 后必须同时兑现用户退出。采用现有驱动状态上的 `ts_ssh_shell_request_close`，仅改变本地循环，不释放句柄、不代表远端已停止。CLI 原 owner 继续 close/disconnect/destroy，WebUI 继续原 op 关闭入口。真实 CLI 回调编译组合测试与独立 baseline-cli-exit 反例覆盖。
