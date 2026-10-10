# R1–R3 本地修复候选及验证证据

基线：`af6fcf262c38b7fc466dfb3e2180791d94f2f173`。独立分支：`fix/rule-pack-import-standalone`。
开始时本地 HEAD、个人仓库远端分支均为该基线，工作区干净；本次增量不包含他人修改。
最终提交身份以包含此文件的提交 SHA 为准，可直接比较基线与该提交。没有合并主线或改写历史。

## 闭合表

| 项目 | 修复前的实际路径 | 共同语义与改动入口 | 定向证据 | 保留的行为 |
|---|---|---|---|---|
| R1 | 删除 selector 已提交、回读未知；恢复后只从保存集合查差异，遗漏运行集合中的 A，仍可新触发且少算容量 | `ts_rule_engine.c` 的统一状态分类覆盖 IDs(A)∪IDs(S)；恢复完整读回后先发布 pending_delete，再清恢复状态；`execute_rule` 最终准入检查、普通 commit/包导入共用状态与并集容量；API/list/get 与双语列表、快捷卡片表达同一状态 | A1：未知→失败恢复→确认删除；只有一行，保存字段为 null；自动/手动拒绝，同 ID 普通写及包预览/正式导入拒绝；B 编辑不复活 A；重建启动 A 消失。A2：selector 写前失败及已知成功立即删除。A3：持有旧 lease/已准入动作不受破坏、最终准入屏障、小容量拒绝 C，重启后 C 可新增 | 无持久化 tombstone，无 GET 热删除；旧对象、租约、已准入快照保持；已知成功普通删除仍立即生效，待新增/待更新保持原行为 |
| R2 | 普通 API 启用 A 后，统一包级依赖检查因禁用模板拒绝整个普通集合；延迟入口还全局等待模板 | 按保存记录 source 只校验包；运行 meta 保留发布版本 source，保护扫描运行包与保存包；移除无条件模板等待，增加组件内有界加载结果；真实首次依赖初始化可进入，保留运行版本时不得破坏性重载 | B1：实际 enable→生产加载，A 启用/动作/source/revision 保留，B 可执行，A 因原有执行检查拒绝。B2：实际有上限延迟入口、无模板日志包、普通模板引用先加载后真实初始化 guard 放行。B3：混合集合包等待/失败时无部分发布，依赖就绪后完整加载；普通来源不被待重启包重标记；运行包阻止模板全量重载 | 普通 NVS/SD JSON 原有加载兼容；包来源仍严格，外部运行成功不由静态加载承诺；既有快照/服务保护保留 |
| R3 | 预检漏直接 SSH 和 Webhook；变更保护重复解析，忽略直接引用并仅检查回退 ID 存在 | `ts_rule_pack.c` 唯一只读有效动作/拟对象覆盖视图供预检和变更检查使用；模板一级优先；两条 SSH 统一实际 internal-first 主机解析；必要字段、加载/失败/缺失分开；getter 保留真实读取错误 | C1：真实包直接及模板 SSH 缺主机时预览/正式导入拒绝，存在时通过；直接路径不依赖命令就绪，模板 LOG 覆盖内联 SSH。C2：保存待重启与已运行两个时点，实际主机删除/坏更新拒绝；真实 getter 优先级、被遮蔽配置允许、内部删除后的有效/无效/未就绪/读取错误回退；真实内部注册/删除 guard 在检查失败时未调用写入；旧运行包与新保存包保护各自依赖。C3：直接/模板/禁用 Webhook 拒绝、保护模板不可改为 Webhook；普通旧 Webhook 仍加载；snapshot、pin、执行、连接、入队、变量写入计数不增加（C 组均为 0） | 没有调用 snapshot 做预检；没有连接或执行探测；没有实现 Webhook、增加 host enabled/auth 约束或改执行器 |

## 先红后绿

以下在基线生产代码、仅增加最小测试入口时采集；Python 包装器均退出 1（测试断言失败，原生进程 SIGABRT），不是编译失败。之后才修改生产语义。

`red-r1.log`：

```text
R1: saved_count=1 active_count=2 restart_pending=0
Assertion failed: (ts_rule_restart_pending()), function main, file test_engine_pack.c, line 89.
```

`red-r2.log`：

