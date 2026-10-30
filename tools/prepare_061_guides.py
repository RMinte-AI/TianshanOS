"""Import the previous edition once and apply the reviewed 0.6.1 revisions.

The four resulting Markdown files are the editable source of the new edition.
PDF builds read them directly; they do not rerun this import.
"""
from pathlib import Path
import re

BASE = Path(__file__).resolve().parents[1]
DEST = BASE / 'docs/user-guides/0.6.1'
PREVIOUS = Path('/Users/massif/rminte-website/website/guides/content')

def section(text, title, body):
    pattern = r'^' + re.escape(title) + r'\n.*?(?=^#{1,' + str(len(title.split()[0])) + r'} |\Z)'
    text, count = re.subn(pattern, lambda m: title + '\n\n' + body.strip() + '\n\n', text, flags=re.M | re.S)
    assert count == 1, (title, count)
    return text

def replace(text, old, new):
    assert old in text, old
    return text.replace(old, new)

def main():
    assert not (DEST / 'shared.zh.md').exists(), 'Editable sources already exist; edit them directly.'
    admin = (PREVIOUS / 'admin.md').read_text()
    root = (PREVIOUS / 'root.md').read_text()
    az, ae = re.split(r'^# TianshanOS Admin User Guide\s*$', admin, flags=re.M)
    rz, re_ = re.split(r'^# TianshanOS root Operations Guide\s*$', root, flags=re.M)
    az = az[az.index('## 1. '):].strip()
    ae = ae[ae.index('## 1. '):].strip()
    rz = rz[rz.index('## 10. '):].strip()
    re_ = re_[re_.index('## 10. '):].strip()

    az = section(az, '### 查看资源与服务状态', '''“带外管理芯片资源监控”显示管理芯片的 CPU、DRAM 和 PSRAM 使用情况。这些读数不代表 AGX 或 LPMU 的资源使用率。

- 点击“详情”，查看内存总量、已用和空闲空间等信息。
- “服务”按钮显示正在运行的服务数和服务总数。点击后可查看每项服务的状态及所属阶段。
- 服务状态异常时，先刷新确认，再按需要重启对应服务。''')
    ae = section(ae, '### View Resource and Service Status', '''“Management Chip Resources” shows CPU, DRAM, and PSRAM usage on the management chip. These readings do not show resource usage on AGX or LPMU.

- Select “Details” to view total, used, and free memory and other memory information.
- “Services” shows the number of running services and the total. Select it to view each service’s state and initialization stage.
- If a service reports a problem, refresh its status before deciding whether to restart it.''')
    az = section(az, '### 查看系统与电源信息', '''“系统总览”包含芯片、固件版本、ESP-IDF 版本和运行时间，同一卡片右侧显示输入电压、内部电压、电流、功率及保护状态。ESP-IDF 是设备所用系统框架的版本，日常操作通常只需核对固件版本。

保护状态旁的开关用于启用或停用低电压保护。启用后，系统按已保存的阈值和等待时间执行低电压关机与供电恢复流程。''')
    ae = section(ae, '### View System and Power Information', '''“System Overview” shows the chip, firmware version, ESP-IDF version, and uptime. The same card also shows input voltage, internal voltage, current, power, and protection status. ESP-IDF identifies the system framework used by the device; for routine use, the firmware version is usually the one to check.

The switch beside the protection status enables or disables low-voltage protection. When enabled, the device uses its saved voltage thresholds and delays to manage shutdown and recovery.''')
    az = replace(az, '操作完成后重新查看状态和健康信息。', '操作完成后重新查看服务状态。')
    ae = replace(ae, 'Check its status and health again when the operation finishes.', 'Check its status again when the operation finishes.')
    az = section(az, '### 控制 AGX 和 LPMU', '''关闭模组前，先保存其中的工作并完成正常关机流程。强制断电可能造成未保存的数据丢失。

- “AGX 电源”显示电源控制状态。点击按钮可上电或断电；操作后等待页面确认。
- “LPMU 电源”显示“在线”“离线”或“未知”。这些状态来自网络检测：在线表示网络可达，离线表示网络不可达，都不能单独确认模组是否已开机或关机。
- 点击 LPMU 按钮会触发一次与物理电源按钮相同的操作。随后等待检测和页面提示；结果未知时，先检查模组和网络，不要连续点击。''')
    ae = section(ae, '### Control AGX and LPMU', '''Save work on the module and complete its normal shutdown process before removing power. Forced power-off can cause loss of unsaved data.

- “AGX Power” shows the power-control state. Select it to switch power on or off, then wait for confirmation.
- “LPMU Power” shows “Online,” “Offline,” or “Unknown.” These states come from a network check. Online means the module is reachable; offline means it is not. Neither state alone confirms whether the module is powered on or off.
- Selecting the LPMU button triggers the same action as pressing its physical power button. Wait for the check and the displayed result. If the result is unknown, check the module and network before pressing the button again.''')
    az = section(az, '### 使用快捷操作', '''设备面板中的快捷操作卡片由 root 用户配置。卡片是否显示、是否允许手动运行分别设置；出现卡片不代表它当前一定能启动。

1. 查看卡片名称、状态和可用按钮，确认目标任务。
2. 点击可执行的卡片，等待本次操作的状态更新。处理中不要再次启动。
3. 后台任务可能提供日志、核验状态或停止按钮。查看日志用于读取输出；停止操作后，还需要确认任务确实已停止。
4. 状态未知或启动被阻止时，先使用页面提供的核验功能，或请 root 用户检查规则、远程指令和主机连接。

需要重新启动服务时，先确认它已停止。启动、停止请求被接受后，任务仍可能处于处理中；以最终状态为准。触发一个操作后，等待几秒再启动其他操作。

长按卡片，出现排序提示后拖动，可以调整显示顺序。没有卡片时，请 root 用户检查规则的“显示在面板”设置。''')
    ae = section(ae, '### Use Quick Actions', '''Quick Action cards in the Device Panel are configured by a root user. Showing a card and allowing manual execution are separate settings. A visible card may not be available to start.

1. Check the card’s name, state, and available controls to identify the task.
2. Select an available card and wait for its state to update. Do not start it again while it is processing.
3. Background tasks may provide log, status-verification, or stop controls. Logs show output. After requesting a stop, check that the task has actually stopped.
4. If the state is unknown or starting is blocked, use the available verification control, or ask a root user to check the rule, remote command, and host connection.

Confirm that a service has stopped before restarting it. An accepted start or stop request may still be in progress; wait for the final state. After triggering one action, wait a few seconds before starting another.

Press and hold a card until the reorder indicator appears, then drag it to change the display order. If no cards are shown, ask a root user to check the rules’ “Show on panel” setting.''')

    fan_z = '''“风扇控制”位于“系统”页面。风扇卡片显示当前设备提供的风扇；曲线设置中请核对要操作的风扇编号。

### 查看风扇状态

顶部显示“有效温度”和“当前输出”。每张风扇卡片的大百分比表示设备当前确认下发的调速值，RPM 表示测得的每分钟转数，两者含义不同。没有有效 RPM 时不显示该读数；大百分比出现 `--` 时，当前输出尚未确认。

拖动手动滑块时，滑块旁显示准备设置的数值，大百分比仍保留当前输出。松开滑块后，查看页面提示和更新后的读数，确认是否设置成功。“请求设置”与当前输出不一致时，不要只凭请求值判断风扇已经改变。

智能模式可能显示“智能模式”“按曲线运行”“保护介入”或“温度失效”，并提供“安全参考”“45 秒预测”和“升温速度”。出现温度失效时，先检查温度来源是否持续更新；设备会按温度失效保护策略调速。点击“TTI”可查看智能热控说明。

点击区域顶部的刷新按钮，重新获取状态。出现“未能完成风扇调节”或输出未确认时，检查设备状态后再决定是否重试。

### 选择运行模式

| 模式 | 作用 |
| --- | --- |
| “关闭” | 停止风扇。 |
| “手动” | 用滑块设置固定的调速百分比，范围为 0-100%。 |
| “曲线” | 按已设置的温度与调速百分比对应关系运行。 |
| “智能” | 在基础曲线上结合温度趋势和调速历史调整散热，并按保护状态采取相应策略。 |

关闭风扇或设置过低的手动值会降低散热能力。执行前确认设备负载和温度，并持续观察。只有在手动模式且当前输出已确认时，滑块才可调节。

### 设置风扇曲线

点击“风扇控制”区域顶部的“曲线”，打开“风扇曲线管理”。卡片内的“曲线”只切换运行模式。

1. 在“风扇”中选择要调整的编号，并与系统页卡片核对。
2. 在“温度变量绑定”中点击“添加变量”，选择温度来源并填写权重。
3. 点击“绑定”。绑定的温度来源由曲线、智能模式的风扇共用，修改会影响使用该来源的风扇。
4. 在“曲线节点”中添加或编辑节点。每条曲线至少需要 2 个节点，最多支持 10 个。
5. 设置“最小转速”和“最大转速”。这里填写的是调速百分比，最小值不能高于最大值。
6. 根据需要设置“温度回差”和“最小调节间隔”。回差允许 0-20°C，用于减少小幅温度波动引起的频繁调节；间隔允许 500-30000 ms，1000 ms 等于 1 秒。
7. 点击“保存曲线”。成功后，设置被保存，所选风扇切换到曲线模式。若希望继续使用智能热控，返回风扇卡片点击“智能”。

保存失败时，先查看具体提示并检查当前状态，再决定是否重试。解除温度绑定后，也应检查有效温度和风扇状态。

### 导入和导出曲线

- 点击“导入配置”，选择曲线 JSON 文件。JSON 是保存曲线数据的文件格式。检查载入的节点、限制和风扇编号，再点击“保存曲线”使其生效。
- 点击“导出配置”，浏览器会下载当前曲线，并尝试将副本保存到 SD 卡的 `/sdcard/config` 目录。分别检查浏览器下载结果和页面提示的 SD 卡保存结果。

### 使用测试温度

测试温度会临时替代正常温度来源，并影响曲线或智能调速。测试期间持续观察风扇和设备状态。

1. 在“测试温度”中输入 0-100°C。
2. 点击“测试”，观察当前输出、状态和风扇响应。
3. 测试结束后点击“清除测试”，恢复正常温度来源，并确认有效温度已恢复。'''
    fan_e = '''“Fan Control” is on the “System” page. Fan cards show the fans provided by your device. Check the fan number before changing a curve.

### View Fan Status

The status bar shows “Effective Temp” and “Current output.” The large percentage on each card is the control value the device confirms it has applied. RPM is the measured speed in revolutions per minute; it is a different reading. RPM is hidden when no valid reading is available. A large `--` means the current output has not been confirmed.

While you drag the manual slider, the value beside it shows the proposed setting. The large percentage continues to show the current output. Release the slider, then check the message and updated reading to confirm the change. If a requested setting differs from the current output, do not assume it has already taken effect.

Smart mode may show “Smart Mode,” “Following Curve,” protection, or an invalid temperature state, together with a safety reference, a 45-second forecast, and the rate of temperature change. If the temperature becomes invalid, check that its source is still updating. The device uses its temperature-loss protection policy. Select “TTI” for an explanation of Smart thermal control.

Use the refresh button in the section header to fetch the current state. If adjustment fails or the output is unconfirmed, check the device before deciding whether to retry.

### Select an Operating Mode

| Mode | Purpose |
| --- | --- |
| “Off” | Stops the fan. |
| “Manual” | Uses a fixed control percentage set with the 0-100% slider. |
| “Curve” | Follows the configured temperature-to-control-percentage curve. |
| “Smart” | Uses temperature trends and adjustment history to tune cooling above the base curve, with separate policies for protection states. |

Stopping a fan or setting a low manual value reduces cooling. Check the load and temperature first, and keep monitoring them. The slider is available only in Manual mode when the current output is confirmed.

### Configure a Fan Curve

Select “Curve” in the Fan Control section header to open “Fan Curve Management.” The “Curve” button inside a card only changes the operating mode.

1. Under “Fan,” choose the number to adjust and check it against the System page.
2. Under “Temperature variable binding,” add temperature variables and assign weights.
3. Select “Bind.” The temperature source is shared by fans in Curve and Smart modes, so changing it affects the fans using that source.
4. Add or edit “Curve nodes.” Each curve needs at least 2 nodes and supports up to 10.
5. Set “Minimum speed” and “Maximum speed” as control percentages. The minimum must not exceed the maximum.
6. Set “Temperature hysteresis” and “Minimum interval” as needed. Hysteresis accepts 0-20°C and reduces adjustments caused by small temperature changes. The interval accepts 500-30000 ms; 1000 ms is 1 second.
7. Select “Save curve.” After a successful save, the selected fan switches to Curve mode. To use Smart thermal control, return to the card and select “Smart.”

If saving fails, read the message and check the current state before retrying. After unbinding temperature variables, check the effective temperature and fan state as well.

### Import and Export a Curve

- Select “Import Config” and choose a curve JSON file. JSON is the file format used to store curve data. Review the nodes, limits, and fan number, then select “Save curve” to apply them.
- Select “Export Config” to download the current curve. The page also attempts to save a copy under `/sdcard/config` on the SD card. Check the browser download and the reported SD-card result separately.

### Use a Test Temperature

A test temperature temporarily replaces the normal source and affects Curve or Smart control. Monitor the fan and device throughout the test.

1. Enter 0-100°C under “Test Temp.”
2. Select “Test” and observe the current output, state, and fan response.
3. Select “Clear Test” when finished. Check that the effective temperature has returned to its normal source.'''
    az = section(az, '## 4. 风扇管理', fan_z)
    ae = section(ae, '## 4. Fan Management', fan_e)
    az = section(az, '## 5. LED 管理', '''“LED 控制”位于“系统”页面，只显示设备提供的 LED。不同 LED 可用的功能不同。

### 常用控制

- 使用卡片底部的灯泡按钮开启或关闭 LED。
- 拖动“亮度”滑块调整亮度，或选择颜色和预设色。
- 点击动画按钮进入“LED 设置”，在“程序动画”中选择动画并使用对应的播放或停止按钮。
- 点击卡片右下角的保存图标，保存当前 LED 配置。
- 点击“全部关闭”关闭所有 LED，并查看页面报告的结果。部分关闭失败时，检查对应设备。

### LED 矩阵高级功能

矩阵设备的卡片提供相应功能图标，打开后可在“LED 设置”中切换分组：

- “程序动画”：选择和运行动画。
- “图像/QR码”：从 SD 卡选择图像，或输入内容生成 QR 码。
- “文本显示”：填写文字，并设置字体、对齐、滚动、前景色和背景。
- “后处理滤镜”：选择滤镜及参数，再应用或停止。
- “色彩校正”：调整显示效果，并按页面按钮重置、导入或导出设置。

完成操作后检查实际灯光或矩阵画面。没有显示的功能无需操作；设置失败时，按页面提示检查文件、输入和设备状态。''')
    ae = section(ae, '## 5. LED Management', '''“LED Control” is on the “System” page and shows the LEDs provided by the device. Available features vary by LED.

### Routine Controls

- Use the light-bulb button at the bottom of a card to turn the LED on or off.
- Move the “Brightness” slider, or choose a color or preset.
- Use the animation button to open “LED Settings.” Select an animation under “Programmatic Animation,” then use its play or stop control.
- Select the save icon at the bottom right of the card to save the current LED configuration.
- Select “All Off” to turn off all LEDs and check the reported result. If some fail, check those devices.

### Advanced LED Matrix Features

Matrix cards provide icons for the available features. Open one, then switch between these groups in “LED Settings”:

- “Programmatic Animation”: Select and run an animation.
- “Image/QR Code”: Choose an image from the SD card or enter content for a QR code.
- “Text Display”: Set text, font, alignment, scrolling, foreground, and background.
- “Post-processing Filters”: Choose a filter and parameters, then apply or stop it.
- “Color Correction”: Adjust the display and use the available reset, import, or export controls.

Check the actual light or matrix display after applying a change. Use only the features shown. If a setting fails, follow the message to check the file, input, or device state.''')
    az = section(az, '#### 通过 LPMU 接入上层网络', '''此操作通过已配置的 LPMU 主机执行网络接入。开始前，请确认 LPMU 可连接、相关网络已接好，并取得该主机上 SSH 账号的 sudo 密码。sudo 密码用于允许账号执行系统配置，可能与 WebUI 登录密码不同。

1. 在“接入上层网络”区域点击“通过 LPMU 接入”。
2. 在弹窗中输入“LPMU sudo 密码”，再点击接入按钮。密码仅用于本次操作，不保存；下次执行需要重新输入。
3. 等待处理结束，不要重复启动。
4. 结果窗口显示互联网连接正常和网络配置已完成时，才表示接入成功。
5. 失败或结果未确认时，先阅读具体原因；需要更多信息时展开“执行日志”。密码错误时核对密码，找不到互联网连接时检查网络接线与上层网络。

状态获取失败不等于脚本已经停止。先点击页面提供的“刷新状态”确认当前执行情况，再决定是否重新接入。''')
    ae = section(ae, '#### Access the Upstream Network via LPMU', '''This operation uses the configured LPMU host to set up network access. Before starting, confirm that LPMU is reachable and the network cables are connected. Obtain the sudo password for the SSH account on that host. This password authorizes system changes and may differ from the WebUI password.

1. Under “Access Upstream Network,” select “Access via LPMU.”
2. Enter the LPMU sudo password and select the access button. The password is used only for this run and is not saved; enter it again for a later run.
3. Wait for the operation to finish. Do not start it again while processing.
4. Access is confirmed only when the result reports both an internet connection and completed network configuration.
5. If it fails or the result is unconfirmed, read the reason first. Expand “Execution Log” for more detail. Check the password for a password error, or the cabling and upstream network if no internet connection is found.

A failed status request does not mean the script has stopped. Use the available “Refresh Status” control to check the current run before deciding whether to try again.''')
    az = replace(az, '从 URL 升级时，“跳过验证”', '从 URL 升级时，“跳过证书验证”')
    ae = replace(ae, '“Skip Verify” skips verification', '“Skip Certificate Verify” skips verification')
    az = replace(az, '升级同时包含 WebUI 时，固件和 WebUI 会依次更新。两者应来自同一发布版本。', '升级同时包含 WebUI 时，固件和 WebUI 会依次更新。升级到 0.6.1 时，请按该版本发布说明同时更新主固件和 WebUI 资源，两者应来自同一发布版本。升级后仍显示旧界面时，尝试强制刷新浏览器，再核对版本和页面。')
    ae = replace(ae, 'When “Include WebUI” is enabled, the firmware and WebUI are updated in sequence. Both should come from the same release.', 'When “Include WebUI” is enabled, the firmware and WebUI are updated in sequence. For 0.6.1, update both the main firmware and WebUI resources as directed by the release notes, using files from the same release. If the old interface remains after upgrading, force-refresh the browser and check the version and page again.')
    az = replace(az, '上传 `.tscfg` 配置包时，页面会进入配置包校验和应用流程。', '上传 `.tscfg` 配置包时，页面会进入配置包校验和应用流程。文件上传成功不等于配置已经应用，请继续检查校验、确认及应用结果。')
    ae = replace(ae, 'Uploading a `.tscfg` configuration package starts its verification and application flow.', 'Uploading a `.tscfg` configuration package starts the validation and application flow. A successful upload does not mean the configuration has been applied; check the validation, confirmation, and application result.')
    az += '\n\n### 遇到失败或结果未确认\n\n先阅读具体提示，再检查目标设备或文件的当前状态。超时或连接中断时，操作可能已经执行；不要立即重复断电、删除、启动任务或升级。批量操作应分别检查成功、失败和未确认的项目，浏览器下载还应在下载列表中确认文件已保存。\n'
    ae += '\n\n### Failed or Unconfirmed Operations\n\nRead the specific message, then check the current device or file state. An operation may have run even if the response timed out or the connection was lost. Check before repeating power, delete, task-start, or upgrade operations. For batch actions, review successful, failed, and unconfirmed items separately. Confirm downloaded files in the browser’s download list.\n'

    # Root-only editorial changes retain the existing terminal/SSH coverage.
    for old, new in [('“成功匹配模式”', '“期望匹配”'), ('“失败匹配模式”', '“失败匹配”'), ('“提取模式”', '“提取正则”'), ('“匹配后自动停止”', '“命中即停止”'), ('“存储变量名”', '“变量名”'), ('“后台执行（nohup）”', '“后台运行 (nohup)”'), ('“服务模式（监测就绪状态）”', '“服务模式”'), ('“就绪匹配模式”', '“就绪匹配”')]: rz = rz.replace(old, new)
    for old, new in [('“Success Pattern”', '“Expected match”'), ('“Fail Pattern”', '“Fail Match”'), ('“Extract Pattern”', '“Extract regex”'), ('“Stop on Match”', '“Stop on match”'), ('“Background (nohup)”', '“Run in background (nohup)”'), ('“Service Mode (Monitor Ready State)”', '“Service Mode”'), ('“Ready Pattern”', '“Ready match”')]: re_ = re_.replace(old, new)
    rz = replace(rz, '3. 页面显示“取消”时，可以终止当前仍在执行的会话。', '3. 页面显示“取消”时，可以请求中断当前执行会话。随后查看状态和输出，确认是否已停止；已完成的远程操作不会撤销。')
    re_ = replace(re_, '3. When “Cancel” is shown, use it to end the current execution session.', '3. When “Cancel” is shown, use it to request interruption of the current session. Check the state and output to confirm whether it stopped. Completed remote operations are not undone.')
    rz = replace(rz, '删除指令前，应确认它没有被自动化数据源或动作模板引用。删除操作无法从指令页面撤销。', '删除指令前，检查数据源、动作模板和规则是否引用它。绑定的服务仍在运行、状态未知或正在处理操作时，删除可能被阻止；先核验或停止服务，确认停止后再删除。删除无法从指令页面撤销。')
    re_ = replace(re_, 'Before deleting a command, confirm that no Automation data source or action template references it. Deletion cannot be undone on the Commands page.', 'Before deleting a command, check its data-source, action-template, and rule references. Deletion may be blocked while a bound service is running, its state is unknown, or an operation is in progress. Verify or stop the service and confirm it has stopped before deleting. Deletion cannot be undone from the Commands page.')
    rz += ''
    rz = replace(rz, '停止跟踪和停止进程含义不同。结束查看时使用“停止跟踪”；确认远程任务可以终止后再使用“停止进程”。', '停止跟踪只结束页面的日志刷新，不停止远程进程。“停止进程”用于请求结束后台任务；操作后检查进程状态。服务模式任务还可能显示启动中、核验中、停止中或结果未确认。请求被接受后继续等待最终状态；状态未知时先核验，确认已停止后再启动。')
    re_ = replace(re_, 'Stop Tail and Stop Process have different effects. Use “Stop Tail” when you only want to stop viewing updates. Use “Stop Process” only after confirming that the remote task can be ended.', '“Stop Tail” only stops log updates in the page. “Stop Process” requests termination of the background task; check the process state afterward. Service-mode tasks may be starting, being verified, stopping, or unconfirmed. An accepted request is not a final result. If the state is unknown, verify it; confirm the task has stopped before starting it again.')

    auto_z = '''“自动化”把数据和操作连接成可重复执行的流程。先建立数据源和动作模板，再用规则决定何时执行。

```text
自动执行：数据源 → 变量 → 规则判断 → 执行动作
手动执行：系统页快捷操作 → 执行动作
```

数据源读取数据，变量保存值，规则判断条件，动作模板定义任务。规则是否显示在设备面板、是否允许手动触发分别设置；自动规则也可以显示在面板中。

### 查看和控制自动化引擎

页面顶部显示引擎状态、规则数、变量数、数据源数、触发次数和运行时长。

| 操作 | 影响 |
| --- | --- |
| “启动” | 启动已停止的引擎，开始处理已启用规则。 |
| “暂停” | 暂停后续自动判断，已有动作可能继续。需要恢复时先停止，再启动。 |
| “停止” | 尝试停止规则处理和数据源读取，结束后续动作调度；配置仍保留。等待操作确认，超时表示停止尚未完成。 |
| “重载” | 重新读取已保存配置；原本运行的引擎加载后恢复运行。先保存编辑内容。 |

操作前检查是否影响散热、告警或其他持续任务。停止引擎不能代替停止远程后台进程；远程任务需要在“指令”或对应服务卡片中停止并确认。

### 完成一条最小自动化流程

1. 创建数据源，用测试功能确认连接及需要读取的字段。
2. 启用数据源，点击该行的“查看变量”图标，检查数值和更新时间。
3. 创建动作模板，检查参数后点击“测试”。测试会立即执行动作，先确认设备和远程主机允许本次操作。
4. 创建规则，暂时关闭“立即启用”，设置条件、冷却时间、动作顺序、延迟和重复方式。
5. 按需要设置“显示在面板”和“允许手动触发”。
6. 保存后再启用，观察变量、触发次数及实际结果。

### 管理数据源

点击“数据源”区域的“添加”，选择类型：

| 类型 | 用途 |
| --- | --- |
| “REST API” | 定期读取一个 HTTP 地址返回的数据。 |
| “WebSocket” | 接收持续连接推送的数据。 |
| “Socket.IO” | 接收 Socket.IO 服务的事件数据。 |
| “指令变量” | 读取已配置远程指令的结果。 |

1. 填写唯一 ID、显示标签和该类型要求的连接信息。
2. 使用测试功能确认连接。网络地址、认证和字段以数据提供方的信息为准。
3. 对前三种类型，从测试结果选择需要的数据字段。Socket.IO 事件名称留空时，测试可尝试发现事件。
4. 对“指令变量”，选择主机及已经配置变量名的指令，再设置检测间隔。
5. 保存、启用，然后用该行的“查看变量”检查结果。

数据源行提供启用开关、查看变量、导出和删除。没有独立的主页面变量搜索区。停用或删除数据源会影响依赖它的规则；删除前检查引用。

导入和导出使用配置包。导入前预览 ID、类型及覆盖内容，按安全指南检查来源和信任要求。

### 查看变量

点击数据源行的眼睛图标“查看变量”，在弹窗中查看名称、类型、值和更新时间。远程指令也可从其变量入口查看结果。

- 确认名称和数据源正确，值与类型适合规则比较。
- 检查更新时间是否符合预期。窗口中的时间用于判断数据是否仍在更新。
- 没有数据时，检查数据源是否启用，再测试连接和字段选择；远程指令变量还需检查对应指令的执行结果。

### 创建和测试动作模板

点击“动作模板”区域的“添加”，选择任务类型：

| 类型 | 作用 |
| --- | --- |
| “CLI 命令” | 执行设备本地控制台命令。 |
| “SSH 命令” | 执行“指令”页已保存的远程指令。 |
| “LED 控制” | 控制 LED 的颜色、动画或矩阵内容。 |
| “日志记录” | 按指定级别写入日志。 |
| “设置变量” | 为自动化变量赋值。 |
| “Webhook” | 向一个 HTTP 地址发送请求。 |

填写唯一 ID、名称和当前类型要求的参数，可设置执行延迟与“异步执行”。异步操作在后台继续，提交后还需通过日志、变量或目标设备检查最终结果。

- CLI：填写命令行，可按需要设置结果变量和超时。
- SSH：选择已配置主机和指令，并核对预览。
- LED：选择设备和支持的功能，填写颜色、动画、亮度、文本、图像、QR 码或滤镜参数。
- 日志：选择级别并填写消息，可引用变量。
- 设置变量：填写变量名和值。
- Webhook：填写请求方式、URL 和请求内容；JSON 是填写结构化内容的文本格式。

“测试”会实际执行动作。先确认断电、重启、远程命令或外部请求的影响，再测试。编辑后保存并重新查看参数；保存失败时保留草稿，按提示修正。导入可能覆盖已有模板，删除前检查规则引用；绑定服务未确认停止时，先核验或停止。

### 创建规则

1. 点击“规则列表”区域的“添加”，填写唯一规则 ID、名称和图标。
2. 选择“条件逻辑”：AND 表示所有条件都满足；OR 表示任一条件满足。
3. 设置“冷却时间”，限制自动触发的频率。页面单位为 ms，1000 ms 等于 1 秒。
4. 添加条件，选择变量、比较方式和值。支持等于、不等于、大于、大于等于、小于、小于等于、值变化和包含；比较值的类型应与变量相符。
5. 添加动作模板，并设置顺序所需的延迟、重复和动作条件。
6. 决定是否开启“显示在面板”及“允许手动触发”。前者控制系统页是否出现卡片，后者控制能否手动运行。
7. 检查后保存，再启用。初次配置可先关闭“立即启用”。

### 配置动作重复和执行条件

| 方式 | 含义 |
| --- | --- |
| “单次” | 每次触发执行一次。 |
| “条件持续时重复” | 按单个动作的条件重复，每轮最多 100 次。 |
| “指定次数” | 按设置次数和间隔重复；每次执行前仍检查动作条件。 |

动作条件与规则触发条件分别设置。“条件持续时重复”未设置动作条件时，会重复到每轮上限；规则以后仍可能再次触发。

停用规则可阻止后续触发，但已经开始的一轮动作可能继续。需要结束后续调度时，使用引擎“停止”并等待确认；已提交的动作和远程进程仍需分别检查，不会撤销已完成的操作。

### 创建快捷操作

“仅手动触发”“显示在面板”“允许手动触发”分别决定是否只手动运行、是否显示卡片、是否允许用户执行。

1. 填写规则 ID、名称和图标，开启“仅手动触发”。这种规则不需要自动触发条件。
2. 添加动作模板，检查延迟、重复及动作条件。
3. 开启“显示在面板”和“允许手动触发”，保存并启用规则。
4. 返回“系统”的设备面板，确认卡片名称和状态，再执行一次并检查结果。

自动规则也可显示在面板；显示卡片不会把它改成仅手动运行。停用规则仍可能显示卡片，但不能启动。面向 admin 提供操作时，名称应让用户知道会发生什么，并提供适用的日志或停止方式。

### 维护规则和配置

- 手动执行要求规则已启用且允许手动触发，并且没有正在执行的同一规则。它不等待自动条件，也不受自动触发冷却时间限制；动作条件、延迟和重复仍生效。
- 停用规则不会撤销已完成的动作；删除也不能从页面撤销。
- 编辑前检查数据源、动作模板和远程指令的引用。绑定服务仍运行、状态未知或正在处理操作时，部分更改或删除会被阻止；先核验或停止服务。
- 导入前预览规则 ID、条件、动作及覆盖内容，按安全指南检查配置包。
- 保存失败时查看提示并保留草稿；保存已成功但列表刷新失败时，先刷新核对，不要直接重复创建。

修改或导入后，检查引擎状态、启用开关和实际结果。页面要求重载或重启时，先检查正在运行的任务，再安排操作。'''
    auto_e = '''“Automation” connects data and operations into repeatable workflows. Create data sources and action templates, then use rules to decide when to run them.

```text
Automatic: Data source → Variable → Rule evaluation → Actions
Manual: System-page Quick Action → Actions
```

Sources read data, variables hold values, rules evaluate conditions, and templates define tasks. Showing a rule on the panel and allowing manual execution are separate settings. Automatic rules can also appear on the panel.

### View and Control the Automation Engine

The top of the page shows the engine state, rule, variable, and source counts, trigger count, and runtime.

| Control | Effect |
| --- | --- |
| “Start” | Starts a stopped engine and begins processing enabled rules. |
| “Pause” | Pauses further automatic evaluation. Existing actions may continue. To resume through the page, stop the engine, then start it. |
| “Stop” | Attempts to stop rule processing, source polling, and further action scheduling. Configuration is retained. Wait for confirmation; a timeout means stopping has not completed. |
| “Reload” | Reads the saved configuration again. An engine that was running resumes after loading. Save edits first. |

Check whether cooling, alerts, or other ongoing tasks depend on Automation. Stopping the engine does not stop a remote background process. Stop remote tasks from Commands or the relevant service card and confirm the result.

### Build a Minimal Automation Workflow

1. Create a source and use its test to check the connection and selected fields.
2. Enable it, then use the row’s “View Variables” icon to check values and update times.
3. Create a template, review its parameters, and select “Test.” Testing runs the action immediately; confirm that the device and host can accept it.
4. Create a rule with “Enable immediately” off. Set conditions, cooldown, action order, delays, and repetition.
5. Set “Show on panel” and “Allow manual trigger” as needed.
6. Save, then enable the rule. Check variables, trigger counts, and actual results.

### Manage Data Sources

Select “Add” under “Data Sources” and choose a type:

| Type | Purpose |
| --- | --- |
| “REST API” | Periodically reads data from an HTTP address. |
| “WebSocket” | Receives data pushed over a persistent connection. |
| “Socket.IO” | Receives events from a Socket.IO service. |
| “Command Variable” | Reads the results of a configured remote command. |

1. Enter a unique ID, display label, and the required connection information.
2. Use the test to check the connection. Obtain addresses, authentication details, and field information from the data provider.
3. For the first three types, select fields from the test response. Leaving a Socket.IO event name blank lets the test attempt event discovery.
4. For a Command Variable, select the host and a command with a configured variable name, then set the polling interval.
5. Save and enable the source. Use its “View Variables” control to check the result.

Source rows provide an enable switch, variable viewer, export, and delete controls. There is no separate variable-search area on the main page. Disabling or deleting a source affects dependent rules; check references first.

Import and export use configuration packages. Preview IDs, types, and replacements before import, and follow the Security Guide for source and trust requirements.

### View Variables

Select the eye-shaped “View Variables” icon on a source row. The window shows names, types, values, and update times. Remote-command variables can also be viewed from the command’s variable control.

- Confirm the names and source, and check that values and types suit the rule’s comparisons.
- Check whether update times match the expected data frequency.
- If no data is available, check that the source is enabled, then test the connection and fields. For command variables, check the command’s execution result too.

### Create and Test Action Templates

Select “Add” under “Action Templates” and choose a task type:

| Type | Purpose |
| --- | --- |
| “CLI Command” | Runs a local device-console command. |
| “SSH Command” | Runs a remote command saved on the Commands page. |
| “LED Control” | Controls LED colors, effects, or matrix content. |
| “Log” | Writes a message at the selected level. |
| “Set Variable” | Assigns a value to an Automation variable. |
| “Webhook” | Sends a request to an HTTP address. |

Enter a unique ID, a name, and the type-specific parameters. Set a delay or “Async Execution” if needed. An asynchronous action continues in the background; check its final result through logs, variables, or the target device.

- CLI: Enter the command, with an optional result variable and timeout.
- SSH: Choose a configured host and command, and check the preview.
- LED: Choose a device and supported color, effect, brightness, text, image, QR-code, or filter operation.
- Log: Choose a level and message; the message can reference variables.
- Set Variable: Enter the variable name and value.
- Webhook: Enter the method, URL, and request content. JSON is the text format used for structured content.

“Test” performs the action. Check the effect of power, reboot, remote-command, or external-request operations before testing. After editing, save and review the parameters. If saving fails, keep the draft and correct the reported issue. Imports can replace templates. Check rule references before deleting; verify or stop any bound service whose stopped state is unconfirmed.

### Create a Rule

1. Select “Add” under “Rules” and enter a unique ID, name, and icon.
2. Choose “Logic”: AND requires all conditions; OR requires any condition.
3. Set the cooldown for automatic triggers. Values are in ms; 1000 ms is 1 second.
4. Add conditions and choose a variable, comparison, and value. Comparisons include equal, not equal, greater than, greater or equal, less than, less or equal, value changed, and contains. Match the value type to the variable.
5. Add templates and set any action delays, repetition, and action-level conditions.
6. Set “Show on panel” and “Allow manual trigger.” The first controls card visibility on System; the second controls manual execution.
7. Review, save, and then enable the rule. Leave “Enable immediately” off while configuring it for the first time.

### Configure Repetition and Action Conditions

| Mode | Meaning |
| --- | --- |
| “Once” | Runs once per trigger. |
| “Repeat while true” | Repeats while the action’s condition holds, up to 100 times per run. |
| “Fixed count” | Repeats for the configured count and interval, checking the action condition before each execution. |

Action conditions are separate from rule-trigger conditions. Without an action condition, “Repeat while true” continues to the per-run limit. The rule may trigger another run later.

Disabling a rule prevents later triggers, but a run already in progress may continue. To end further scheduling, use the engine’s “Stop” control and wait for confirmation. Check submitted actions and remote processes separately. Completed operations are not undone.

### Create a Quick Action

“Manual Trigger Only,” “Show on panel,” and “Allow manual trigger” control whether a rule runs only manually, whether its card is visible, and whether users may execute it.

1. Enter the ID, name, and icon, then enable “Manual Trigger Only.” Such a rule needs no automatic trigger conditions.
2. Add templates and review delays, repetition, and action conditions.
3. Enable “Show on panel” and “Allow manual trigger,” then save and enable the rule.
4. Return to the System Device Panel. Check the card’s name and state, run it once, and review the result.

Automatic rules can also appear on the panel. Showing a card does not make its rule manual-only. A disabled rule may remain visible but cannot start. Give admin-facing actions clear names and appropriate log or stop controls.

### Maintain Rules and Configuration

- Manual execution requires an enabled rule that permits manual triggers, with no run of the same rule already in progress. It bypasses automatic trigger conditions and cooldown, but still applies action conditions, delays, and repetition.
- Disabling does not undo completed actions. Deletion cannot be undone from the page.
- Check source, template, and remote-command references before editing. Some changes or deletions are blocked while a bound service is running, its state is unknown, or an operation is in progress. Verify or stop the service first.
- Preview IDs, conditions, actions, and replacements before import, and follow the Security Guide for package trust.
- If saving fails, read the message and retain the draft. If saving succeeded but refreshing failed, refresh and check the list before creating another item.

After changes or import, check engine state, enable switches, and actual results. If the page requests a reload or reboot, review running tasks before proceeding.'''
    rz = section(rz, '## 12. 自动化管理', auto_z)
    re_ = section(re_, '## 12. Automation Management', auto_e)
    DEST.mkdir(parents=True, exist_ok=True)
    for name, text in [('shared.zh.md', az), ('shared.en.md', ae), ('root.zh.md', rz), ('root.en.md', re_)]:
        (DEST / name).write_text(text.strip() + '\n')
    print('Created four editable source files in', DEST)

if __name__ == '__main__':
    main()
