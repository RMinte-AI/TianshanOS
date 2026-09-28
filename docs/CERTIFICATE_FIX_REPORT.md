# 证书修复实施与验收报告

日期：2026-09-23。工作区 `/Users/massif/TianshanOS`，分支 `main`，HEAD
`d6ed947a592265fa12754828bc79803fc50c1db2`。本报告描述该 HEAD 上的**未提交补丁**，
不是已部署版本；没有 commit、push、PR、真实 CA 操作、设备连接、刷写或重启。

开始时仅 `output/`、`tmp/` 是已有未跟踪目录，保留未动。仓库中没有找到额外
AGENTS.md。已读取 README 构建说明及用户提供的完整修复方案。
实际 SDK 为 ESP-IDF 5.5.2、目标 ESP32-S3，SDK 内 mbedTLS 为 **3.6.5**（不是方案引用的
在线 3.6.1 文档版本）；未升级 SDK、修改分区、调整权限或关闭 mTLS 验证。

## 交付状态

- 完成安装长度/边界、请求局部错误、缓存与快照互斥、UTC、元数据缓存、材料版本、
  校时/材料通知、局部协调与有界重试、真实 TLS 运行态、待应用展示、自动注册适配。
- 宿主测试通过；采用真实 SDK mbedTLS/cJSON，平台功能使用替身，业务代码用 ASan/UBSan。
- ESP32-S3 固件和 WebUI 构建通过，实际命令及尺寸见后文。
- **实机 Web 安装、TLS 握手、重启恢复与资源测量未执行。不能据此宣称生产可发布。**
- **用户已批准的范围补充：**修正通用服务管理器吞掉停止错误的问题。
  `stop_service_internal()` 失败时返回原错误、恢复原状态、不发送停止成功事件；
  `ts_service_restart()` 也不会把运行中服务的停止失败当成可忽略的状态错误。
  `tests/certificate/test_service_stop.sh` 先从 HEAD 复现旧错误，再测试当前真实函数：
  一般失败、超时、状态错误均阻止重启；正常停止、重复停止与正常重启通过。
  共享回调已静态核查：正常返回成功的流程不变；返回失败的服务不再被假标为已停止。
  保留管理状态不代表能撤销其他模块内部的部分操作，其他服务实机故障未逐个注入。

## F1–F12 基线核对

所有问题在当前 HEAD 均仍存在，没有复用“已部署修复”的历史假设。

| 项目 | 基线核对 | 本轮处理 |
|---|---|---|
| F1 | 两个 Web 安装入口遗漏 NUL 长度 | 均改为实际内容长度 + 1 |
| F2 | 自动注册、CLI 已正确包含 NUL | 不再加一；全调用面核对 |
| F3 | 缺私钥与不匹配共用错误码 | `_ex` 请求局部错误；保留旧函数包装 |
| F4 | 页面忽略 error、PKI 翻译键错位 | 证书局部提取，键存在性回退，纯文本显示 |
| F5 | 浏览器校时没有通知 | 与 NTP 共用通知；失败不通知，丢事件可被兜底发现 |
| F6 | HTTPS 一次性 pending_init | 单一串行任务、两类事件、5 秒轻量检查 |
| F7 | 活动 HTTPS 复制材料 | 一致性拥有型快照；活动指纹与存储版本分开 |
| F8 | mktime、本地时区、未初始化 tm | 纯公历 UTC 算法；两个 HTTPS 时间消费者同步适配 |
| F9 | 读取缓存派生状态、刷新写 NVS | 静态元数据缓存；每次状态采样一次 now，派生状态不写 Flash |
| F10 | 32 字节序列号可溢出 serial[64] | serial[65]、有界编码、截断标志 |
| F11 | HTTPS 不是整套 WebUI | 验收端点仍为现有身份/权限 API |
| F12 | 通用 RUNNING 不代表实际监听 | cert.status 独立公开完整启动后的真实状态；停止失败正确传播，重启不继续 |

