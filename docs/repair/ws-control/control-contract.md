# 控制请求契约（实施前与迁移决定）

基线 1591ecc；本轮仅本地修改、测试、编译。保留其他未跟踪文件。

| 请求/来源 | 原入口与缺陷 | 统一目标、接受点及执行者 | 结果与引用释放 |
|---|---|---|---|
| Shell 输入 / WS ssh_input | wildcard current Shell | 从 req 的完整 peer，在 op 锁内匹配原 peer + OPEN 并取引用；资源 mutex 下写原 shell | 不匹配向请求者报既有 error；执行完释放；不改投 |
| Shell 断开 / WS ssh_disconnect | wildcard + ctx atomic | 同一目标获取；原 ctx 保存 disconnect 意图，原 poller 顺序发送 disconnecting 并关闭 | 接受非关闭确认；poller 保活；请求释放借用 |
| Shell 信号 / WS ssh_signal | wildcard current Shell | 同一目标获取；原 shell 资源 mutex 下发送 | 同输入 |
| Shell resize / WS ssh_resize | wildcard current Shell | 同一目标获取；原 shell 资源 mutex 下调整 | 同输入 |
| Exec 取消 / ssh.cancel API | id acquire + session 存在才 abort | exact 非零操作 ID、已接受业务且未 ENDED，在 op 锁内保存 USER 意图并取原引用；底层 config.cancelled 持续读取原 op | 既有 cancelled:true 仅表示请求接受；结束后 INVALID_STATE；调用退出释放 |
| 业务超时 / 原 timer 或驱动 deadline | 与匹配共享 cancel_requested | 原 timer 上下文保活，登记 TIMEOUT；驱动返回时在业务结束决定点记录无其他原因的 deadline | timeout；timer 原有 daemon 排空后释放 |
| 匹配停止 / 原 executor 输出回调 | 与超时共用标记 | 原上下文登记 MATCH；复用协作停止，不改变匹配规则 | match_success/failed；不覆盖较早 USER 原因；同步回调释放 |
| 服务停止 / service owner | current Shell / busy Exec | 独立 owner Shell acquire；HTTPD barrier 后关闭原 Shell；Exec 只等待或 busy，不登记业务取消 | busy/timeout 保留结果通道及依赖；借用释放，执行者自行退出 |

观察失败只封口网页观察，不登记任何业务停止原因。业务阶段与 OP_OPEN/CLOSING 等观察阶段正交：PREPARING -> SUBMIT_CLAIMED -> ENDED。取消和 claim 在 s_op_lock 内排序；claim 前取消保证不再调用命令执行入口，claim 后只能协作停止，不能保证远端未执行或已经终止。配置 cancel_context 为原 op，由 executor/timer/借用引用保活，session 清理完成后才能回收。

保留所有原因位和第一停止原因；结果以已接受的第一原因解释。业务结束决定点锁内封存，之后的 cancel 被拒绝，不能更改终态。准备/密钥加载期间取消不可打断同步存储读取，但读取后不再获得提交资格。底层 DNS 等不可协作阻塞边界仍需说明。

页面只通过统一受控目标获取，状态查询 wildcard 不用于页面副作用。完整 peer 包括 server、server generation、connection generation、fd；取得引用后不再查询 current。现有 Shell 帧没有操作 ID，同一连接上先后两代 Shell 的迟到消息仍无法区分，本轮不扩协议。Exec ID 沿用原 API 权限范围，不擅自添加页面绑定或改变公开协议。

## 实施后核对

页面路径统一为 ws_handler -> handle_ssh_control -> shell_for_peer(ref++) -> 原资源 mutex / disconnect 意图 -> release。资源已结束或 I/O 失败时用原 ssh_status:error 报告，不重新选择目标。peer_closed 与客户端登记清理均传递完整 peer；修复旧 fd 关闭回调可能关闭替换连接的同类路径。

Exec API 直接执行 exact-ID 取消登记，去掉 is_running 预检查，并保留完整 uint32 ID。业务结束锁内封存，不等到投递结束才拒绝取消。MATCH 明确原因还修正了首次提取停止被旧共享标记误分类为超时；匹配触发规则保持原样。非用户原因时，原已匹配事实对 deadline 返回值的结果解释仍保留，详见 README。

原状态机的输出准入/两阶段终态/引用回收没有迁移到另一套机制。新增控制状态同样归 s_op_lock；资源 I/O 和 JSON 不进入该锁。没有引入 op-lock -> 资源 mutex 的嵌套；资源 mutex 内读取 op 状态时只取得短时 op 锁。
