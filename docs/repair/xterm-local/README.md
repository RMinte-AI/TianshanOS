# Web Terminal xterm 本地化

基线 633bb94df3e1e454929a14257c06c307c09100e6；当前 PR #42 分支。初始工作区与原未跟踪文件见 baseline.txt；原 output/、tmp/、runtime-repair 未跟踪文件保持原样并排除提交。

## 生产修改与边界

- `components/ts_webui/web/js/terminal.js`：仅三个 loader URL 改为同源 vendor + TS_ASSET_VERSION；保留按需 Promise、去重、失败 UI/重试及 destroy 防护。J1、SSH、输入、终态等行为未改。
- `components/ts_webui/web/index.html`：原缓存标识统一更新到 20260928-local-xterm。
- `components/ts_webui/web/vendor/xterm/`：三个发行文件和两个独立 MIT LICENSE，完整来源/版本/原路径/SHA256/大小见 [vendor.json](vendor.json)。开发阶段从原运行时固定版本 jsDelivr 地址一次性取得；构建不下载依赖。无 map、src、typings、npm 包或缓存。
- `tools/minify_web.py`：两个 JS/CSS 循环各增加 vendor 目录跳过；后续 gzip 不变。

CMake、SDK 配置、版本、发布流程、后端、SSH、硬件保护和前端布局均未修改。

### 经用户确认的路径调整

当前 SPIFFS 路径含目录最多 32 字节。原建议 addon 与许可证长路径实际导致 SDK 失败，证据见 path-limit.txt 和 initial-build-path-failure.log。用户确认后仅改为 `vendor/xterm/fit.min.js` 与 `vendor/xterm/LICENSE-fit`，其字节和版本不变。旧失败目录存在缓存的 GLOB 名称，因此最终使用全新独立目录 `/tmp/tianshan-xterm-final/build`，没有改 CMake 或文件系统格式。

## 版本、许可证和静态资源占用

| 文件 | 版本 | raw 字节 | gzip 字节 |
|---|---|---:|---:|
| xterm.min.js | xterm 5.3.0 | 283670 | 65684 |
| xterm.css | xterm 5.3.0 | 5383 | 2037 |
| fit.min.js | xterm-addon-fit 0.8.0 | 1789 | 822 |
| 合计运行时资源 | 固定版本 | 290842 | 68543 |

两个 LICENSE 共 2364 字节；含许可证 vendor 原始总计 293206 字节。源码 vendor 与 web_optimized 未 gzip 文件逐字节相同，gunzip 后也逐字节相同。两份 MIT 版权信息保留。

SPIFFS 同时包含 raw、gzip 和许可证：新增文件数据合计 361749 字节（不含文件系统元数据）。生成器已分配 676/768 个 4096 字节块，未分配 92 块（376832 字节）。这是生成器布局统计，不等于运行时 ESP SPIFFS free API；未声称固定 3 MiB 镜像文件大小就是实际新增占用。构建未溢出，仍需设备挂载验收。

## 本次验证与重复入口

