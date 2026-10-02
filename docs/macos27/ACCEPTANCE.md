> **2026-09-29 当前结论：本地修订已实施，发布验收未完成。** 用户授权的压缩器修复已落地；当前证据和限制以 [CARD-REVISION.md](CARD-REVISION.md) 与 [逐项验收](acceptance-migration.md) 为准。下文全部是被否决版本的历史技术记录，包含过期文件数、Safari 和性能结论，不作为当前证明。

# macOS 27 第三版：本地实施验收

本轮在 `feat/macos27-webui` 上完成全部生产页面及可执行本地条件入口的视觉实施。**发布验收仍未完成**：SPIFFS零增长项FAIL，性能总体INCONCLUSIVE，Safari/真实手机/设备与部分原生输入缺证据。未提交、推送、部署、刷机或向真实设备写入。

本页与 PERFORMANCE.md、acceptance-migration.json 是当前结论；EXECUTION.md是保留初次失败和修复过程的历史日志，不能引用其中旧的中间统计作最终结果。

## 结果与范围

生产修改仅3个文件：`components/ts_webui/web/css/style.css`、`index.html`、`js/app.js`。全部其余1362个已跟踪文件保持基线SHA-256；后端、API/router/terminal/dragSort、语言包、第三方资源、构建链和分区均未修改。

实际打开并核验了批准冻结HTML：`http://127.0.0.1:18781/docs/macos27/reference/approved-macos27-v3.html`，桌面1440×1000、小屏390×844及内存/LED浮层。参考与批准SVG及辅助图哈希匹配。生产交付包含暖金/浅蓝静态背景、中性淡灰分组、原位置同序导航、真实玻璃菜单/模态和准确两路径品牌SVG；没有假窗口、演示业务或新设置入口。

| 范围 | 已完成的本地检查 | 证据 |
|---|---|---|
| 系统 | 资源/电源/时间/设备/风扇/三种LED实体、内存/服务/保护/曲线/组件/菜单 | stage1、baseline-surfaces、nested-surfaces、remaining-surfaces |
| 网络 | 以太网、WiFi、扫描、AP站点/AP配置，30场景 | network-replay.json |
| 文件 | 列表全部列、选择、上传表单、新建、重命名、SD/SPIFFS，36场景；配置包上传/确认分支 | files-replay.json、deep-conditional.json |
| 终端 | 原xterm键入与本地WS输出、日志控件、ANSI palette及选择，12场景 | terminal-replay.json、final-browser.json |
| 自动化 | 数据源4型、动作6型及LED子型、规则嵌套条件/动作、11种组件编辑；5种配置包导入预览 | nested-surfaces-replay.json、imports-replay.json |
| 指令 | 主机/孤儿、新建/编辑、匹配/后台服务/变量/日志/图像选择，36场景及深层入口 | commands-replay.json、rare-surfaces.json |
| 安全 | root/admin/guest UI条件、密钥公私导出显示、证书缺失/已安装、CSR/CA/安装验证、配置包验证/应用确认 | security-replay.json、states-replay.json、deep-conditional.json |
| OTA | 概览/手动、升级/降级/同版本、进度各阶段、WWW部分失败与完成/重启通知 | ota-replay.json、ota-conditional.json |

上述写操作形状的请求全部在严格本地替身或浏览器路由内终止；不是设备写入、密码学验证、SSH执行或真实OTA成功。原确认流程、权限条件、数值/单位、阈值和清理机制保持。

## 功能与运行边界

- 648函数、496绑定、2093相关调用、4164条件、214模板逐项1:1映射；另外768个源码字段/入口候选按原所属函数、顺序与非style标记逐项对应。原始模板和条件保留，不能将候选项数量当独立UI数量。
- 89能力分组中84个当前可达入口有V8实际执行记录，5个非活动旧入口有调用链依据标N/A。**这不是全部内部路径、提交成功或设备功能100%覆盖。** 每个候选的未独立运行状态没有被批量改成PASS。
- 八组实际运行字段/事件/控件清单前后逐项一致，包括min/max/step、value、禁用条件；导入有效/无效预览的按钮状态按响应正确变化。
- AST严格比较及5类负向控制通过；入口逐字恢复比对及3类负向控制通过。完整diff逐块审查结果见 REVIEW.md，未引入业务逻辑变化。
- 浏览器观察覆盖108个两语言/宽度/路由组合、48个角色/空态组合、21个错误/加载/长内容组合；无新增整页横向溢出。少数empty配置故意未指定API返回501，属于明确缺响应分支，不能当完整空数据后端证明。

