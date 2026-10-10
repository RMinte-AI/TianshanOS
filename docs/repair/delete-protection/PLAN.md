# 删除保护与快捷操作提示：已确认的实施方案 v4（第三轮方案 v2）

日期：2026-10-10。用户已明确回复“确认，按第二轮文案实施”，并批准第三轮方案 v2 及三句非 root 首页说明。

本版本采用原《修改方案与交接.md》v2，以及《评审意见-第二轮.md》的修订。本文件记录落地边界，配套 COPY.md 是实际语言包文案，RESULT.md 记录验证与剩余边界。

## 工作位置与范围

- 分支：fix/automation-delete-protection。
- 独立目录：/Users/massif/.codex/worktrees/automation-delete-protection/TianshanOS。
- 起点：main / 050757bb8046206797804ba5edc110ae3244bf17（0.6.1）。
- 不修改主目录，不合入另一 AI 的 SSH 指纹分支。
- 不提交、推送、合并、发布或刷机；不改变 SD/NVS 存储策略，不自动修复历史断裂引用。

## 固件合同

1. 删除模板在 ts_action_template_remove_checked 内先零等待取得规则 transaction，再取得 SSH binding；检查已加载且无需恢复，再遍历所有规则，包含停用规则，并去重收集 id/name。
2. 有引用拒绝删除，不能调用 template_remove_impl 或 API 的 unlink；列表细节分配失败仍拒绝删除。无法完成检查与已确认被引用是两种结果。
3. 拿不到 transaction 时通过 ts_rule_config_status 判断 recovery/loading/busy；未初始化按 loading，不能误报更新。
4. 老入口 ts_action_template_remove 委托同一实现，避免另一删除入口绕过保护。
5. 新建／新增／增加次数的模板引用，在 ts_rule_commit 的 transaction + binding 内、持久化之前验证存在。缺失返回 action_missing 和可靠 ID；内部/内存错误不说“已不存在”。
6. 既有缺失 ID 及原有出现次数可以保留，历史展示编辑、启停维持原行为；不扩大引用，原执行保护不放宽。
7. 缺失引用拒绝时清除 committing，释放全部锁和负载；SD/NVS 不写，revision 不增长，后续触发/保存/启停不被锁死。
8. 490 行缺失模板时的原服务保护保留；不会把缺失配置当成远程服务停止。
9. API 业务失败仍返回 ESP_OK + 非零 result.code，让当前 main 的真实 HTTP 成功包络保留 message/data；不依赖 SSH 分支的新通用出口。

## 前端与文案

- 第二轮第 6 节的 unconfirmed/checkFailed/actionMissing/loadingDetails 与无列表英文已采用；C 组仅有“关闭”。
- 模板名来自用户实际选择的 option.data-template-name，取不到用 ID；后端不编造名称。保存失败保留草稿。响应详情丢失时使用既有 saveFailed，不显示空名称或原始 action_missing。
- runtimeSaveError 保留 action_missing 的名称提示，将本轮新增原因 template_lookup_failed 映射到现有 saveFailed；不扩展其他原因。新文案在 deleteProtection 命名空间。
- 查看规则以及从模板阻断框查看指令只改变路由，不定位、不高亮、不自动编辑或删除；指令自身被阻断时只有关闭，不重载当前页。
- 首页 recovery > loading > 空状态，保留面板；root 原说明及前往自动化入口不变，所有非 root 无按钮，按状态使用 quickLoadingNeedRoot/quickRecoveryNeedRoot/quickEmptyNeedRoot 三句用户确认说明。不泛化提示用户检查 SD 卡。
- 使用现有 sheet/confirmSheet、系统字体及语义色。仅增加确认框长标题换行，并约束引用列表文字列的宽度。
- 本次“处理中”提示在阻断框出现时立即隐藏；不清除其他失败/警告。关闭后释放列表 DOM 的导航回调引用，离开页面关闭旧框。
- app.js 查询标识及语言包使用的 TS_ASSET_VERSION 为 delete2；api.js/CSS 保持 delete1。xterm 地址随 TS_ASSET_VERSION 变化，内容未改；发布版本仍为 0.6.1。

## 服务状态完整映射

先判定未完成操作：busy=true，或 operation_phase 是 queued/executing。若 operation_kind 是 start/stop/verify，分别显示 starting/stopping/verifying；操作类型未知显示 operationPending。终态 receipt 不当成仍在操作。

在没有未完成操作时：

| state | 文案键/处理 |
|---|---|
| running / ready / checking | running：仍在运行 |
| starting | starting：正在启动 |
| stopping | stopping：正在停止 |
| unknown / failed / timeout | unconfirmed：尚未确认已停止 |
| stopped | 允许进入最终删除检查 |
| 未来未知或缺失状态 | unconfirmed，保持拒绝 |

预检后又被后端拒绝，使用已有 serviceDeleteProtected，不用旧状态猜新原因。原有核验失败、超时、网络错误反馈保留；不自动停止、不增轮询。

## 验证增补

- H13：缺失模板拒绝后 committing=false，随后有效保存、启停和手动触发仍可用。
- H14：事务被加载占用或未初始化时显示 loading，更新占用时显示 busy。
- H15：9 state × 2 busy × 4 kind × 3 phase × 2 language，共 432 个映射分支。
- 两种保存/删除顺序使用持久化屏障实际并发验证；不只按顺序调用替身算法。
- 规则/模板名称上限47字节、ID63字节；中文/引号/HTML符号；32条引用去重；最大标题手机/桌面不裁切。
- 同一对象先被服务阻止，确认停止后的下一次删除再被引用阻止，两次说明各自准确，没有旧框残留。
- 生产函数产生的 HTTP 字节原样回放实际 api.js 和按钮，不替换 api.call。

上述验证证据见 RESULT.md；不将主机或浏览器结果当作真实热拔插、断电或远程服务验收。