1. `node --test tests/prompts/xterm-vendor.test.cjs`：固定 SHA256、skip-minify/gzip identity、生产 loader 冷启动、共享 Promise、失败后重试通过。见 vendor-test.txt。
2. `npm run test:xterm --prefix tests/prompts`：Node 专项及双语 Chrome 通过，见 offline-source.txt。全新 context 禁止 Service Worker、全部非 fixture origin 拦截；未注入 Terminal/FitAddon。真实 Terminal 构造、FitAddon、页面初始化、真实键盘输入、SSH、J1、退出、销毁和重入通过。
3. 原 WebUI Node 40 项、Chrome 38 项重新执行通过，见 node.txt、browser.txt。原资源失败/重试测试改为本地文件故障注入，恢复后使用真实 bundle；未弱化断言。
4. J1 专项通过，见 j1.txt。实际 C→JS→C v3 双语回归通过，见 cross-en/zh.txt/json，保留 H1、控制归属、取消/回收等既有断言。
5. 固定 ESP-IDF v5.5.2 / ESP32-S3，独立目录全量 `idf.py -B /tmp/tianshan-xterm-final/build -D SDKCONFIG=/tmp/tianshan-xterm-final/sdkconfig build` 成功，见 build.log。日志刷机命令只是 SDK 输出，没有执行。
6. `PROJECT_WEB_ROOT=/tmp/tianshan-xterm-final/build/esp-idf/ts_webui/web_optimized OFFLINE_RESULTS=docs/repair/xterm-local/optimized node --test tests/prompts/xterm-offline.test.cjs`：本次打包资源双语离线回放通过，见 offline-optimized.txt。
7. `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf V3_BUILD=/tmp/tianshan-xterm-final/build python3 tests/ws_subscriptions/verify_xterm_assets.py`：全部 vendor/gzip/许可证存在、字节一致、最终 www.bin 目录名存在；固定 SDK 重建 SPIFFS 与实际 www.bin 字节相同，容量及 flash 文件校验通过，见 build-assets.json。
8. 相同 IDF_PATH/V3_BUILD 加 `V3_RESULTS=docs/repair/xterm-local python3 tests/ws_subscriptions/verify_project_assets.py`：原优化 JS 语法、gzip、缓存标识和 SPIFFS 重建验证通过，见 project-assets-check.txt、assets.json。
9. `python3 tests/release/test_release.py`：3 项通过。静态检查现有 workflow 的 build/*.bin 上传与 firmware/**/*.bin Release 模式包含 www.bin。无 workflow 修改。
10. `git diff --check` 通过；build-inputs.json 的生产输入哈希在构建后复核一致，无构建后生产输入改动。runtime-search.txt 记录 components/tools/tests 无可执行 xterm CDN 引用。

完整网络记录：源码 [英文](requests-en-US.json)、[中文](requests-zh-CN.json)；优化产物 [英文](optimized/requests-en-US.json)、[中文](optimized/requests-zh-CN.json)。每条 HTTP/HTTPS URL/origin 均记录且断言同源，外部请求为零、pageerror 为零。vendor 请求带现有缓存标识。无 CDN fallback。

浏览器真实运行 xterm 及键盘；网络/设备 WebSocket 是受控替身。现有 resize 仅调用 fit，Ctrl+C 发送 ssh_input；未擅自新增 ssh_resize/ssh_signal。独立 resize/signal 控制路径由现有 C 跨层测试保留验证。

## 从产物到运行入口及发布链

最终 www.bin 由逐字节核验的 web_optimized 构建，含 vendor/raw/gzip/LICENSE。`ts_webui.c:static_file_handler` 去掉 query 后访问 /www 对应路径；`ts_http_send_file` 识别 JS/CSS，按 Accept-Encoding 发送 .gz。浏览器在源资源及实际优化资源上均仅请求本地路径并完成终端输入和 SSH 模式操作。真实 ESP HTTP 服务尚未执行。

[flasher_args.json](flasher_args.json) 的 flash_files 包含 `0x6a0000: www.bin`；CMake 现有 FLASH_IN_PROJECT 未改。**使用项目标准 idf.py flash 时，本地 xterm 随 www.bin 自动刷入设备，无需用户单独复制文件。单独刷 TianShanOS.bin 不会更新 xterm，因为 WebUI 位于独立 www 分区。**

现有 OTA 的 includeWww/startWwwOta、ota.www.start、ota.www.start_sdcard 与 /ota/www 上传链支持配套升级 App + www；App-only OTA 不更新 WebUI，这是原语义。现有 workflow 继续发布 www.bin；本轮不修改工作流、打 tag、合并或创建 Release。

## 验收边界

本次必需的本地检查已执行。真实设备浏览器、SPIFFS 挂载/静态 HTTP/gzip、更新后的缓存与真实 SSH 仍待实机验收；未刷机、重启或执行真实远端命令。构建成功和模拟协议不代替设备验收。发布准备按用户授权仅提交并推送本轮范围到 PR #42。
