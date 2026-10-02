> 2026-09-29：本文记录的是用户已否决版本的历史检查，不是当前修订版验收。当前实施及未解决项见 [CARD-REVISION.md](CARD-REVISION.md)。

# 本轮测试与复现

只连接本机替身，不能指向真实设备。所有运行参数、原始JSON、PNG和日志均在 `output/macos27-20260928/`；此目录不是发布Web根。参考HTML只用于风格对照，所有生产截图来自原版或本轮实际生产文件。

环境：macOS，Node v24.10.0，Chrome 152.0.7977.65。实际批准参考由headed Chrome打开；批量回归与性能使用headless Chrome。Playwright来自现有Codex runtime，Acorn来自已有 `tests/prompts/node_modules/acorn`，未增加产品运行依赖。

## 已执行命令族

以下命令应在仓库根执行。源码和压缩产物服务器需分别占用四个端口；现有会话已在运行时不要重复启动。日志中旧初次替身不足/测试错误保留，最终结论只使用最终重跑和汇总。

```sh
node tests/macos27/fixture-server.cjs output/macos27-20260928/baseline/web 18782 output/macos27-20260928/baseline/requests.jsonl
node tests/macos27/fixture-server.cjs components/ts_webui/web 18783 output/macos27-20260928/after/requests.jsonl
node tests/macos27/fixture-server.cjs output/macos27-20260928/baseline/web_optimized 18784 output/macos27-20260928/baseline/optimized-requests.jsonl
node tests/macos27/fixture-server.cjs output/macos27-20260928/after/web_optimized 18785 output/macos27-20260928/after/optimized-requests.jsonl
```

fixture只监听127.0.0.1，无真实设备代理。未知API返回501。`state=populated/provisioned`、`role=root/admin/guest`、`lang=zh-CN/en-US` 是明确的合成分支；empty配置某些未指定端点返回501，不能据此声称纯空数据的所有后端响应通过。早期日志与最终优化资源日志分开保留。

```sh
node tests/macos27/replay-baseline.cjs
node tests/macos27/replay-after.cjs
node tests/macos27/rare-surfaces.cjs
node tests/macos27/remaining-surfaces.cjs
node tests/macos27/deep-conditional.cjs
node tests/macos27/ota-conditional.cjs
node tests/macos27/populated-variables.cjs
node tests/macos27/keyboard-auth.cjs
node tests/macos27/lifecycle-input.cjs
node tests/macos27/language-failure.cjs
node tests/macos27/zoom-profile.cjs
node tests/macos27/final-browser.cjs
node tests/macos27/control-contrast.cjs
```

`replay-*.cjs` 调用13个同名UI过程。它保留单个过程的异常后继续，因此不能仅凭退出0判通过；必须检查 `replay-final.log`、JSON和 `result-audit.py`。缺私钥配置中3个禁用证书按钮的点击超时原样保留为FAIL，已由provisioned独立测试覆盖可用分支。真实认证、签名、升级和SSH均未执行。OTA的start/reboot形状请求只在浏览器内被显式拦截，原始请求留档。

导入测试使用 `/private/tmp/macos27-valid.tscfg` 与 `macos27-invalid.tscfg`。内容为本地合成文本，前者不含 `INVALID`，后者含 `INVALID`；接口preview结果是合成值，不是密码学验证。测试点击原file input并只预览，不提交导入。

```sh
node tests/macos27/verify-contract.cjs output/macos27-20260928/baseline/web components/ts_webui/web
node tests/macos27/contracts.cjs components/ts_webui/web output/macos27-20260928/after/contracts.json
node tests/macos27/map-contracts.cjs output/macos27-20260928/baseline/contracts.json output/macos27-20260928/after/contracts.json docs/macos27/source-mapping.json
node tests/macos27/map-runtime.cjs
python3 tests/macos27/result-audit.py
python3 tests/macos27/final-integrity.py
```

契约检查有5类JS负向控制和3类入口负向控制。单独读取每个app.js差异后才加入表现白名单；不能将AST PASS当运行覆盖。V8覆盖按函数入口计数，不能推出每个内部条件或业务成功。

## 真实构建

沿用 `tools/minify_web.py` 和当前ESP-IDF `spiffsgen.py`；未修改构建脚本、分区或服务器代码。

```sh
cp -R components/ts_webui/web/. output/macos27-20260928/after/web_optimized/
python3 tools/minify_web.py output/macos27-20260928/after/web_optimized --gzip
/Users/massif/.espressif/python_env/idf5.5_py3.12_env/bin/python /Users/massif/esp/v5.5.2/esp-idf/components/spiffs/spiffsgen.py 0x300000 output/macos27-20260928/after/web_optimized output/macos27-20260928/after/www.bin --page-size=256 --obj-name-len=32 --meta-len=4 --use-magic --use-magic-len
python3 tests/macos27/build-evidence.py
```

基线用相同参数。所有gzip逐个解压与同目录实际压缩前文件相等；生成器重建镜像逐字节相等。`build-evidence.json`是离线页账，不冒充设备SPIFFS运行空闲容量。压缩脚本内原有正则能力有限，因此另对真实产物运行页面、语法和完整调用/条件映射。

## 性能与截图

```sh
node tests/macos27/performance.cjs output/macos27-20260928/performance/final-v3
node tests/macos27/scroll-performance.cjs
node tests/macos27/paint-trace.cjs
node tests/macos27/paint-trace.cjs --solid-diagnostic
node tests/macos27/asset-transfer.cjs
python3 tests/macos27/performance-report.py
```

性能每端10次交替冷加载、50路由/弹窗循环、250交互和至少30分钟驻留。`asset-transfer.cjs`临时监听18786/18787，发送字节准确的gzip，测试脚本通过Playwright initScript注入；API仅转发到上述固定localhost替身。测试结束关闭服务器。

需要Pillow的 `contrast.py`、`control-contrast.py`、`evidence-catalog.py` 使用现有runtime Python：`/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3`。实际合成背景诊断只清除文字颜色，正常截图独立保存，不把诊断图当视觉交付。

`stage1.js`和`material-audit.js`通过Playwright CLI `run-code --filename`执行，原始工具JSON在对应log。`zoom-profile.cjs`是临时独立Chrome配置的真实200%缩放，实际innerWidth与DPR已记录；失败的快捷键试验不算PASS。用户浏览器偏好未修改。

所有实际命令和退出状态另汇总于 `command-results.json`；平台边界见 `platform-boundaries.json`。缺Safari/手机/实机及噪声项按真实状态保留。