## 视觉与可访问性

玻璃通过真实backdrop透射，不在浮层绘制假渐变；外侧保留原50%黑色遮罩。仅模态backdrop增加brightness(2)补偿遮罩下亮度，菜单没有该补偿。内部低透明分组保留可读承载面，图表/日志/xterm绘图区不玻璃化。

实际合成背景9个玻璃文字样本最低5.45:1；12个按钮正常/悬停/焦点样本最低4.74:1。语言/成功/告警/危险按钮仅加深各自语义文字，数据色表、LED/图表/ANSI/用户组件色值不变。指定样本通过不等于全部任意数据背景或辅助技术认证，V-11仍按范围保留INCONCLUSIVE。

已检查320/390/768/1024/1440、844×390横屏；真实Chrome200%缩放记录outerWidth1440、innerWidth720、DPR2，无整页溢出。Chromium减少透明度/高对比/forced-colors实际回退实体，减少动态效果取消非必要过渡。无backdrop原生引擎、Safari、真实手机/软键盘/IME/读屏未被模拟结论替代。32个页面/宽度/版本组合另测命中几何，原版38个、新版32个小于24px候选（包括原生range/checkbox和既有紧凑控件）；包围盒不能证明完整命中区域、间距例外或真实触控合格，R-06保留INCONCLUSIVE。

既有缺陷：原版与新版内存弹窗均没有通用Esc关闭、焦点陷阱、关闭后焦点返回；保持原行为，未借换肤改业务。原窄屏隐藏导航/文件名列已改成原位置换行或局部滚动，全部入口/列保留。

## 构建与性能

现有minify/gzip/3MiB SPIFFS构建成功，所有gzip解压匹配实际产物，镜像重建逐字节一致。真实压缩产物全路由运行无新增pageerror；保护源文件及最终配置哈希一致。

| 指标 | 原版 | 新版 |
|---|---:|---:|
| 所有文件总字节（含raw+gzip等） | 2,507,387 | 2,518,348 |
| gzip文件合计 | 370,172 | 372,761 |
| 首访实际响应体总字节（10次各端一致） | 497,930 | 496,437 |
| 首访请求数（含主文档） | 30 | 29 |
| SPIFFS数据/索引页占用 | 2,592,000 | 2,603,264 |
| 离线可用数据/索引页字节 | 357,120 | 345,856 |

首访减少1,493字节和1个请求，来自准确SVG替代旧品牌PNG。完整镜像占用增加11,264字节（11KiB），**旧P-07零增长要求FAIL，尚未收到接受增量的决定**。离线剩余337.75KiB不是设备运行可写容量。

冷加载每端10次，常见路由/弹窗每类50样本，每端50完整循环；A驻留1,827,506ms、B驻留1,827,313ms，最终配置哈希未变。稳定期监听均169、活动socket均1，DOM A839/B841，未记录pageerror或longtask。性能最终时长和计数以 PERFORMANCE.md / performance/summary.json 为准。不能根据冷加载变快抵消其他绘制成本；首轮/files中位数增量在浏览器内时延补测中方向反转，不能据单轮归因；Layerize样本有成本增量，帧量化/共享主机/GPU证据不足等限制保留，整体INCONCLUSIVE，没有使用5%/10%退化额度。

最终www.bin SHA-256：`b04679256720c799c8130256f781b41be6cff0b9d636ff59968e7ffc3a1f6b06`。其他最终哈希在 final-configuration.json / final-hashes.json。

## 证据入口与最小下一步

全部路径以仓库为根：

- `output/macos27-20260928/screenshots.html`：实际生产同视口A/B截图；无配对的诊断图明确标出。
- `docs/macos27/acceptance-migration.json`：133项原要求、替换理由、结果和证据，未删除失败项。
- `docs/macos27/function-inventory.json`、`field-mapping.json`、`source-mapping.json`：能力、字段/入口和源码契约映射。
- `output/macos27-20260928/regression-summary.json`、`asset-transfer.json`、`build-evidence.json`、`performance/`：实际运行与原始性能数据。
- `docs/macos27/REVIEW.md`、`TESTING.md`、`ROLLBACK.md`：完整审查、可复现命令和精确回退。

后续只需针对未验收边界推进：用户解锁Mac以运行Safari/原生输入检查；提供真实手机与只读设备验证环境；决定P-07的11KiB增量是否可接受。设备写入/部署仍需新授权。本轮不把这些缺口作为缩减本地页面实施的理由，也不将目标或发布标记为全部完成。
