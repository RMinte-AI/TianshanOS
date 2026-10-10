# CI v2 实施回执

## 授权与范围

用户已批准 v2 并授权实施、提交个人仓库、创建专用上游 PR，持续处理到最新提交 CI 全部通过。设计文档保留审批时的原文，实施状态以本回执为准。

基线：上游 main `50093e1720b2e76878ed9a9f989125f35e5e0629`；独立分支 `massif/ci-gates-plan`。实施前再次确认 main 未变、PR #47 仍未合并。没有覆盖 package.json、package-lock.json 或 runtime/run_all.sh。

## 实际变更

- `build.yml` 接入唯一范围/发布资格来源、完整回归、真实构建 Web 工件和 `CI Gate`；Release 直接依赖 changes/build/Gate。
- `tools/ci/check_profile.py`：只有白名单文档 PR 轻量；main、标签、published、手动运行完整检查。重命名同时考虑旧/新路径；diff 真正失败会失败。
- `tools/ci/check_gate.py`：所选重任务全部成功才通过；分类失败、缺失/非法输出、取消及意外跳过均拒绝。工作流最终步骤通过合法的 cancelled() 条件拒绝当时可观察到的取消；不回写已经完成的状态。
- `tools/ci/check_release_state.py`：复用原版本/说明校验；main 已有 Release 先正常跳过，再考虑新发布标签。标签不匹配拒绝；published 保留上传。查询错误不能当作不存在。该脚本只读，不创建 Release。
- `tools/ci/check_web_artifacts.py`：上传前核对 www.bin 与本次 flasher 映射；成品检查应有 gzip、解压内容及 JS 语法。
- `tests/prompts/xterm-offline.test.cjs`：资源读取/请求失败进入断言；保留真实终端交互、离线和退出重进。
- `tests/ci/test_web_resource_failure.cjs`：唯一浏览器负例，在隔离成品副本删除样式资源，要求原场景实际失败。子 Node 测试运行器清除继承的 NODE_TEST_CONTEXT，避免场景未执行的假通过。
- 证书入口支持 CI Python/临时路径/依赖；current-only 仍准备 restart 输入并执行当前停止、重启。仅主机编译启用 _DEFAULT_SOURCE；保留 ASan/UBSan。
- `tests/release/test_release.py` 保留 main 可发布预期，改为验证真实策略及新 Gate 接线，没有另写表达式解释器。

## 本地验证

| 命令/入口 | 结果 |
|---|---|
| `python3 -m unittest discover -s tests/ci -p 'test_*.py'` | 9 项通过；模拟 Release 响应，没有真实发布 |
| `python3 tests/release/test_release.py` | 3 项通过 |
| `actionlint -shellcheck= .github/workflows/build.yml` | 通过 |
| `npm run test:xterm --prefix tests/prompts`（OFFLINE_RESULTS 指向临时目录） | 4 项通过 |
| `bash tests/ws_subscriptions/run_host.sh`、`run_reviewer.sh`、`run_f1_f2.sh` | 三个当前入口全部通过；未重放历史基线 |
| `bash tests/certificate/run_host.sh`（匹配 SDK MbedTLS、CI 式 Python/临时路径） | subject/time_retry/material/UI/lifecycle/coordinator/API/time_cancel/当前 stop/restart 全部通过 |
| 匹配 ESP-IDF v5.5.2 的 `idf.py -B build/ci-candidate -DSDKCONFIG=.../build/ci-candidate/sdkconfig build` | 真实完整构建成功；没有修改默认配置或 version.txt |
| `check_web_artifacts.py --build-dir build/ci-candidate` | 本次 www.bin 与 flasher 映射通过 |
| `check_web_artifacts.py --web-root build/ci-candidate/esp-idf/ts_webui/web_optimized` | 13 个真实成品资源语法/gzip 通过 |
| `PROJECT_WEB_ROOT=.../web_optimized node --test tests/prompts/xterm-offline.test.cjs tests/ci/test_web_resource_failure.cjs` | 成品中英文正常场景 + 单个资源缺失负例，3 项通过 |
| `git diff --check` | 通过 |

本地为 macOS，Node 为 v24.10.0；工作流使用 Ubuntu、Node 22。Ubuntu 的最终结论以该 PR 最新提交 CI 为准。本地没有重复跑全部历史/浏览器矩阵；上游工作流按 v2 执行已有完整套件及新增入口。

## 发布与边界

上游版本仍为 0.6.1，其 Release 已存在。本 PR 无发布资格；如果以后经用户批准按该版本合并 main，完整 CI/Gate 成功后会记录“该版本已存在，本次未发布新资产”。后续尚未发布的合法版本继续自动发布，不要求版本 diff、提交消息版本或人工打标签。

未修改固件业务、HTTP 合同、GitHub 设置或版本；未自动合并、打标签、手动触发工作流、发布或刷机。不承诺真机运行，也不承诺阻止所有问题提交进入 main。尚未配置必需检查；以后仅在用户另行授权后配置准确名称 CI Gate。

Ubuntu 首次运行在新接入的 WebSocket 主机入口链接阶段报告 floor/libm 缺失。三个入口编译包含 ts_ws_subscriptions.c，因此补显式 -lm；不改业务或断言，ASan/UBSan 保留。修复后再次执行三个当前入口，并以最新提交 CI 验收。

没有产品/范围偏离，也没有需要用户新增决定的事项。运行取消用最终步骤条件实现（v2 允许）；actionlint 已验证这些条件的合法位置。
