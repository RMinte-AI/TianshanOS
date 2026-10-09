# 删除保护修复：实现与验证交付

2026-10-09。本地实现已完成，等待用户／另一 AI 的代码审查；没有提交、推送、合并、发布或刷机。

## 位置与基线

- **分支**：`fix/automation-delete-protection`。
- **独立工作目录**：`/Users/massif/.codex/worktrees/automation-delete-protection/TianshanOS`。
- **基线**：`main` / `050757bb8046206797804ba5edc110ae3244bf17`（0.6.1）。
- 修改保留为该工作目录中的未提交文件；仅 checkout 分支名不能在另一目录取得这些尚未提交的内容，审查请直接进入上面的工作目录。
- `/Users/massif/TianshanOS` 仍在 main，tracked diff 为空；原有未跟踪文档/工具保留。
- 没有合入 `fix/ssh-fingerprint-confirm`，没有修改通用 HTTP 失败出口或密钥逻辑。

## 用户可见结果

1. 动作模板仍被规则使用时，删除被拒绝，说明框列出具体规则；包含停用规则，重复引用不重复列出。解除引用后才可删除。
2. 另一个页面先删除模板后，用旧选择保存新引用会被拒绝，提示用户实际选过的模板名称；草稿保留。后端只返回可靠 ID。
3. 保存拒绝后 `committing` 清除，原规则仍能手动触发、再保存、启停。
4. 模板删除拿不到事务时，区分仍在加载、正在更新与恢复异常；无法确认引用时不谎报已确认被引用。
5. 服务阻断按九种状态与未完成操作分流；用户删模板时标题/关系说明也明确模板与指令的关系。
6. 首页区分没有快捷操作、正在加载和恢复异常，保留面板及前往自动化入口。
7. “查看规则／查看指令”只跳转，不定位、不高亮、不自动停止或删除。C 组只有关闭。
8. 现有 sheet 的字体/色彩/容器保持；长名称、长标题和63字节ID可以换行，手机/桌面不裁切。阻断框出现时，本次“处理中”提示立即隐藏，不遮挡按钮。

## 技术边界

- 删除在 transaction → binding → 短期规则/模板 mutex 的顺序下完成检查和原移除实现；不拿反向锁、不新增锁或常驻任务。
- 新增或增加次数的缺失引用不能保存；历史缺失引用的原有出现次数可保留，不自动修复，不放宽执行保护。
- 490 行的原服务保护保持。
- API 业务失败保持 ESP_OK + 非零 code，data 从 main 的生产成功包络带出。细节无法生成／丢失时仍拒绝并有回退。
- 不改变 SD/NVS 加载、镜像、导入和存储格式，不根据“拔卡”推断内部备份必然可用。
- 现有模板实际持久化、外部手动改卡、热拔插与断电恢复并未因本轮主机模拟获得真机验收。

## 上一轮验证（第三轮实施前的源码）

| 检查 | 实际结果 | 证据 |
|---|---|---|
| 运行时总入口 `IDF_PATH=… bash tests/runtime/run_all.sh` | 通过，包含原存储、服务、SSH、规则生命周期等回归 | [runtime.log](evidence/runtime.log) |
| 生产删除/提交/HTTP字节 | 通过，8个固定响应；两种并发顺序用屏障控制；拒绝时无移除/文件清理/提交；历史次数、committing、47/63边界 | [reference-wire.log](evidence/reference-wire.log) |
| 静态/交互总入口 `npm --prefix tests/prompts test` | **126/126，通过** | [unit.log](evidence/unit.log) |
| 实际 Chrome 浏览器总入口 `npm --prefix tests/prompts run test:browser` | **262/262，通过** | [browser.log](evidence/browser.log) |
| H15 状态矩阵 | 9×2×4×3×2，共432个映射分支，中英都不显示原枚举 | `tests/prompts/delete-reference.test.cjs` |
| 完整 ESP32-S3 固件与 SPIFFS 构建 | 通过，最终源码；构建版本 `0.6.1+050757bbd.10092140` | [build.log](evidence/build.log) |
| diff 空白/语法与主目录隔离 | `git diff --check` 通过；主目录 tracked diff 为空 | 本工作目录与主目录的 git status/diff |

生产函数测试的 RTOS、存储、SSH 等外围使用合成边界，不连接设备。浏览器原样回放生产 HTTP 字节，实际 api.js、按钮、确认框和路由参与；没有用假的 api.call 绕过出口。

视觉验证包括中英文、390/1440宽度、32条引用、名称47/ID63字节、最大服务标题、子元素裁切、进度提示遮挡、Esc/Enter/关闭、双重阻断、迟到操作与页面离开。

## 上一轮资源与工件（不代表第三轮净增量或新构建）

- 最终 ELF 类型尺寸：引用条目112B、删除诊断20B、提交诊断84B（新增64B请求局部ID），规则结构仍240B。
- 引用细节默认32条最多3584B临时PSRAM；配置上限64条时7168B。新增引用存在性查询使用一个1224B临时模板缓冲，退出即释放。
- 无新增常驻任务、每条规则持久化字段、分区或轮询频率。
- 六个修改的 Web 源文件按相同 gzip 参数测得净增加 **2807B**；这是源文件gzip对比，不是假称实际网络包或板卡内存测量。实际SPIFFS构建通过。
- [类型尺寸](target-sizes.txt)、[逐文件资源变化](resource-deltas.json)。
- 本地待审工件：`build/delete-protection/TianShanOS.bin` 与 `www.bin`，以及配套 bootloader/partition/ota 数据；[SHA-256清单](evidence/artifacts.json)。未写入设备。
- [源文件哈希](evidence/source-hashes.json)对应本次本地实现，便于审查时识别后续变动。