本次没有在修复前建立完整的红灯测试基线；上述是源码证据，不冒充修复前实机复现。
实施中故障测试曾发现“相同 PEM 幂等路径绕过损坏私钥检查”，已修复并保留回归用例。

## 文件与行为对应

| 文件 | 主要符号/用途 | 兼容边界 |
|---|---|---|
| `components/ts_cert/include/ts_cert.h` | 追加状态/validity/error、snapshot、generation | 原枚举 0..5 不变；原 esp_err_t 安装入口保留；新结构需全量重编译 |
| `components/ts_cert/src/ts_cert.c` | `install_material`、`info_from_crt`、`refresh_metadata`、`ts_cert_get_snapshot` | 原 NVS namespace/键不变；不把多键 set/erase 当事务 |
| `components/ts_cert/include/ts_cert_time.h`、`src/ts_cert_time.c` | `ts_cert_time_utc`、`ts_cert_time_days`、`ts_cert_hex` | 纯 C、无 TZ/全局时钟副作用；支持年份 1..9999 |
| `components/ts_api/src/ts_api_cert.c` | 两个 install、`add_validity`、`add_runtime` | 保留 code/error/data 协议、路径和权限；只增加字段 |
| `components/ts_core/ts_event/include/ts_event.h` | PKI 材料变更事件 | 小型 generation/kind 数据，不携带 PEM/指针 |
| `components/ts_net/src/ts_time_sync.c` | `notify_clock_updated`、UTC readiness | 继续使用原最小年份与原时间源策略，不声称时间经过认证 |
| `main/ts_services.c`、`main/ts_https_retry.h` | `https_coordinator`、串行启停、重试策略 | 不调用管理器重入自身；普通 GET/重复通知不重置预算 |
| `components/ts_core/ts_service/src/ts_service.c` | `stop_service_internal`、`ts_service_restart` | 用户批准的共享管理器最小修正；失败返回原错误，阻止假重启 |
| `components/ts_https/include/ts_https.h`、`src/ts_https.c` | runtime 快照、完整启动后发布、失败保留所有权 | 活动材料只在显式停止/重启后切换；非 mTLS 配置不强加 CA |
| `components/ts_https/src/ts_https_auth.c` | UTC 剩余天数 | 不改角色/权限策略 |
| `components/ts_pki_client/src/ts_pki_client.c` | 已有材料保留、安装返回值、协作取消 | 待校时/尚未生效不触发重新签发；缺 CA 不重新申请设备证书 |
| `components/ts_console/commands/ts_cmd_pki.c` | 新时间状态和容量提示 | 文件读取仍传 read_len+1；不再把所有无效状态写成 Expired |
| `components/ts_webui/web/js/app.js` | `certError`、`installCertMaterial`、`refreshCertStatus` | code===0 才成功；丢响应显示未确认，不自动重发 POST |
| `components/ts_webui/web/js/lang/{zh-CN,en-US}.js` | `pkiRepair` 文案 | 两种语言覆盖；缺新增字段的旧固件显示未知，不推断运行 |
| `components/{ts_cert,ts_api}/CMakeLists.txt` | 新源文件、API 私有 HTTPS 依赖 | ts_cert 不依赖 WebUI/API/main，无 SDK 变更 |
| `tests/certificate/` | 可运行宿主测试、临时夹具生成、生产函数提取 | 测试注入不进入生产 API |

锁顺序为：材料锁内读写缓存/RNG/NVS与元数据 → 解锁 → 发布事件。HTTPS 获取自己的
精确长度快照后才启动 TLS；不会持材料锁等待网络或服务管理器。CA 的小型 SD 副本写入
仍串行处理，写入/关闭失败只是警告，不改变 NVS 保存成功结论。

NVS set/commit 或多键操作异常后进入 `storage_error`，旧 RAM 材料不被冒充为新保存结果，
阻止新的 TLS 加载；请重启后读回并检查实际存储。既有活动实例可继续运行，其版本与
指纹仍来自自己的快照。这里没有断电跨键回滚保证，也不会自动清空 NVS。

