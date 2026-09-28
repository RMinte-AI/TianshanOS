# J1 电源告警显示修复

结论：**J1 已修复，本轮要求的本地验证通过；真实设备与远端终端显示仍待实机验收。** 本轮不对其他系统功能作无缺陷承诺。

实施基线 HEAD：`8ad39a9b1ca39d225f94fb4db602fb2ee7438d09`，分支 `fix/v0.5.2-reliability-and-prompts`。开始时无 tracked 修改，原有 output/、tmp/ 和 runtime-repair 下未跟踪文件未纳入修复。实施阶段未提交、推送、更新 PR、合并、发布或操作设备。用户随后授权更新 PR；提交前审查见 [publication-gate.md](publication-gate.md)。

## 修改范围

| 文件 | J1 必需的改动 |
|---|---|
| components/ts_webui/web/js/terminal.js | 仅 handlePowerEvent：销毁或无 terminal 时返回；完全采用指定 canRestoreLocalInput 条件。只有本地可交互时走原恢复代码，否则仅另起一行输出原告警 |
| components/ts_webui/web/index.html | 沿用 TS_ASSET_VERSION 和查询参数方式，缓存标识更新到 20260927-power-prompt；无布局改动 |
| tests/prompts/regression.test.cjs | 在既有 Node 测试中追加中英文状态矩阵，执行生产消息分发与已注册输入回调 |
| tests/prompts/project-cross.cjs | 在原跨层链中追加 J1：真实 C 保护通知 → 生产 socket 消费 → 实际输入 → 原 C Shell → 正常退出与回收；原 H1 等断言保留 |
| tests/ws_subscriptions/test_project.c | 仅测试夹具增加 power 命令，调用生产 power_policy_event_handler；输出原 op identity 供断言归属，不修改任何生产后端 |
| tests/ws_subscriptions/verify_project_assets.py | 原校验器硬编码旧 cache key，改为与当前源 index 标识比对；增加既有风格的结果目录参数，避免覆盖 v3 历史证据 |

[实际 diff](changes.patch)、[前后输入哈希及范围记录](inputs.json)。已比较 terminal.js 修改前后：handlePowerEvent 之外完全一致；index 只替换缓存标识。后端 SSH、WS/operation、队列、引用、取消、采样、保护、版本和发布流程均未改。

## 行为与测试结果

修复前 J1 状态测试在活动 SSH 上失败：连续两次告警产生 8 次写入（包含清行、本地提示符/缓冲与光标），期望仅两次通知；实际 C 跨层反例也出现 4 次写入而不是一条通知。见 before-node.txt、before-cross.txt；这两项是业务断言失败，不是编译或环境失败。

| 场景 | 本次验证结果 |
|---|---|
| 本地 abcd 未提交、光标在第 2 位 | 连续告警均保留原 clear → 告警 → tianshan> abcd → 左移 2 位序列；字段不变，无自动提交；一次告警只恢复一次输入行 |
| 活动 SSH，连续告警 | 通知可见且另起一行；不清远端行、不写本地提示符、不回放 abcd、不用本地位置移动光标；状态及发送列表不变 |
| 告警后继续输入 | 实际 onData 产生 ssh_input；跨层返回真实 C handler 后，原 operation identity 不变且 driver 写入增加一次 |
| 正常退出 | 实际退出键产生 ssh_disconnect；请求后以及 disconnecting 阶段无本地提示符，收到 closed 后才恢复；真实 C channel 最终为零，原全部资源结算断言通过 |
| connecting / disconnecting / restoring | 均不恢复本地输入行，告警不改变任何指定状态字段；原受限输入仍被忽略 |
| 连接断开 / socket 非 OPEN / socket 不存在 | 不恢复本地提示符、不触发恢复或业务请求 |
| destroyed / terminal 不存在 | 直接调用告警处理也不访问销毁对象、不抛异常、不发请求 |
| 已从 SSH 正常关闭后再次告警 | 根据当时真实本地状态恢复提示符、缓冲与光标，不沿用先前 SSH 状态 |
| 原事件筛选 | countdown 31 不显示；30 及原保护通知仍显示。告警文本、颜色、翻译和 switch 分支无修改 |

状态测试使用实际 terminal.js 和实际注册输入回调。Chrome 跨层加载真实入口、语言包和 socket 消费者；SDK 最终消息 bytes 来自生产 C 保护处理及传输层。SDK/网络、libssh2 外边界和 xterm 渲染仍为既有测试替身，不等于实机运行。

## 本次执行记录

以下均为本轮重新执行，未用历史结果代替：

- `node --test --test-name-pattern=J1 tests/prompts/regression.test.cjs`：中英文 J1 状态/输入/筛选断言通过，见 after-node.txt。
- `npm test --prefix tests/prompts`：40 项通过，包含原回归及新增 J1；见 webui-node.txt。
- `npm run test:browser --prefix tests/prompts`：原浏览器回归 38 项通过，见 webui-browser.txt。
- `CROSS_LANGUAGE=zh-CN/en-US node tests/prompts/project-cross.cjs`：两种语言均通过，保留原 H1、归属、生命周期等断言；见 cross-node-results.json 及对应消息证据。
- 上一条加 `--browser`：两种语言 Chrome 跨层通过，见 browser-results.json。
- 使用本次构建目录的 `PROJECT_WEB_ROOT=.../web_optimized`，再次运行双语 `project-cross.cjs --browser`：通过，见 optimized-results.json。证明打包后的消费者也具有 J1 行为。
- `git diff --check`：通过。

C 夹具使用 `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf`、`DEVELOPER_DIR=/Library/Developer/CommandLineTools`。跨层记录通过 `CROSS_OUTPUT` 保存到本目录。没有新增测试框架或变更依赖版本；独立控制/取消全套主机入口本轮未额外重跑，相关生产代码未修改，且现有断言未删改。

## 本次构建及静态资源

固定 ESP-IDF v5.5.2 / ESP32-S3，独立目录：`/tmp/tianshan-j1-build/build`。

```sh
# 在固定 SDK export.sh 环境中
idf.py -B /tmp/tianshan-j1-build/build -D SDKCONFIG=/tmp/tianshan-j1-build/sdkconfig build
IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf \
V3_BUILD=/tmp/tianshan-j1-build/build V3_RESULTS=docs/repair/ws-j1 \
python3 tests/ws_subscriptions/verify_project_assets.py
```

构建成功；应用镜像 0x215120，应用分区约 31% 剩余。见 build.log。该日志包含 SDK 的刷机用法提示，**没有执行刷机命令**。

[assets.json](assets.json) 记录源码、优化文件、gzip、固件和 www 的哈希。JS 语法检查、gzip 解压一致、新 cache key 检查通过；用验证后的优化目录重新生成 SPIFFS，与实际 www.bin 逐字节一致。中英文优化资源跨层回放同时通过。支持固件/www 配套更新并刷新页面；未验证设备实际浏览器缓存升级过程。

## 未执行及验收边界

本轮要求的本地项目均已执行，无缺少环境而跳过的必需项目。未授权实机，因此未测试设备上的真实电源告警、真实 SSH/xterm 光标呈现、网络时序及缓存升级。实机应确认本地行中输入、活动 SSH 连续告警、输入与退出、连接过渡状态和更新后资源缓存的同样行为。没有发送远端重绘、回车或查询；远端提示符仍由远端输出决定。
