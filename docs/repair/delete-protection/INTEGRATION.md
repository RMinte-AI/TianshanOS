# PR #47：删除保护与规则包导入整合验证

2026-10-10。整合起点为 PR #47 的 `796b00ae0d9c2a4e7750af8d3fa0e06e29daf2f5`，
合入已审查的 `df540286a671840e03260e995a2a32da4927537c`（含原实现 `af6fcf26` 和 R1–R3 修复）。
用一个新增 merge commit 保留两边历史；该提交同时用于个人仓库 main 和现有 PR 来源分支。
上游 PR 保持未合并。原工作区及未跟踪文件没有作为整合输入，也未被覆盖。

## 冲突处理与必要联动

- 中英文 README 和头图保持 PR 原 HEAD 的完整字节，不恢复旧正文或旧图片。
- 普通保存同时保留“新增/增加缺失模板引用拒绝”的既有检查与 R1 的保存/运行状态检查。
- 模板删除先保留既有全部运行规则引用诊断，再检查已保存包依赖，最后走原服务保护/移除出口。
  待重启包引用同样阻止删除；无法取得可靠诊断时返回已有检查失败表达，不编造规则详情。
- 页面保留删除保护的首页加载/恢复/空状态及入口；待删除触发禁用、导入回读保护与双语错误保持。
- 共同 binding gate 改为递归 mutex，所有取得/释放匹配；新增同一 gate 的非阻塞 try 接口。
  edit gate 保持 binding→transaction 顺序，繁忙时立即拒绝，兼容 API/导入/加载/模板删除的同线程嵌套。
  没有调整网络执行器、服务状态政策、分区、完整集合 selector 或证书信任策略。
- 仅适配既有生产函数提取清单和 RTOS 测试边界：真实零超时使用 trylock，函数提取正确处理单行定义。
  POSIX 编译声明适配 Linux 严格 C11；不减弱断言、不更换旧 HTTP 字节 fixtures。

## 当前整合树验证

固定 ESP-IDF v5.5.2，macOS arm64，Apple Clang 21，原有 Node/Playwright 依赖。
临时 PKI 与 NVS/SD/RTOS/SSH 边界夹具不代表物理设备验收。

| 命令 | 最终退出码 | 结果 |
|---|---:|---|
| `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf bash tests/runtime/run_all.sh` | 0 | 全部原有 runtime、规则包 crypto/store/engine、A/B/C，以及删除保护 H01–H14 与 8 份真实 HTTP 字节通过；包含 1851 存储故障场景 |
| `python3 tests/runtime/test_delete_reference.py` | 0 | 实际生产 edit/commit/delete 路径，H01–H14、并发顺序、拒绝无写入、历史引用次数、8 份字节一致 |
| `bash tests/runtime/run_service_watch.sh` | 0 | 原服务回归及实际 production binding gate 的递归持有、跨线程立即拒绝、平衡释放通过 |
| `node --test tests/prompts/delete-protection.test.cjs tests/prompts/delete-reference.test.cjs tests/prompts/static.test.cjs` | 0 | 33/33 |
| `node --test tests/prompts/delete-protection.browser.test.cjs tests/prompts/delete-reference.browser.test.cjs tests/prompts/rule-pack.browser.test.cjs` | 0 | 58/58：38 原删除保护+20 规则包双语浏览器场景 |
| `source /Users/massif/esp/v5.5.2/esp-idf/export.sh && idf.py -B /tmp/tianshan-pr47-integration-build -DSDKCONFIG=/tmp/tianshan-pr47-integration-sdkconfig build` | 0 | 当前合并源码 ESP32-S3 app 与 SPIFFS 完整构建成功，隔离配置/输出 |
| `git diff --check` | 0 | 冲突标记清除、空白检查通过 |

规则包生产夹具追加了真实 `ts_action_template_remove_checked()` 的两种时点：保存待重启与实际运行。
拒绝时 template_remove 出口计数为零；运行版本的引用详情仍列出对应规则。

验证途中修正了旧提取夹具的单行函数边界、零超时模拟；首次 Chrome/SDK 运行受沙箱限制，
在授权环境重跑成功。表格报告最终状态，不把这些未成功的初次尝试当作通过证据。

构建在合并提交前、相同最终生产源码上进行，版本元数据仍取起点 HEAD；二进制不作为正式发布物。
`TianShanOS.bin` SHA-256：`6202b10e7b819e76b4eae05a1216688d8f11d17da9599387f91fb06f18c86768`。
`www.bin` SHA-256：`367866366f92f605f4cde0093faf6dcd1bdefe223e0568fdc2d24aa30681456f`。

本机原始日志摘要（临时目录可能被系统清理）：

- `/tmp/tianshan-pr47-integration-runtime-final.log` SHA-256 `8968ee765e6874ec0e9768163579870e06d7c67eebfa98efa0aa9cf92e2b8d8b`
- `/tmp/tianshan-pr47-integration-delete-final.log` SHA-256 `e9f9af9f719a3d6dc95b8a7f982831438b0dd8c2bb2c0291e11c65bf59419d96`
- `/tmp/tianshan-pr47-integration-binding.log` SHA-256 `207b853b27949387940567bd141d45aae0470036fa3991827042295097cd3a08`
- `/tmp/tianshan-pr47-integration-unit.log` SHA-256 `2c0c468a14087d60a89ffbbf371350b4de606fa7477e63c4ae76b32ed38427cb`
- `/tmp/tianshan-pr47-integration-browser.log` SHA-256 `b3c6ad34c9756c59d3c63f4cb097f42ade70b595382ceabd77fbbaa22e9727f3`
- `/tmp/tianshan-pr47-integration-build.log` SHA-256 `ffc8fa6627d65f327d706537341665aedc84c774e57c6e40c125a790c58b3e2d`

## 未执行与发布边界

未操作设备、刷机、重启、执行真实 SSH、合并上游 PR、打标签或发布 Release。
真实签名根、Developer 证书用途、可信时间、NVS/SD 空间及设备验收仍未完成。
启动即可发生旧集合到 v3 的迁移；不承诺旧固件原地降级。完整迁移/信任边界见
`tests/runtime/rule_pack_contract.txt`；R1–R3 历史红/绿证据见 `tests/runtime/rule_pack/R1-R3-review.md`。