长度契约仅适用于本轮 PEM 文本：已知跨度包含最后一个 NUL，跨度内无提前 NUL。
两个安装容量均为 4000 字节含 NUL；4096 缓冲宏未改。JSON 解码后 cJSON 的 C 字符串
不能保留原始 `\u0000` 后缀语义，本轮不声称 strlen 能检出该原始后缀。

## 调用者审查

| 调用来源 | 缓冲/长度 | 结论 |
|---|---|---|
| Web `api_cert_install` / `api_cert_install_ca` | cJSON 拥有的终止字符串；strlen+1 | 修正长度；底层再次检查边界 |
| CLI `cmd_pki_install` | malloc(file_size+1)，fread 后补 NUL；read_len+1 | 保持正确长度；文件内容上限 3999 |
| 自动批准 `ts_pki_client_submit_csr` | cJSON 字符串；strlen+1 | 长度原本正确；新增设备证书/CA 结果检查 |
| 下载 `ts_pki_client_install_certificate` | cJSON 字符串；strlen+1 | CA 失败或缺失不能报全流程完成 |
| 配置包签名者 `ts_config_pack` | PEM 文本；strlen+1 | 无需更改；没有改变配置包授权或用途规则 |
| `ts_cert_get_info` | 以前重复解析缓存；现在读取元数据 | 不暴露材料指针，不重复解析 |
| HTTPS `load_certificates` | 一次一致性快照，精确大小副本 | 替代三次独立读取；活动指纹取叶证书 DER SHA-256 |

搜索未发现 `ts_cert_info_t` 作为未经版本化二进制结构写入持久化存储。

## 实际执行证据

宿主命令：`./tests/certificate/run_host.sh`，退出码 0。结果：

```text
PASS UTC/serial bounds and bounded retry/control policy
PASS material: real SDK mbedTLS, mocked NVS/allocation/events, state/time/concurrent snapshots
PASS UI: error typing, bilingual/fallback, text output, uncertain POST, button recovery, refresh separation
PASS actual HTTPS lifecycle: init/start/URI/stop failures, retained ownership, duplicate start, loaded generation, optional CA
PASS actual coordinator with simulated scheduler: fallback, duplicate events, bounded retries, stop intent, explicit restart, stage errors
PASS actual cert API handlers: JSON validation, PEM installation, business errors, additive status, active/stored separation
PASS actual time notifications/UTC readiness and cooperative enrollment stop (syscalls/tasks mocked)
REPRODUCED manager stop bug: callback failure becomes success/stopped
PASS manager stop/restart: failed stop retained, restart aborted, successful/duplicate stop unchanged
```

Node `--check` 验证 app.js 与两种语言资源；`git diff --check` 验证补丁空白。
宿主测试使用 Command Line Tools 的 clang，应用代码开启 ASan/UBSan；SDK mbedTLS
宿主库没有 sanitizer 插桩，也未使用 ESP32 的硬件加速配置。

目标构建：

```sh
. /Users/massif/esp/v5.5.2/esp-idf/export.sh
export IDF_PYTHON_ENV_PATH=/Users/massif/.espressif/python_env/idf5.5_py3.12_env
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
./tools/build.sh
```

使用已有 Python 3.12 构建配置。最初 export 选择 3.14，与现有 CMake 配置不符；随后
改为命令局部的 3.12 环境。沙箱禁止 psutil 的 sysctl 进程读取，批准放开本地构建后继续。
没有 fullclean、重配目标、改变全局 PATH、修改 Xcode 许可或绕过 SDK 安全配置。

第一次编译发现 `_Atomic TaskHandle_t*` 不能传给 FreeRTOS 的普通输出参数，改为普通
句柄与原子运行/取消标记；后续目标编译通过。宿主曾发现相同 PEM 的私钥检查回归并修复。
这些失败不被隐藏为“第一次就全绿”。最终构建退出码 0，版本 `0.5.1+d6ed947d.09232301`（未提交源码的本地构建）。
Node v24.10.0，Apple clang 21.0.0。优化后的 app.js 通过语法检查，gzip 解压内容与
优化文件一致。应用分区 3,145,728 字节，剩余 988000 字节（约 31%）。