```text
R2: reload=259 loaded=0 count=2 template_enabled=0
Assertion failed: (loaded==ESP_OK&&s_rule_ctx.loaded&&s_rule_ctx.count==2), function main, file test_engine_pack.c, line 94.
```

`red-r3_ssh.log`：

```text
R3 r3_ssh: preview=0 reason=ok
Assertion failed: (got!=ESP_OK&&!strcmp(reason,webhook?"action_unsupported":"dependency_missing")), function main, file test_engine_pack.c, line 100.
```

`red-r3_webhook.log`：

```text
R3 r3_webhook: preview=0 reason=ok
Assertion failed: (got!=ESP_OK&&!strcmp(reason,webhook?"action_unsupported":"dependency_missing")), function main, file test_engine_pack.c, line 100.
```

最小反例命令：`python3 tests/runtime/test_rule_pack_crypto.py --engine --case=<r1|r2|r3_ssh|r3_webhook>`。
当前修复已扩展同一夹具为 A1–A3/B1–B3/C1–C3。该当前夹具包含新增 helper 的提取清单；复现历史红证据需使用当时最小夹具，不能将最终清单机械套到缺少 helper 的旧代码上。

## 最终验证

环境：macOS arm64，Apple Clang 21.0.0，Python 3.14.2，Node v24.10.0。
固定 SDK：ESP-IDF v5.5.2，`30aaf64524299d3bde422ca9a2848090d1bc5d0f`；使用 SDK 原生 Mbed TLS 3.6.5 和临时 OpenSSL PKI。
本地测试 CA PEM SHA-256：`a9c2a6c7b202c4acfa34b58e44d257505e5833e3fe70fcd24ad5aa19df4f7371`；没有上传测试私钥。

| 实际命令 | 退出码 | 结果与边界 |
|---|---:|---|
| `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf bash tests/runtime/run_all.sh` | 0 | 既有相关 runtime/codec/store/快照/动作提交/SSH/规则包与新增定向组全部通过；ASan/UBSan 原生夹具，硬件/RTOS/存储出口模拟 |
| `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf bash tests/runtime/run_rule_pack.sh` | 0 | 最后补齐模板正式导入及无副作用计数后复跑受影响共享夹具：795 保存、620 迁移、436 恢复故障场景；原包加密/信任/跨设备/CAS/幂等与 A/B/C 均通过 |
| `node --test tests/prompts/rule-pack.browser.test.cjs` | 0 | 20/20；实际双语页面+本地模拟 API，覆盖待删除单行、触发禁用、未知保存回读与 action_unsupported；非真机浏览器验收 |
| `CERT_MBEDTLS_BUILD=/tmp/tianshan-pack-mbedtls CERT_TEST_PYTHON=/Users/massif/.espressif/python_env/idf5.5_py3.14_env/bin/python IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf bash tests/certificate/run_host.sh` | 0 | 既有证书 subject/material/time/API/HTTPS 生命周期等回归通过；不等同于部署 PKI 验收 |
| `source /Users/massif/esp/v5.5.2/esp-idf/export.sh && idf.py -B /tmp/tianshan-rule-pack-firmware-build -DSDKCONFIG=/tmp/tianshan-rule-pack-sdkconfig build` | 0 | ESP32-S3、16MB 隔离构建，app 与 www.bin 成功；原生产配置/设备未修改。构建签名根为空，尚不能作为部署后导入验收证据 |
| `git diff --check` | 0 | 增量无空白错误 |

固件 `TianShanOS.bin` SHA-256：`7b2295d87fc60b1376cb3997c5089bc6f26c8c48bbd934daf6e00da77121c8ea`。
WebUI `www.bin` SHA-256：`85344da490c52dd1de75ac41dcabd6e1e3ecee8f513dd9758ee11beab9c19d25`。

原始输出保存在本机 `/tmp/tianshan-rule-pack-r1-r3-evidence/`；下面摘要哈希对应采集文件，临时目录可能被系统清理。真实 PASS 摘录与失败断言已纳入本报告，不上传凭证。

