# 指令／自动化存储与 WebUI 两次改动：Reviewer 复核

日期：2026-10-28。状态：五处已确认问题已修正，本轮代码审查与本地验证完成。

审查基线为 `002d3a7b8ddb75d5a43c0046382d1335f49fbfc0`，本轮开始已核对与上游 main 一致。审查对象包括现有未提交的自动化／指令改造及后续 WebUI 校正，未覆盖其他工作目录或重置既有改动。

## 发现与修正

以下五项均有修改前失败证据和修改后通过证据。四个前端问题还使用审查前保存的原脚本、校正后的真实协议夹具回放，四项全部失败；因此不是只对照新实现编写能通过的断言。

### 1. 删除窗口在配置变化后无法取得新的确认上下文（P2）

列表读到版本 A 后，另一处配置修改使版本变为 B。后端正确拒绝 A，但窗口仍保存 A，用户再次点击普通或强制删除仍提交 A。预览阶段和提交阶段都可能遇到此情况。

修正保留后端版本保护：窗口收到 `configuration_changed` 后停止使用旧确认，显示“重新检查删除影响”。用户点击后重新读取当前列表和影响清单，重新打开确认窗口；删除仍需再次确认。读取不自动提交删除，也不沿用原替换选择或暂停确认。

[代码：删除确认上下文](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/js/app.js:264)

验证包含中英文、预览前变化和预览后变化，以及“重新检查不会产生删除请求”。原先“预览未结束或预览失败时仍可进入强删”的行为继续通过。

### 2. 数据源运行实例号与配置读取视图不一致（P2）

运行表分配了实例号，配置读取视图中的同一数据源却仍是 0。列表 API 用两者相等来确认状态归属，因此真实运行表即便已经连接，API 仍返回未连接、更新时间 0。

修正在发布前准备实例身份，并写入尚未发布的配置视图；运行表使用同一身份。未改来源保留身份，修改／切换后的新来源取得新身份。发布后不再修改已经对外可读的定义，也不靠删除实例检查来掩盖问题。

[代码：数据源准备与发布](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_automation/src/ts_source_runtime.c:41)

原生测试修改前稳定复现 `runtime.connected=true`、API 却返回 false；修正后连接状态与更新时间可读。既有保留连接、换来源重置、旧回调隔离用例同时通过。文件格式、保存位置及备份内容未改变，实例号仍不持久化。

### 3. 合法 ID 含引号时，主机、命令和密钥按钮的事件拼接错误（P2）

HTML 转义不能替代 JavaScript 参数编码。主机 ID 如 `lab'host` 能由配置接口保存，但点击主机卡片时会产生无效 JavaScript；导出按钮也存在同类拼接。

修正统一使用已有的 `configurationHandler`：参数先按 JSON 编码，再按 HTML 属性转义。没有新增一套 ID 限制或改变保存格式。

[代码：主机选择](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/js/app.js:7962)

验证实际点击带引号的主机卡片、命令导出、主机导出与密钥导出，确认收到完整原始 ID，页面没有脚本错误。

### 4. 命令图标字符串被直接解释为 HTML（P2）

非图片、非 `ri-` 的图标值原样插入 innerHTML，可产生 HTML 节点和事件属性；SVG 引用中的字符串也未作属性转义。

修正将普通图标值作为文字显示，SVG 引用按属性转义。正常 emoji、现有 SVG 图标和 SD 图片分支保持原用途。

[代码：命令图标渲染](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/js/app.js:8056)

验证普通图标字符串不能生成 img 节点或执行事件，带引号的 SVG 引用不能生成额外属性。

### 5. 首页使用旧恢复字段，读取失败仍显示“正在加载”（P2）

当前接口使用 `loaded` 和 `load_error`。首页仍读取旧 `recovery_required`，并将所有 `loaded=false` 都显示为加载中，真实错误因此被掩盖。

修正按实际接口区分暂未完成加载与明确读取失败；真实失败使用现有中英文错误解释，并保留管理入口及管理员提示。空配置继续显示空状态。通知占满卡片宽度，避免长句挤在半列中。

[代码：首页快捷操作](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/js/app.js:4229)

验证空、加载中、读取失败三种状态，以及中英文、root/admin 两种界面。没有新增探测、重试或自动修复流程。

## 已完成的清理

- 移除未被生产代码使用的 `mirror_synced` 结果字段。
- 移除重复的 `ts_action_template_remove_checked` 接口及专用结果类型；生产调用已使用统一删除入口，其引用检查、影响清单与强删测试保留。
- 移除“删除前必须核验远端状态”、逐卡片 SSH 检查等已经与当前实现相反的注释，以及不再使用的首页恢复说明。
- 校正删除夹具：预览与提交都先检查 active_version，与真实 API 的 edit_begin 顺序一致。
- 将已有布局检查加入现有 `test:browser`，使现有 CI 入口实际执行这些检查；没有另建检查平台或重复的 CI 阶段。

## 关键合同复核

| 范围 | 本轮复核结论 |
|---|---|
| 分区与密钥 | 保留现有密钥区；系统 NVS 和自动化文件区独立。迁移完成后才清旧非密钥数据，未新增擦除密钥路径。 |
| 保存与发布 | 统一所有者管理写入；可失败的运行视图准备在落盘前，发布不再分配内存。文件提交结果与清理结果分开报告。 |
| 工作来源 | 有卡日常编辑写 SD；手动副本与系统设置分开。应急编辑、持久化来源选择、插卡后明确选择的目标不变。 |
| 引用与强删 | 目标、关联清理和规则停用通过同一次配置提交；远端 unknown/running 不再是删除定义的前提。权限、版本和真实写入结果仍有效。 |
| 执行归属 | 排队、延迟、重复及晚回调按实例归属处理；已开始的操作持有固定内容。独立服务记录保留原主机与进程定位。 |
| WebUI | 窗口归属、失效表单、列宽、响应式、材质、字体和危险确认沿用上一轮校正；本轮只调整已发现的问题。 |

## 验证结果

- 原生配置／存储套件通过，使用 AddressSanitizer 与 UndefinedBehaviorSanitizer，覆盖分区、迁移、实际文件替换、引用、执行生命周期、API 和来源模式。
- 完整网页入口 704 项通过，包含已经接入的 4 组布局检查；静态检查 130 项通过。
- 截图排版收尾后，仅对受影响的 5 项用例重新检查；源码和实际打包脚本各 5 项通过。
- 压缩工具 3 项检查、打包资源一致性 1 项检查通过。
- 完整固件与网页镜像构建成功，应用分区仍有约 30% 空间；未执行烧录。
- JavaScript 语法、关键 CSS 同步、git diff --check 通过。

日志保存在 [/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/reviewer](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/reviewer)。此前通过且本轮未改变的物理拔插、远端服务和证书检查未重复执行；远程 CI 尚未触发，本轮只接入其入口并本地验证。

## 本地模拟截图

以下截图用于检查错误说明和重新确认入口，不代表实机状态。

![配置变化后的重新检查入口](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/reviewer/delete-recheck-en-US.png)

![真实读取失败的中文说明](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/reviewer/unavailable-zh-CN.png)

## 交付边界

既定 R01–R18 产品目标保持不变。未新增自动重试、自动补配置、多级兜底或额外保护锁。

本轮未提交、推送、发布或刷机。既有测试证书不具备配置签名用途的已接受限制仍保留，未放宽验签。此前实机记录对应当时的候选固件；本轮源码修正尚未在设备上运行。