| 产物 | 原有构建产物字节 | 本轮字节 | 差值 | SHA-256 |
|---|---:|---:|---:|---|
| TianShanOS.bin | 2154096 | 2157728 | +3632 | `7f80532700f622abc7b46cbf2e7f314e35b0332a5aa3204fe6195404613ff0e5` |
| www.bin | 3145728 | 3145728 | +0 | `6ce320ce1f67ae4b35970dde8b74ce6e0f40c9e4e6b74d2c9056ca47ba4d3adc` |

此比较针对工作区原有二进制，未另建干净基线固件，因此差值不是独立性能测量。
本轮二进制和日志保留在 `/tmp/tianshan-certificate-tests/artifacts/`；仓库中原本受跟踪的
两个 build/*.bin 恢复为修改前内容，避免将生成文件混入源码补丁。它们不能被当作本轮
新固件使用；重新构建或使用上述临时验证产物，部署前仍须完成实机验收。

## 62 项验收映射

`HOST` 表示该行核心行为已在宿主执行；`PARTIAL` 表示仅覆盖其中明确列出的部分，
仍缺整项验收；`NOT_RUN` 表示未执行。即便 HOST，也不等于真实 ESP32/WebUI/TLS 验收。

| 编号 | 状态 | 实际测试 / 限制 |
|---|---|---|
| A01 | PARTIAL | `test_api` 正常设备 PEM、NVS/缓存；`test_material` 模拟重载；真实 Web/Flash 重启未测 |
| A02 | PARTIAL | `test_material` 拼接双证书 CA，真实 mbedTLS；真实 Web 未测 |
| A03 | PARTIAL | LF/CRLF/无末尾换行已跑；前端 trim；全部空白排列未穷举 |
| A04 | HOST | `test_api` 缺失/数字/空串；`test_material` 空白 |
| A05 | HOST | `test_material` 坏 PEM，原材料/版本不变 |
| A06 | HOST | `test_material` 合法 CA + 损坏 CA，全批拒绝 |
| A07 | HOST | `test_material` 缺私钥与模块未初始化区分 |
| A08 | HOST | `test_material` 另一 P-256 公钥拒绝，原材料不变 |
| A09 | HOST | `test_material` 损坏已存私钥；同 PEM 不绕过检查 |
| A10 | HOST | `test_material` 漏 NUL/提前 NUL/超长；ASan/UBSan |
| A11 | PARTIAL | 测 3999..4096 每个长度的边界/解析分流；未生成恰好 3999 字节有效证书验证真实 NVS 容量 |
| A12 | PARTIAL | 缓存分配失败与 mbedTLS 分配错误返回注入；不是穷举库内全部分配点 |
| A13 | HOST | 模拟 set/commit 失败；commit 失败后 mock 存储可已变化，缓存和成功事件不伪更新 |
| A14 | PARTIAL | 宿主无 SD 的路径正常安装；未模拟 SD 部分写及 close 失败 |
| A15 | HOST | 相同 PEM 的写入计数与版本不增 |
| A16 | HOST | `test_ui` POST 超时只提交一次，结果未确认；后端可能已写入的语义保留 |
| A17 | PARTIAL | 安装返回值路径已静态核对；未运行真实自动批准/下载服务器 |
| A18 | PARTIAL | `test_material` init 读取/分配失败与坏私钥；真实 NVS 损坏恢复未测 |
| B01 | HOST | UTC/CST-8/PST8PDT 同 epoch 结果一致 |
| B02 | PARTIAL | 算法不调用 TZ/mktime；PST8PDT 复核；未逐个枚举 DST 跳变 epoch |
| B03 | HOST | 2000/2024 闰日有效、2100 闰日无效 |
| B04 | PARTIAL | not_before-1 与端点已跑；+1 尚未独立列测 |
| B05 | PARTIAL | not_after 与 +1 已跑；-1 尚未独立列测 |
| B06 | HOST | 86399/0/-1/-86400/-86401 与 UI 不足一天 |
| B07 | HOST | 2050、9999 年 64 位结果 |
| B08 | HOST | 1970、time()=-1、阈值前时间未就绪 |
| B09 | HOST | `test_time_cancel` 阈值 UTC 前后一秒、三种 TZ |
| B10 | PARTIAL | 无证书、坏 PEM/私钥；纯 UTC 非法日期；未构造签名证书起止倒置夹具 |
| B11 | HOST | 解码后长度 0/1/20/31/32/33；终止、截断、保护字节与 sanitizer |
| B12 | HOST | 状态读取 1000 次 NVS set/commit 计数不变，再跨时间边界 |
| B13 | HOST | 实际生成 CSR 后读取 100 次保持 CSR_PENDING，无状态持久化 |
| B14 | PARTIAL | 自动注册的存在性条件与回调语义静态审查；未运行真实自动注册任务闭环 |
| C01 | PARTIAL | `test_coordinator` 单任务、单实例；实机启动顺序未测 |
| C02 | PARTIAL | 真实校时通知/材料前提分别测；协调器假前提→真前提；未实机组合 |
| C03 | PARTIAL | 同 C02；后安装材料的真实设备闭环未测 |
| C04 | PARTIAL | 底层 CA 安装不依赖私钥，条件统一；各种真实顺序未实机穷举 |
| C05 | HOST | 重复通知保留等待资格；创建任务/订阅计数不增 |
| C06 | HOST | 100 次重复通知/启动不重复 URI/实例 |
| C07 | HOST | init、默认端点、URI/TLS 启动故障；1/5/15 秒虚拟重试 |
| C08 | HOST | 四次失败后普通重复事件/时间推进不再重试 |
| C09 | HOST | 明确 stop/start 恢复；纯策略新 generation 重置预算 |
| C10 | HOST | 停止后大量旧通知与旧到期时间不能启动 |
| C11 | HOST | 模拟 TLS start 正在进行，另一线程 stop、材料变更；stop 返回后保持停止 |
| C12 | PARTIAL | 通知丢失不回滚保存；协调器无事件也检查前提；不是实机队列压力测试 |
| C13 | PARTIAL | not_before 派生与兜底分别测；真实无事件等待到日期未实机测 |
| C14 | PARTIAL | 活动 A/存储 B 的 API 版本与指纹分离；真实握手仍 A 未测 |
| C15 | PARTIAL | CA generation 更新和状态重读；真实 CA-only 握手变化未测 |
| C16 | PARTIAL | 模拟 stop/deinit/init 重新加载 generation；真实重启和 B 握手未测 |
| C17 | PARTIAL | 生成新密钥/删除凭证后已有快照仍自洽；真实会话未测 |
| C18 | PARTIAL | pthread 安装与状态/快照并发；不是所有 ESP 任务与 NVS/校时交错的穷举 |
| C19 | HOST | 快照取 B 后安装 A，持有的 B 副本和旧 generation 不变 |
| C20 | PARTIAL | TLS/协调器停止失败与半启动清理保留句柄；管理器停止/重启真实函数错误传播已测；设备用户入口未测 |
| C21 | PARTIAL | 真实取消函数等待超时不强删；材料锁并发测试；完整 HTTP 注册取消竞态未实机测 |
| C22 | HOST | `test_lifecycle` 非 mTLS 缺 CA 能启动；没有自动降级 |
| D01 | HOST | message/error/空白/布尔 true 类型选择 |
| D02 | HOST | 中英文实际资源与缺键回退；相邻 PKI 提示检查 |
| D03 | HOST | POST 成功/失败/超时/刷新抛异常均恢复按钮，刷新不改已保存结果 |
| D04 | PARTIAL | 旧字段缺失的有效期/HTTPS 显示未知；未与旧固件做真实浏览器组合 |
| D05 | HOST | HTML 字符串使用 textContent，不拼入 HTML；DOM 替身测试 |
| D06 | NOT_RUN | 没有真实 mTLS 设备/客户端，不能认定认证通过 |
| D07 | PARTIAL | 未修改角色/用途/权限策略；旧真实凭证/配置包端到端回归未跑 |
| D08 | PARTIAL | 测试凭证临时生成、不打印密钥；源码/产物变更审查；不是全站日志安全审计 |

## 资源与升级

- 新增一个 8192 字节内部 RAM 栈的协调任务、一个静态材料 mutex、两个保留订阅与少量元数据。
  这是配置申请量，不是实机测量的最终总消耗；TCB、分配器开销、库临时内存另计。
- 材料副本按实际长度优先 PSRAM，失败回退内部堆；错误检查后再交换。没有把 4KB/4KB
  PEM 固定数组放入事件栈。事件处理器只唤醒协调任务，不解析/TLS 启动。
- 当前材料的叶证书元数据、匹配结果、CA 解析结果和指纹被缓存。安装复用候选解析对象；
  GET 与 5 秒无变化兜底不重新解析、不写 NVS、不创建新 TLS 实例。
- 第一次瞬态失败后至多再试 3 次（1、5、15 秒）。缺前提不做快速重试；版本变化、
  明确重新启动、前提由假变真才重置预算。用户停止优先。
- 内部空闲堆/PSRAM、协调任务栈 high-water、失败前后资源趋势：**未实机测量**。
- 保留旧 NVS。固件与 WebUI 应成套更新；只升级一侧不算完整交付。新 UI 对旧固件新增
  字段缺失显示待确认；旧 UI 无法正确表达新状态，升级说明须提醒同时更新页面。
- 运行中更新证书或 CA 不自动断连接。重启设备或成功的显式服务重启后才尝试加载新材料。
  缺时间/CA、密钥不匹配、证书未生效/过期时 HTTP 管理路径保留，HTTPS 不新启动。
- 构建产物属于本地验证附件，不包含在本补丁中；没有改动用户原有 output/tmp 内容。

## 实机放行步骤（尚未执行）

1. 在明确授权的 ESP32-S3 测试设备记录旧版本、现有凭证及可恢复方案，不清空生产 NVS。
2. 更新固件与 WebUI。设备生成专用测试密钥/CSR，用隔离测试 CA 签发；不得拿本报告
   宿主生成的私钥替代设备 CSR 主流程。
3. 交叉执行先校时/后装、先装/后校时、CA/设备证书互换顺序；断开 NTP 后用安全页
   的浏览器校时按钮，验证失败能重试及设备时间/来源如实显示。
4. 从正常页面安装有效/损坏/不匹配/过期/未来证书，记录脱敏响应、保存内容与真实状态。
5. 正确信任 CA、SAN 地址、客户端证书/私钥下，验证现有 `/api/auth/whoami`：

   ```sh
   curl --fail-with-body --cacert ./test-server-trust.pem \
     --cert ./test-admin-cert.pem --key ./test-admin-key.pem \
     "https://${TEST_DEVICE_HOST}:${TEST_HTTPS_PORT}/api/auth/whoami"
   ```

   不使用 `-k/--insecure`。分别验证无客户端证书、非信任客户端证书被拒绝及现有角色权限。
6. 握手确认活动 A 的叶 DER SHA-256；存储 B 后握手仍 A、页面常驻待应用；重启后
   完整认证握手确认 B。只换 CA 时也核对 configured/loaded 版本及客户端信任变化。
7. 测试停止与重启、重试期间停止、删除/换钥期间活动会话；停止失败须从真正用户入口
   保留错误，不能只测底层函数。管理器宿主回归已通过，仍需设备入口验证。
8. 记录内部堆/PSRAM、任务栈余量、重复故障后的趋势和实际镜像版本。所有未测项继续
   标为未验证，不能因页面显示有效或编译通过而取消。
