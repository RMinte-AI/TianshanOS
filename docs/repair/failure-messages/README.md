# 失败提示：统一目录与写法

用户点按钮、做操作失败时看到的提示，按同一套方式生成：**动作＋对象 → 原因 → 下一步**。
固件只返回一个固定的「原因代号」，网页按当前语言把它翻成句子。所有页面的操作失败提示都走这一套：
自动化、SSH、系统与服务、账户登录与改密、文件、网络与 WiFi、风扇、LED、证书与密钥、配置包、OTA 升级。

```
禁用规则“夜间关灯”失败：它正在执行中。等执行完成后再试。
└── 动作＋对象 ──────┘ └─ 原因 ─────┘ └─ 下一步 ───────┘
```

## 句子怎么拼（`js/api.js` 的 `describeFailure`）

| 部分 | 来源 | 说明 |
|---|---|---|
| 动作＋对象 | `failure.ops.<组>.<操作>.{failed,unknown,blocked,done}` | 每个按钮自己的，带对象名（规则名、主机地址） |
| 原因 | `failure.reasons.<代号>.why` | 一个分句，**不带句末标点** |
| 下一步 | `failure.reasons.<代号>.hint` | 完整一句，带句号；没有有用的下一步就不写 |
| 格式 | `failure.format` / `formatNoHint` / `formatUnknown` | 中英文各自的标点：`{action}：{why}。{hint}` / `{action}: {why}. {hint}` |

- **结果未确认**（原因代号 `result_unknown`、`commit_unknown`，或写操作抛异常没拿到设备回答，调用时传 `write: true`）：
  不写成功也不写失败，只说「…的结果未确认」＋下一步（不要重复提交），提示条用警告色。
- **代号查不到**：设备的英文原文**不会**出现在句子里，只进折叠的「详情」；句子按错误类别说
  （`busy`、`no_memory`、`not_found`…，都不认识就是 `no_detail`：「设备没有返回具体原因。可在「终端」页打开「系统日志」…」）。
- **缺参数**（原因文字里的 `{host}` 等调用方没给）：降级到按类别说，不会出现「说明暂时无法显示」。
- **旧固件**：只发英文原文的接口（如 `Rule not found`）有一张小对照表；其余按错误类别说。

## 固件怎么配合

- 失败时把原因代号放在 `error`（规则相关接口沿用 `message`）里，用小写蛇形：`execution_busy`、`ssh_auth_failed`。
  细节放 `data`（失败响应现在也带 `data`，见 `docs/repair/ssh-fingerprint/RESULT.md`）：
  `data.stage` 是失败发生的阶段，`data.detail` 是设备原文，只给「详情」用。
- 数字错误码仍然要给，选最接近的类别：旧网页只会按它说话。
- 目前接入的代号：规则保存/启停（引擎、存储、接口层的 `error_code`），手动触发被拒（`ts_rule_trigger_ex`），
  SSH 连接失败（`ts_ssh_get_fail_stage`）。

## 新增一条失败提示

1. 固件返回原因代号（需要时加 `data.detail` / `data.stage`）。
2. 语言包 `js/lang/zh-CN.js` 和 `en-US.js` 的 `failure.reasons.<代号>` 各加 `{ why, hint }`；操作名称加到 `failure.ops`。
3. 页面调用：`reportFailure({ op, params, result | error, endpoint, write })`；
   页面自己就能判断的拒绝（没有发请求）用 `phase: 'blocked'` 和 `token`。
4. 测试：在 `tests/runtime/test_failure_wire.py`（或 `test_ssh_fingerprint_wire.py`）里让生产代码生成一份线上字节，
   再在 `tests/prompts/failure-messages.browser.test.cjs` 里用它断言完整的句子。

`tests/prompts/failure-catalog.test.cjs` 会检查：中英文条目一一对应、占位符一致、不带机器代号、
中文里没有多余英文、英文里没有中文、原因无句末标点、下一步是完整句子；并且**从 C 源码读出固件会发出的每个原因代号**，
没有对应说法就失败。反过来，目录里没有任何地方会产生的条目也会失败。

## 页面里怎么用（`spec = { op, params, result | error, token, endpoint, write }`）

| 场景 | 调用 |
|---|---|
| 提示条（8 秒，带「详情」） | `reportFailure(spec)`；结果未确认时 `reportFailure(spec, { type: 'warning' })` |
| 一段内嵌文字（不带详情） | `failureText(spec)` |
| 内嵌 HTML（鼠标悬停显示详情） | `failureNote(spec)` |
| 弹窗里的结果框（带折叠详情） | `packBox(box, spec)` |
| 页面自己发现的问题（没有发请求，或设备的回答不能用） | `throw pageFailure('原因代号', { 占位符 })`，调用处照常 `reportFailure({ op, error })` |
| 要给出设备的原因 | 用 `requireApiSuccess(result, '接口名')` 抛错；**不要**写 `throw new Error(result.message)`：那会被当成「页面错误」，设备给的原因就丢了 |
| 浏览器拒绝写剪贴板 | `reportCopyFailure(e)` |
| 旧接口只发英文原文 | 在 `js/api.js` 的 `FAILURE_LEGACY_TEXT`（整句）或 `FAILURE_LEGACY_PREFIX`（开头）里加一条对照 |

`write: true` 表示这个操作会改变设备上的东西：拿不到设备回答时说「结果未确认」，而不是「失败」；读取、测试、校验传 `write: false`。

## 失败提示条（`showFailureToast` / `reportFailure`，`js/app.js`）

- 停留 8 秒；鼠标或键盘焦点在上面、或「详情」展开时暂停；× 可以直接关闭。
- 「详情」折叠：原因代号、接口、设备原文、阶段、时间。页面自己判断出的拒绝没有这些，也就没有「详情」。
- 普通的 `showToast` 不变；失败提示不会被随后的普通成功提示冲掉。

## 尚未验证

没有在真机上看过：SSH 失败阶段的判断（lwIP 的 `errno`）、手动触发被拒的各种原因，都只在主机上用生产代码加模拟件验证过。
