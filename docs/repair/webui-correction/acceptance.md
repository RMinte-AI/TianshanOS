# TianShanOS WebUI 集中校正验收记录

后续 Reviewer 复核：[两次改动审查记录](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/docs/repair/reviewer-audit/2026-10-28-two-changes.md)。本页实机／浏览器记录对应当时版本；后续修正与验证边界见该记录。

验收状态：完成。最终 697 项既有浏览器回归、130 项静态检查、源码与打包资源各 4 组布局检查、1 项打包一致性检查全部通过。

本轮仅校正实现与视觉，保留已经确定的自动化、NVS/SD、删除保护、最高级强制删除、应急编辑及插卡来源选择行为。所有接口检查使用本地模拟；没有运行设备业务命令、重启、刷机或改动实机数据。

工作目录：`/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS`。上游 `origin/main` 与当前基线均为 `002d3a7b8ddb75d5a43c0046382d1335f49fbfc0`，本轮结束前已再次核对。原有未提交工作保留；本轮未提交、推送或发布。

依据设计系统入口及 Audit 第十六、十七、十八节、NativeDialogs、Performance、tokens 和页面/弹窗/响应式目标稿；图标额外核对了后续 Phosphor 生成器和对应表，避免把早期迁移方案当成当前定稿。

## 18 项逐项结果

| 项 | 复现判断 | 修改前证据 | 实现调整 | 验收结果 |
|---|---|---|---|---|
|1|成立|命令窗口位于主内容层内；桌面标题命中的是顶栏。|命令、网络内嵌窗口挂到 body，随页面移除；配置编辑器切页清理。LED 与变量窗口切页关闭。|标题命中测试、初始焦点、Esc、离开页面后的窗口清理均通过。|
|2|成立|手机数据源表头约 508px、数据行约 725px，列起点不同。|表头与各行使用同一组 Grid/subgrid 列；每列至少容纳内容；滚动放在整表。覆盖自动化、命令、文件、变量、密钥、主机、指纹、配置包、WiFi、AP 和 DHCP 列表。|多行长名称、表头/数据坐标差 <0.1px；横滚后仍对齐。另检查操作按钮不得侵入相邻列。|
|3|成立|过宽的手机响应式选择器把 12 个图标改成 12 行。|删除按内联 style 文本匹配的全局规则；图标网格保持六列两排；内存双栏采用明确的组件响应式规则。|中英文及八个宽度断点均保持 6×2。|
|4|成立|网络卡片 SVG 相对容器中心向上 2.5px。|卡片、命令和警告图标容器使用 Flex 居中，消除文字基线空间。|网络、命令与警告容器的中心偏差 <0.1px；按钮与图文组合截图已查看。|
|5|观察成立，缺陷判断调整|播放边界中心右偏约 1.499px；下载/删除上偏 0.5px；刷新右偏 0.5px。|保留 Phosphor Regular 的原始图形。逐个查看相邻图标后，没有把边界不对称当作容器偏移，也没有统一平移或改 CSS 线宽。|当前图标是 256 栅格的填充轮廓路径。补充 Icons.md，区分早期 107 个/1.35 描边方案与后续 82 个符号的 Phosphor/自绘定稿。|
|6|成立|英文动作的命令行、结果变量、ID 换行后下方空隙为 0px。|手机分组行统一增加上下 10px 内边距，同时覆盖 .rows 中的行；保留原有圆角层级。|这些控件的下方间距断言通过；开关、禁用字段和滚动状态已查看。|
|7|成立|LED X 输入 x≈149、Y 输入 x≈277，提示占据 X 控制区。|自动位置说明移至标签下方，数值控制区保持一致。|禁用及可编辑两种状态均对齐；可编辑 X/Y 实测 x=277、宽=80。|
|8|成立|390px 中文下，用户菜单右边约 386，顶栏右边 378。|重新分配品牌与用户控件的手机空间，保留状态、语言和登录入口。|中英文、390/600/601/819/820/1200/1201/1440px 均保留至少 10px 的右侧内边距。|
|9|成立|英文动作类型第一排 60px，第二排 40px。|同组选择卡片共享行高并撑满网格单元。|各尺寸两排高度一致；没有缩小字号或截断名称。|
|10|成立|Command Variable 及 LED 长标签被固定高度挤压。|手机四个及以上选项分为两列，允许完整标签，统一行高和内边距；选中状态保留。|数据源分段实测每项 175×36px；LED 五项完整显示。|
|11|成立|创建后启用开关位于操作区，手机反向按钮排列把它排到最下面。|将开关放到数据源基本配置分组，操作区只保留提交/取消。|页面断言及中英文截图确认开关位于表单中，保存参数未改。|
|12|成立|命令、数据源普通 ID/名称字段混用旧的上标签布局。|普通字段统一分组行；命令正文和请求 URL 保持整宽。|新建/编辑、只读 ID、字段校验及保存流程回归通过。|
|13|成立|Developer 和 fixture.invalid 在手机上被任意拆词。|手机四列键值布局改为一列；普通短值不再 anywhere 换行，指纹等长值保留适当换行。|短值单行断言通过，未用小字号或省略号遮掩。|
|14|成立|自动化 Stop 及 AGX 正常关闭按钮使用红色图标。|正常停止/关闭改为中性表达，保留真实错误、警告及破坏性确认的颜色。|系统正常关闭与自动化停止截图已核对；强制删除交互回归通过。|
|15|成立|主内容面 ::before 使用 blur(32px)。|移除主内容背景模糊，保留半透明底色；顶栏和浮层继续使用现有材质。|实际计算的主内容 backdrop-filter 为 none；内联首屏样式与外部样式同步。|
|16|成立|页脚加载 Bold，计算字重 700。|从仓库注明自购授权的 Regular TTF 生成页脚所需 WOFF2 子集；删除被替代的 Bold 网页资源。|计算字重 400，Regular 已加载；TianshanOS · RMinte® AI 所需字符均存在。未下载新字体、未扩大品牌字体使用范围。|
|17|成立|保存/恢复与检查卡/停用自动化紧挨排列，缺少用途分组。|增加应急副本/配置选择分组，卡与自动化操作另成一组；说明跟随所属操作。|四个操作均保留；原有预览、确认、备份和来源切换测试通过。|
|18|成立|编辑器常驻配置版本说明；后台运行介绍重复解释机制。|编辑器只在实际读取失败或配置变化时插入说明；简化后台介绍，保留未知状态原因、原主机核验与删除不停止服务的限制。|真实配置变化提示仍可见，原输入保留且保存被阻止；中英文说明与错误流程回归通过。|

