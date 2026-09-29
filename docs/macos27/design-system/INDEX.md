# macOS 27 WebUI 设计系统（Claude Design 定稿）

改 WebUI 前按此顺序读：

1. `README.md`：视觉基础、内容规范、红色预算。
2. `Audit.md`：审查结论与规范。第十三节是 macOS 27 对照，第十六节红色预算，第十七节分组表单，第十八节弹窗统一与页脚字体。
3. `Performance.md`：性能预算。数字来自仓库静态文件和本地替身，不是真机实测，标【待核】的必须先核实。
4. `NativeDialogs.md`：`prompt/confirm/alert` 的替换对照，`confirmAction()` 要改成异步。
5. `Icons.md`：remixicon 到内联 SVG 精灵的图标清单。
6. `tokens.json`：颜色、字体、间距、圆角令牌。
7. `boards/*.html`：各页面和弹窗的目标稿（静态 HTML，可直接用浏览器或 Playwright 打开，1440 宽）。`spec.css` 是样式。文件名对应：Main 系统页、Net 网络、Files 文件、Term 终端、Auto 自动化、Cmds 指令、Ota 固件升级、Sec 安全、Spec 控件与材质、Sheets* 各类弹窗、SheetsStates 菜单提示与状态、SheetsMore 表单规范、Resp 响应式与英文。
   稿里的页脚品牌字用 Quantify（Regular），字体文件不在此目录；本目录内的稿会回退成系统字体。

## 边界

- 只做浅色，不做深色。
- 数字不用 Quantify；Quantify 只用于品牌字标（SVG 描边）和页脚「TianshanOS · RMinte® AI」一段。
- 只改样式，不改功能；`onclick` 处理函数、接口、路由、语言包 key 不动。新增文案中英文同时加。
- 落地前逐页核对功能清单，见 Audit 第十二、十四节。

## 目录说明

- `boards/`：各页面和弹窗目标稿的静态 HTML 副本（只读参考，不含字体文件）。
- `source/canvas/`：在线设计画布的可编辑源文件（`*.dc.html`、`canvas.json`、`spec.css`、`quantify.css`）。`.dc.html` 是设计画布的组件格式，用设计画布工具打开、编辑和发布；`quantify.css` 内嵌 Quantify 字体。
- `source/generator/`：生成这些稿的 Python 脚本（`gen.py` 到 `gen5.py`，按序 `exec` 叠加）和测高脚本 `measure.cjs`。脚本里的目录路径是当时环境的绝对路径，重跑前要改成本机路径；需要 Python 3.12 和 Playwright。这是生成过程的记录，不是产品构建的一部分。
- `source/audit-shots/`：Audit 文档引用的 5 张审查截图；`source/assets/` 是品牌山形标志 SVG。
- `demo/`：官网嵌入用的演示包（7 个页面，菜单互相跳转，见 `demo/README.md`）。
- 在线版本：画布「TianshanOS 界面优化稿」和设计系统「TianshanOS Design System」保存在作者的 claude.ai 账号里，私有。
