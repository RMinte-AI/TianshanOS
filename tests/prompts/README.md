# Prompt and operation-result regressions

Run from the repository root. The test harness loads the **actual** inline translator in `index.html`, language packs and complete production JavaScript into a VM. It does not import the hand-transcribed reproduction package.

```sh
npm ci --prefix tests/prompts
npm test --prefix tests/prompts
npm run test:browser --prefix tests/prompts
node tests/certificate/test_ui.cjs
node --test tests/prompts/configuration.browser.test.cjs tests/prompts/service-runs.browser.test.cjs
```

The browser suite requires locally installed Chrome (`channel: 'chrome'`). It serves the actual WebUI on an ephemeral localhost port, intercepts all `/api/` calls, replaces WebSocket with a stub and rejects external requests. Only the terminal retry test supplies local fixture responses for CDN URLs. It does not use device addresses, credentials, firmware, or real commands. The xterm fixture verifies resource recovery and production initialization, not xterm rendering itself.

No device validation is performed. Real file storage, browser disk download completion, SSH execution, power state, protection, firmware upgrades and embedded resource packaging require separate authorized acceptance.

`static.test.cjs`: literal/conditional translations, HTML attributes, finite dynamic families and placeholder contracts. `regression.test.cjs`: actual API/UI/terminal functions with controlled network, DOM, time and device-result inputs. `browser.test.cjs`: actual HTML/deferred-script loading, DOM safety, language recovery and terminal CDN retry. Browser screenshots are written under `output/playwright/`.

During this repair, existing local Acorn/Playwright installations were used through `NODE_PATH`; no dependencies or browser binaries were installed. The public submission summary is in `docs/PROMPT_REPAIR_REPORT.md`.

`ownership.browser.test.cjs` covers endpoint contracts, startup/navigation ownership, upload generations, modal ownership, out-of-order outcomes, and independent directory refresh. It is included in `test:browser`.


自动化配置重构的当前网页验证入口：

```sh
npm --prefix tests/prompts run test:configuration
```

`configuration.browser.test.cjs` 使用实际页面和本地 HTTP 设备夹具，检查工作来源、整套副本、保存版本、引用删除／最终强删、立即导入和窗口归属。`service-runs.browser.test.cjs` 检查独立运行身份、冻结主机／密钥引用／日志位置、显式核验与停止、普通后台命令接管，以及流式输出／日志的迟到结果。两者均检查中英文手机和桌面布局。

旧的“服务未确认停止则不得删除”与“等待重启删除”测试及静态响应夹具已经移除，其相反的旧行为不再作为验收标准；真实对象删除和运行记录持久化分别由 `tests/runtime/run_config_storage.sh`、`test_service_control.py`、`run_service_runs.sh` 连接生产实现验证。这里的浏览器夹具没有连接 ESP32、真实 SD 卡或 SSH 主机。其余历史 flow／wire 用例仍在接管，本入口通过不等于全部脚本已经通过。

`npm run test:packaged --prefix tests/prompts` reads the actual build output selected by the absolute `TS_PACKAGED_WEB` environment variable. It checks that script, stylesheet and dynamic language URLs share the generated content version, then operates configuration and independent-run controls in Chrome. It requires a completed ESP-IDF web build and does not silently replace it with source files.
