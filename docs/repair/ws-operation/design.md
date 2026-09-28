# Shell/Exec 统一生命周期：实施前设计底稿

基线：74b1b646ec4a45f9159bd028e622b13fca5eff75，fix/v0.5.2-reliability-and-prompts。
本轮开始无已跟踪文件修改；保留 output/、tmp/ 和既有未跟踪 diff-check.txt。
完整生产源码及原测试保存在 baseline.tar.gz，逐文件 SHA256 在 baseline.json。

## 实际依赖和旧写入者

HTTPD handler -> handle_ssh_connect -> SSH create/connect/open -> ssh_poll_task -> ssh_send_output。
HTTPD 输入/resize/signal/disconnect 与 poller 共享 shell/session。
API / 自动化 -> exec start/start_ex -> exec task -> ts_ssh_exec_stream -> output callback -> 输出、match、变量更新。
FreeRTOS timer -> timeout callback -> abort；cancel API -> abort。
两类输出 -> transport submit -> flush -> HTTPD work -> done（可能早于 submit 返回）。
stop -> pause telemetry -> HTTPD barrier -> producer cleanup -> event unregister_sync/log detach -> manager drain -> TX drain -> worker exit。

旧状态写入：connect/poller/cleanup/status/done 共同写 Shell 的 running/state/session/terminal；Exec wrappers/task/output done/terminal flush 共同写 running/params/terminal/result_active；timer 和 cancel 借用全局 session。
G1：done 减 pending 后仍操作全局 terminal；这时 poller 退出和 terminal settle 可以允许新 connect。
G2：output callback 已经复制/编码，而 pending 尚未登记；另一交付失败可先封口。

## 统一状态表

FREE -> STARTING（同锁创建门，创建引用）-> OPEN（资源就绪）-> CLOSING（原子关闭输出门）-> FINALIZING（prepare、output targets、close builder 全部归零，唯一认领）-> TERMINAL_PENDING -> TERMINAL_SETTLED -> RECLAIMING -> FREE。
STARTING 失败可直接 CLOSING；没有接受业务的创建失败直接丢弃预留并终结观察。
执行者责任与观察阶段正交：TERMINAL_SETTLED 仍可有 Exec executor；不能复用。

## 所有权及线性化点

固定 Shell/Exec 各一槽，稳定 portMUX 只保护状态、计数、身份、借用和投递账本。
创建/stop 共用门；acquire/current-get 在锁内取得原操作引用。新任务参数携带原操作及本代业务资源。
输出 begin 在 OPEN 检查的同一锁内登记 preparing 和引用；JSON/分配在锁外。
整批 target 在首个 submit 前登记；票据保留至全部提交返回；每个 token 独立结算，重复回调不得重复减少。
失败事实与 close 合并在同锁内；回调保活到其最后一次原上下文访问之后。
首个 close 持 builder 引用冻结接收者；终态 payload/reservation 在 finalizer 中唯一移动；随后回调只结算，不修改已封口 payload。
终态全部目标登记后逐个提交；finalizer 引用保留至所有 submit 返回。
executor、borrower、timer、prepare、delivery、finalizer 全部退出后锁内 RECLAIMING；锁外释放业务资源；最后锁内 FREE。
锁外禁止不持引用访问业务资源；不持 op 锁调用 TX、分配、JSON、SSH、RTOS 或等待。

## 迁移映射

s_ssh/s_exec_terminal -> slot reservation；pending -> preparing + unsettled；ready/claimed/result_active -> 单一 phase；generation -> 不可变 epoch；global params/session/shell -> 原操作绑定资源。
status/terminal helpers -> close builder + worker finalizer；done -> token settlement；task/create -> 独立 lease；timer -> 原 params + daemon 排空引用；cancel/input -> 原操作 borrow。
普通输出各只有一个任务生产者；Shell connecting/connected 在 HTTPD 中准备完毕后才开放 poller，disconnect 不再插入并行普通输出。
Exec started/output/match 均由同一 executor；观察关闭不跳过收集、匹配、变量和合法取消。

## 最小调整与不变范围

TX 增加向预留目标绑定固定 peer 的内部入口，保留类别、容量、连接校验和逐目标 exactly-once。
控制层由现有 subscription worker 推进；不新增任务。终态不承诺浏览器收到。
Shell input 等借用退出后才允许业务资源销毁；Exec timer 删除之后用同一 timer daemon 的 barrier 归还引用，不能把 xTimerStop 返回当作回调退出。
不改变 SSH 匹配规则、变量语义、硬件、全局事件和遥测策略。