## 文案与上一轮截图

- [已确认实施方案 v3](PLAN.md)。
- [实际中英文文案](COPY.md)：包含用户确认的第二轮修订；占位参数一致。
- [中文手机](../../../output/playwright/delete-protection/zh-CN-390.png) / [中文桌面](../../../output/playwright/delete-protection/zh-CN-1440.png)。
- [英文手机](../../../output/playwright/delete-protection/en-US-390.png) / [英文桌面](../../../output/playwright/delete-protection/en-US-1440.png)。
- 同目录截图旁的 `*-style.json` 记录实际字体、圆角、宽度。

## 下一位 AI 的代码审查入口

请在上面的独立工作目录执行只读 `git diff` 并查看新增未跟踪文件，重点核查：

- `ts_action_template_remove_checked` 的事务/绑定顺序、列表去重、确认结论与细节可用性的分离、lease释放及拒绝后无副作用。
- `validate_new_template_refs` 的旧/新次数比较、NOT_FOUND与读错误区分，拒绝分支清除committing，历史兼容不放宽执行保护。
- API真实包络及前端回退、选择名称来源、失效页面回调、单按钮关闭、处理中提示与后续toast正常恢复。
- COPY.md是否逐字符合已批准文案，long-title/list样式是否仍符合既有设计规则。
- 上述日志和工件确实对应当前源码；若审查后修改源码，重跑与修改相关的检查，而不是继承旧PASS。

审查不等于授权推送、合并、发布或刷机。原历史断裂配置仍保留原安全保护，不能宣称本轮自动清理了所有现场旧配置。


## 第三轮补记：2026-10-10，按第三轮方案 v2 完成

用户已授权实施 R1–R5 并确认三句非 root 首页说明。完整方案在主目录 `output/review-delete-protection-copy/第三轮修复方案与交接-v2.md`，回执在同目录 `第三轮实施回执.md`。仍为原独立工作目录中的未提交修改。

- R1：静态检查加入 deleteProtection，当前原24键加新增3键均由相同检查覆盖。
- R2：无未完成操作证据的未知/缺失服务状态使用 unconfirmed；真实 pending 分支及删除安全门保持。
- R3：template_lookup_failed 使用现有 saveFailed；action_missing 名称说明保持，保存失败草稿保留。
- R4：只有 root 显示自动化入口；三句已确认非 root 说明按场景使用，root 原正文/补充句保持。新 t() 使用字面量键。
- R5：指令自身的服务阻断框只有关闭，关闭不导航或重载；动作模板查看指令和查看规则继续只跳转。
- TS_ASSET_VERSION 和 app.js 查询串为 delete2；api.js/CSS 保持 delete1，xterm 地址随共享标识变化。没有改变发布版本。

| 本轮实际命令（独立目录） | 结果 | 证据 |
|---|---|---|
| `node --test tests/prompts/static.test.cjs tests/prompts/delete-reference.test.cjs tests/prompts/delete-protection.test.cjs` | 33/33，通过 | [round3-unit.log](evidence/round3-unit.log) |
| `node --test tests/prompts/delete-reference.browser.test.cjs tests/prompts/delete-protection.browser.test.cjs` | 38/38，通过 | [round3-browser.log](evidence/round3-browser.log) |
| `git diff --check` | 通过，无空白错误 | [round3-verification.json](evidence/round3-verification.json) |

新增浏览器场景只有中文 admin 三状态和中文查询失败保留草稿；已有相关文件的语言/视口/服务覆盖照常执行，没有新增矩阵。admin 同时用于 auth/status 返回等级和初始化 localStorage，实际 api.getLevel 为 admin、isRoot 为 false。三状态无按钮、对应定稿出现、旧 root 管理操作提示不出现；加载/恢复标题与正文保持，空状态仅替换正文。

本轮未运行固件测试、全量测试、ESP-IDF构建或真机测试；固件/HTTP未修改，用户明确限定验证范围。上方126/262结果、构建日志、截图、二进制及2807B资源统计均为上一轮历史证据，不能当作本轮固件工件或资源增量。没有重新生成 HTTP fixture、截图或二进制。

[source-hashes.json](evidence/source-hashes.json)现对应本轮源码；[source-hashes-before-round3.json](evidence/source-hashes-before-round3.json)保留上一轮源码记录，旧工件哈希不变。复审可用[round3.patch](evidence/round3.patch)查看相对上一轮未提交实现的精确增量，而不只与 main 比较。补丁采用无上下文格式，避免空白上下文行被 Git 当作尾随空格；需要应用时使用 git apply --unidiff-zero。

方案无实质偏差，没有需要用户另作决定的事项。固件、通用HTTP出口、锁顺序、committing及api.js/CSS等受保护文件与本轮开始时逐字相同；main源码未改。未提交、推送、开PR、合并或刷机，未修改其他分支/工作目录。
