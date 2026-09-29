# TianshanOS

天山操作系统的 WebUI 设计系统。TianshanOS 是运行在 ESP32-S3 上的机架管理系统，管理 Jetson AGX 与 LattePanda Mu 载板，WebUI 以 SPIFFS 镜像形式存放在设备里，前端为原生 HTML、CSS、JS，没有构建框架。

本系统提取自 `components/ts_webui/web/` 的生产代码（`index.html` 关键 CSS 与 `css/style.css`）和 `docs/macos27/reference/` 的批准稿 v3。数值以生产代码为准，两者不一致的地方在下面注明。

## 内容规范

- 界面为中英双语，所有文案走语言包（`js/lang/zh-CN.js`、`en-US.js`）。新增文案两种语言同时加。
- 标签用名词或动词短语：「系统」「网络」「同步时间」「重启」。按钮写它做的事，不写「确定」。
- 数值和单位分开排版：`128 / 320 KB`、`19.2 V`、`40%`。主值大、单位小、辅助说明更小。
- 状态用词固定：已连接 / 未连接、已启用、已同步。危险操作要写明对象，例如「删除证书」。
- 设备是真实硬件：写错误时带上设备侧原因，不写「出错了」。

## 视觉基础

- **结构**：彩色壁纸之上，悬浮一条玻璃顶栏和一块近白色的主内容面。顶栏和内容面圆角为 `radius-shell`（23px）。
- **分层靠底色**：卡片用 `bg-muted` 灰底，无边框、无阴影（`--shadow` 为 none）。只有白色按钮带 `shadow-sm`。不要给分组叠加边框加阴影。
- **玻璃**：仅顶栏、菜单、弹窗使用 `shadow-glass`，浮层实色回退为 `bg-muted`。用户开启「减少透明度」或浏览器不支持 backdrop-filter 时回退实色。
- **强调色**：只有一个蓝色。填充和高光用 `accent`，文字和主按钮用 `accent-text`。状态色（成功、危险、告警）与强调色分开。
- **红色预算**：破坏性按钮平时中性灰，悬停才变红；红色只出现在确认弹窗主按钮、故障状态、校验错误三处，见 Audit 十六。
- **状态表达**：淡底（`*-tint`）加深色文字（`*-text`），例如成功按钮是 `success-tint` 底配 `success-text` 字。避免整卡、整行染色。
- **圆角**：按钮和输入框 `radius-sm`，卡片 `radius-lg`，弹窗 `radius-modal`，导航和状态用 `radius-full` 胶囊。
- **动效**：过渡 0.15s / 0.2s，缓动 `cubic-bezier(0.4, 0, 0.2, 1)`。尊重 `prefers-reduced-motion`。
- **无障碍**：文字对比度不低于 4.5:1，已有 forced-colors 和高对比度回退。

## 图标与品牌

- 图标字体为 Remix Icon（`web/fonts/remixicon.woff2`）。
- 品牌标志是一座山形线稿，描边 `#293d54`，见 `assets/Logos/tianshan-mark.svg`。
- 标志与「TianshanOS」字标并排，字标为 `brand` 样式。

## 与批准稿的差异

批准稿 v3 原型使用 `--accent:#0669d5`、`--radius:18px`、`--rbtn:8px`，生产代码使用 `accent-text:#0064d2`、`radius-lg:20px`、`radius-sm:9px`。本系统以生产代码为准。改视觉时先决定是把生产靠向批准稿，还是把批准稿更新到生产值。

## 使用注意

- 新代码只用本系统的语义令牌。`legacy-*` 颜色仅为对照旧样式表而保留。
- 本系统只有浅色主题，源码里没有深色主题实现。
- 页面体积受 SPIFFS 3 MiB 限制，见「优化方向」一节；性能预算见 `Performance.md`。
- 字体：正文和数字用系统字体；Quantify（自购授权）只用于品牌字标，不用于数字，也不内嵌字体文件。授权范围（网页嵌入、Bold 的 Personal Use 标记）待与 Sentype 确认。


## 补充文档
- `NativeDialogs.md`：原生 prompt/confirm/alert 到 sheet/内联校验的对照表，含改代码时的异步改动面。
- `Icons.md`：remixicon 到内联 SVG 精灵的图标清单。
