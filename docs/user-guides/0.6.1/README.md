# TianshanOS 0.6.1 用户指南源文档

修订日期：2026-10-09。核对基准：版本 `0.6.1`，提交 `050757bb`。

本目录是可继续维护的源文档。日常章节只维护一次，生成时用于 admin 与 root 两份双语指南；root 的登录和导航按其权限调整。原有文档的安全管理范围保持为引用单独安全指南。

| 文件 | 内容 |
| --- | --- |
| shared.zh.md / shared.en.md | 两个账户共用的日常功能，中英文对应 |
| root.zh.md / root.en.md | root 专属终端、指令、自动化，中英文对应 |
| ../../../tools/build_061_guides.py | 合并源文档、生成 PDF 与验证报告 |

在仓库根目录运行 `tools/build_061_guides.py`，依赖 ReportLab、pypdf、pypdfium2 和 Pillow，以及 macOS 中文、Arial、Courier 字体。输出在 `output/pdf/0.6.1/`，逐页图片及验证报告在 `tmp/pdfs/guides-061/`。输出目录也包含合并后的 admin.md、root.md，便于单独阅读或复用。

`tools/prepare_061_guides.py` 仅用于本次从上一版内容导入并初步修订，不是日常构建入口；新源文档生成后直接编辑本目录文件。初始内容来自旧指南的可编辑副本，桌面旧 PDF 的 SHA-256 与上次交付一致。

## 内容核对记录

以下是源码及界面定义核对，不代表设备实测。发布说明仅用来确定变更范围，具体用户说明以当前调用入口、界面实际使用的语言键及后端行为为准。语言包中残留但未用于当前界面的旧名称没有作为操作名称。

| 章节 | 本轮核对重点 | 主要证据 |
| --- | --- | --- |
| 1 开始使用 | 登录、改默认密码、语言、账户权限、图标提示 | web/index.html；web/js/app.js 登录流程；web/js/router.js |
| 2 系统状态 | 管理芯片资源范围、服务列表实际字段、电源与关机设置名称 | web/js/app.js loadSystemPage、updateServiceList、showShutdownSettingsModal |
| 3 设备面板 | AGX 电源、LPMU 网络可达性、快捷服务核验与停止、数据组件 | web/js/app.js updateLpmuPowerButton、triggerQuickAction、requestServiceControl、saveWidgetEdit |
| 4 风扇 | 智能/Smart、TTI、实际下发输出与草稿、未知输出、共享温度源、保存曲线 | web/js/app.js updateFanInfo、showFanCurveModal、applyFanCurve；ts_drivers/src/ts_fan.c；ts_api/src/ts_api_fan.c |
| 5 LED | 卡片图标、统一设置窗口分组、播放/停止/保存、操作反馈 | web/js/app.js generateLedDeviceCard、openLedModal、generateLedModalContent |
| 6 网络 | WiFi、DHCP、NAT、LPMU 每次输入 sudo 密码、确认互联网与配置、执行日志 | web/js/app.js loadNetworkPage、startNetworkLpmuAccess、showNetworkLpmuAccessResult；ts_api/src/ts_api_lpmu_access.c |
| 7 文件 | 存储切换、文件操作、批量结果、配置包上传不等于应用 | web/js/app.js loadFilesPage 及文件处理；web/js/lang/* promptRepair |
| 8 OTA | 同时升级 www、跳过证书校验、SD 同目录 www.bin、分区与回滚 | web/js/app.js loadOtaPage、resolveSdcardOtaPaths、otaFromFile；ts_api/src/ts_api_ota.c；docs/releases/v0.6.1.md |
| 9 安全入口与异常处理 | 保留独立安全指南范围、超时与结果未确认的用户处理 | web/js/router.js；web/js/app.js loadSecurityPage；web/js/lang/* promptRepair |
| 10 终端 | root 权限、help/ssh --help、SSH Shell 参数、Ctrl+\、日志筛选 | web/js/terminal.js；web/js/app.js loadTerminalPage；ts_console/commands/ts_cmd_ssh.c |
| 11 指令 | 只读 ID、当前输出匹配字段、秒/毫秒单位、服务状态与删除保护 | web/js/app.js loadCommandsPage、saveCommand、requestServiceControl；ts_api/src/ts_api_ssh.c |
| 12 自动化 | 变量入口、面板/手动权限独立设置、禁用与执行中拦截、动作条件/重复、停止与远程进程区别、保存失败 | web/js/app.js loadAutomationPage、showSourceVariables、showAddRuleModal、submitAddRule；ts_automation/src/ts_rule_engine.c、ts_automation.c、ts_action_manager.c |

表中 web 路径相对于 `components/ts_webui/`，其他组件路径相对于 `components/`。中文和英文所有明确引用的当前操作名称分别核对语言包与实际使用位置。

## 交付验收

- 两份 PDF 都包含中文与英文，admin 共用功能与 root 对应章节来自同一源文件。
- 封面明示编写依据版本 0.6.1、修订日期及后续版本适用提示。
- 每个源段落和表格单元格在 PDF 提取文本中可找到；无空白页，所有书签页码有效。
- 每页渲染供检查；最终检查记录和文件 SHA-256 位于生成目录的验证报告。
- 保留上一版桌面 PDF 备份，再交付新文件，并核对交付文件哈希。