## 共用根因与清理

- 弹窗从页面隔离层移出，生命周期归属页面；没有抬高局部按钮或标题的 z-index。
- 表格由整表管理列宽，行使用 subgrid；长名称和操作区按内容获得空间，手机在表格内滚动。删除旧的逐行 max-content 和重复滚动规则。
- 删除影响所有内联网格的手机规则；保留图标、选项和表单各自的布局语义。
- 表单换行、分段多选项、卡片等高和图标基线在共用组件处理；没有逐图标位移或新增延时补丁。
- 删除已不用的 editingHint 文案及旧网页 Bold 字体；真实错误按需插入，没有隐藏限制和失败原因。
- 相邻断点核对还修正了 601px 英文状态摘要溢出，以及手机 OTA URL 输入框被按钮挤窄；后者采用整宽地址字段。

## 行为与构建验证

- 既有浏览器回归：697 项。静态/VM 检查：130 项。全部通过。
- 新布局回归：源码 4 组、实际 ESP-IDF 打包资源 4 组；含中英文、八个宽度断点、八个页面、长名称、横向滚动、禁用/可编辑、焦点、Esc、切页清理和真实配置变化提示。
- 打包一致性：1 项；检查内容版本、加载 URL 和配置/后台运行控制。
- 修改前后 app.js 中 243 个 API 调用表达式及参数的 AST 比较一致；API 客户端和路由器未修改。此结果与交互回归一起用于约束产品行为，截图本身不证明行为正确。
- ESP-IDF 的 optimize_web 和 spiffs_www_bin 已生成压缩资源与 www 镜像；未构建/发布新的客户串口包，也未烧录设备。
- 检查 JavaScript 语法、git diff --check 和首屏关键 CSS 同步。

复查入口：

```sh
npm --prefix tests/prompts test
npm --prefix tests/prompts run test:browser
npm --prefix tests/prompts run test:layout
TS_UI_LAYOUT_WEB=/private/tmp/tianshan-automation-build/esp-idf/ts_webui/web_optimized node --test tests/macos27/ui-correction.browser.cjs
TS_PACKAGED_WEB=/private/tmp/tianshan-automation-build/esp-idf/ts_webui/web_optimized npm --prefix tests/prompts run test:packaged
python3 tests/macos27-r2/critical.py --check
```

## 修改前后截图

以下配对来自独立的修改前资源副本与修改后资源，使用相同的本地模拟数据。手机配对为 390×900；桌面命令为 1440×1000。自动回归另包含 390×1000、1440×1000 及长名称多记录状态。截图已实际打开查看。

| 场景 | 修改前 | 修改后 |
|---|---|---|
|命令窗口·手机|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-command-zh-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-command-zh-390.jpg)|
|命令窗口·桌面|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-command-zh-1440.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-command-1440-zh-CN.png)|
|自动化表格|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-automation-zh-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-automation-zh-390.jpg)|
|英文动作表单|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-action-en-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-action-en-390.jpg)|
|英文数据源|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-source-en-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-source-en-390.jpg)|
|LED 数字参数|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-led-zh-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-led-zh-390.jpg)|
|英文安全页|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-security-en-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-security-en-390.jpg)|
|配置与备份|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-config-en-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-config-en-390.jpg)|
|后台运行说明|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/before-services-en-390.jpg)|[查看](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-services-en-390.jpg)|

图标边界测量：[icon-bounds.json](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/icon-bounds.json)。LED 可编辑状态：[截图](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/after-led-editable-zh-390.jpg)。

## 主要代码入口

- [页面弹窗、分组表单、配置说明与表格渲染](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/js/app.js:81)
- [共享列宽、居中与手机响应式](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/components/ts_webui/web/css/style.css:538)
- [图标当前定稿说明](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/docs/macos27/design-system/Icons.md)
- [布局验收脚本](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/tests/macos27/ui-correction.browser.cjs)
- [合成接口数据](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/tests/macos27/ui-correction-profile.json)

## 验收边界

本轮要求中的实现调整和本地 UI 验收均已覆盖。没有把本地模拟结果当作新一轮实机验收；测试机目前没有应用本轮网页更新。真实设备加载新版 WebUI 后的验证尚未执行，继续遵守既有设备操作边界。

机器可读记录及本轮日志：[acceptance.json](/Users/massif/.codex/worktrees/automation-storage-v2/TianshanOS/output/playwright/ui-correction/acceptance.json)。打包资源版本：`f748ad1cb86d7c9f`。