- `red-r1.log` SHA-256 `cd766716c0cd5e13032539a0046737dd119b06f99061cfe18d39de1c0ed6fc5f`
- `red-r2.log` SHA-256 `865be96c6f59c69c1e2c81ffa40c00ee3fe245a9afe0572ced228053ef525f2c`
- `red-r3_ssh.log` SHA-256 `a509289e12ffb31e44c382698e53f805a68cdb2cd2af9a78bf31bff9726e7d36`
- `red-r3_webhook.log` SHA-256 `b33a34cd3b04eb1df5fe9489cf8c40d4dae5c47d5c39e828681300b3ed877720`
- `runtime.log` SHA-256 `71576e556a5488dd9c8dc91c35282865453212c1a57c0901268e028758ac235a`
- `rule-pack-final.log` SHA-256 `4e18f9adda283011930a21cdf6a7172af7ec032ec9c55cdf2514fe1f9848e250`
- `browser.log` SHA-256 `99110bec0b9fd51a18b54891144a774eb20224791480dea5972dcb1b2aa8e9ce`
- `certificate.log` SHA-256 `3df50d403d90be2292a1f031fb3384dd9a4af027b0b23ffd29453ed0caa10082`
- `build.log` SHA-256 `05d8a435f50ac365b22938f6be8c376d76e5878b4beea514f585948e3877c828`

规则包最终真实输出：

```text
PASS real production create/trust/signature/ECDH/HKDF/GCM, cross-device PEM, offline accepted reload, malformed inputs
PASS unconfigured signing root refuses a cryptographically valid package
PASS production codec -> real pack -> complete-set store -> offline boot: 795 save, 620 migration and 436 recovery fault cases; full sources/revisions/readonly, idempotence, unrelated edit, SD absence and same-generation JSON mirror
PASS production import + engine + crypto + store: saved/active isolation, held old lease, pending edit/reload/deinit guards, unrelated merge, CAS/credential conflicts, offline idempotence/boot, dependency union, capacity and missing SD recovery
PASS A1-A3: confirmed deletion, unknown/old selector, unchanged active lease/admitted action, final admission barrier, union capacity, sibling edit and reboot
PASS B1-B3: actual enable/execute, ordinary source compatibility, bounded deferred loading, first template load, mixed complete publication and published source identity
PASS C1-C2: direct SSH preview/formal, real host source priority, proposed view/fallback fields and readiness, effective template, active/saved source protection; no snapshot/pin/execute
PASS C3: direct/template/disabled Webhook refused, protected template unchanged, ordinary compatibility, no snapshot/pin/execute side effects
```

## 实际修改文件

- `components/ts_api/src/ts_api_automation.c`
- `components/ts_automation/include/ts_action_manager.h`
- `components/ts_automation/include/ts_rule_pack.h`
- `components/ts_automation/src/ts_action_manager.c`
- `components/ts_automation/src/ts_rule_engine.c`
- `components/ts_automation/src/ts_rule_pack.c`
- `components/ts_webui/web/js/app.js`
- `components/ts_webui/web/js/lang/en-US.js`
- `components/ts_webui/web/js/lang/zh-CN.js`
- `tests/prompts/rule-pack.browser.test.cjs`
- `tests/runtime/extract_engine.py`
- `tests/runtime/rule_pack/stubs/ts_action_manager.h`
- `tests/runtime/rule_pack/test_engine_pack.c`
- `tests/runtime/rule_pack/test_store_set.c`
- `tests/runtime/rule_pack_contract.txt`
- `tests/runtime/run_rule_pack.sh`
- `tests/runtime/test_action_pack.py`
- `tests/runtime/test_rule_pack_crypto.py`
- `tests/runtime/rule_pack/R1-R3-review.md`

## 范围与未执行项

本轮只有 R1–R3 因果相关代码、既有夹具提取适配及双语 UI 联动，没有存储格式、selector、信任政策、执行器重写，没有实现 Webhook、操作设备、合并主线或覆盖其他 worktree。
没有新增需要另行修复的范围外问题。为保证读取错误不被伪装成主机不存在，仅联动无副作用 host getter 的错误透传。

未执行：刷机、物理重启、真机导入/SSH/服务/浏览器验收；真实设备签名根、Developer OU/KU/EKU、可信时间、NVS/SD 可用空间核验。没有仍失败或环境阻塞的本轮本地检查。

发布前提沿用原契约：必须安装并明确配置真实签名根，确认 Developer 证书用途、可信时间及足够存储空间。
**启动即可发生旧集合到 v3 的迁移，不以用户点击导入为前提。** 老版本不理解 `set_select`，不可承诺原地降级；需明确备份/恢复策略，不双写旧权威、不增加降级框架。

结论：本地修复候选及证据已完成，设备验收未完成。
