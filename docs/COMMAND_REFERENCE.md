# TianShanOS 命令规范文档

> **版本**：1.0  
> **日期**：2026年1月15日

---

## 📋 命令格式规范

### 基本格式

TianShanOS 采用**参数风格**命令格式，所有命令遵循统一规范：

```
<command> [options] [arguments]
```

### 选项格式

| 格式 | 说明 | 示例 |
|-----|------|-----|
| `--option value` | 长选项带值 | `--speed 75` |
| `--option=value` | 长选项带值（等号） | `--speed=75` |
| `-o value` | 短选项带值 | `-s 75` |
| `-o=value` | 短选项带值（等号） | `-s=75` |
| `--flag` | 布尔标志（开启） | `--verbose` |
| `--no-flag` | 布尔标志（关闭） | `--no-verbose` |
| `-f` | 短布尔标志 | `-v` |

### 通用选项

所有命令都支持以下通用选项：

| 选项 | 短选项 | 说明 |
|-----|-------|------|
| `--help` | `-h` | 显示命令帮助 |
| `--json` | `-j` | 以 JSON 格式输出 |
| `--quiet` | `-q` | 静默模式，只输出结果 |
| `--verbose` | `-v` | 详细模式，输出调试信息 |

---

## 📦 命令分类

### 系统命令

```bash
# 显示系统信息
system --info
system -i

# 重启系统
system --reboot
system --reboot --delay 5        # 延迟5秒重启

# 关机（待实现）
system --shutdown

# 显示版本
system --version
system -V

# 显示运行时间
system --uptime

# 显示内存使用
system --memory

# 显示任务列表
system --tasks
```

### 服务管理命令

```bash
# 列出所有服务
service --list
service -l

# 显示服务详情
service --status --name fan_controller
service -s -n fan_controller

# 启动服务
service --start --name fan_controller
service --start -n fan_controller

# 停止服务
service --stop --name fan_controller

# 重启服务
service --restart --name fan_controller

# 启用/禁用服务自启动
service --enable --name fan_controller
service --disable --name fan_controller

# 显示服务依赖
service --deps --name fan_controller
```

### 配置管理命令

```bash
# 获取配置值
config --get --key system.language
config -g -k system.language

# 设置配置值（临时）
config --set --key fan.default_speed --value 50

# 设置配置值（持久化）
config --set --key fan.default_speed --value 50 --persist
config --set -k fan.default_speed -v 50 -p

# 列出所有配置
config --list
config --list --namespace fan      # 指定命名空间

# 导出配置到文件
config --export --file /sdcard/config_backup.json

# 从文件导入配置
config --import --file /sdcard/config_backup.json

# 重置配置为默认值
config --reset --key fan.default_speed
config --reset --namespace fan     # 重置整个命名空间
config --reset --all               # 重置所有配置

# 显示配置变更历史
config --history
```

### 引脚管理命令

```bash
# 显示引脚状态
pin --list
pin --list --used                  # 只显示已使用的引脚
pin --list --available             # 只显示可用引脚

# 显示引脚映射
pin --mapping

# 获取引脚信息
pin --info --function LED_MATRIX
pin --info --gpio 9

# 测试 GPIO（调试用）
pin --test --gpio 2 --mode output --level high
pin --test --gpio 2 --mode input
```

### 风扇控制命令

```bash
# 显示风扇状态
fan --status
fan --status --id 0                # 指定风扇
fan --status --json                # JSON 输出

# 设置风扇速度
fan --set --id 0 --speed 75
fan -s --id 0 -S 75

# 设置风扇模式
fan --mode --id 0 --value manual
fan --mode --id 0 --value auto
fan --mode --id 0 --value curve

# 启用/禁用风扇
fan --enable --id 0
fan --disable --id 0

# 配置温度曲线
fan --curve --id 0 --points "30:20,50:40,70:60,80:100"

# 设置占空比限制
fan --limits --id 0 --min 10 --max 100
fan --limits --id 0 --min 0         # 只设置最小值

# 设置迟滞参数
fan --hysteresis --id 0 --hyst 30 --interval 2000

# 保存/加载配置
fan --save
fan --save --id 0
fan --load
```

### 温度管理命令

```bash
# 显示温度状态
temp --status
temp --status --json

# 列出温度源
temp --providers
temp --providers --json

# 设置测试温度（调试用）
temp --set --value 45

# 切换温度模式
temp --mode --value manual
temp --mode --value auto
```

### LED 控制命令

```bash
# === 通用 LED 命令 ===

# 列出所有 LED 设备
led --list

# 显示设备状态
led --status --device matrix
led -s -d matrix

# 设置亮度
led --brightness --device matrix --value 80
led -b -d matrix -v 80

# 清除显示
led --clear --device matrix

# 填充颜色
led --fill --device matrix --color "#FF0000"
led --fill -d matrix -c red          # 支持颜色名称

# 刷新显示
led --show --device matrix

# === 单像素操作 ===

# 设置单个像素
led --pixel --device matrix --index 0 --color "#00FF00"
led --pixel -d matrix -i 0 -c green

# 矩阵坐标方式
led --pixel --device matrix --x 16 --y 16 --color blue

# === 动画控制 ===

# 播放动画
led --animation --device matrix --name rainbow
led -a -d matrix -n rainbow

# 播放动画（带参数）
led --animation -d matrix -n breathe --speed 50 --color red

# 停止动画
led --animation --device matrix --stop

# 列出可用动画
led --animation --list

# 加载动画文件
led --animation --load --file /sdcard/animations/custom.json --name my_anim

# 导出当前动画
led --animation --export --file /sdcard/animations/export.json

# === 特效控制 ===

# 应用特效
led --effect --device board --name fire
led --effect -d board -n wave --speed 80

# 列出可用特效
led --effect --list

# 停止特效
led --effect --device board --stop

# === 图层操作（矩阵专用）===

# 创建图层
led --layer --device matrix --create --name overlay --z-order 1

# 设置图层透明度
led --layer --device matrix --name overlay --opacity 128

# 删除图层
led --layer --device matrix --name overlay --delete

# 列出图层
led --layer --device matrix --list

# === 图像操作（矩阵专用）===

# 显示图像
led --image --device matrix --file /sdcard/images/logo.png

# 显示图像（带缩放）
led --image -d matrix -f /sdcard/images/logo.png --scale fit

# 导出当前画面
led --image --device matrix --export --file /sdcard/export.png

# 支持的图像格式：BMP, PNG, JPG, GIF
# GIF 会自动播放动画

# === 状态绑定 ===

# 绑定状态指示
led --bind --device touch --source agx.status --config /sdcard/status_map.json

# 解除绑定
led --unbind --device touch

# === 配置保存 ===

# 保存配置
led --config --save
led --config --save --device matrix

# 加载配置
led --config --load

# 重置配置
led --config --reset --device matrix
```

### 网络管理命令

```bash
# 显示网络状态
net --status

# 显示 IP 配置
net --ip

# 设置静态 IP
net --set --ip 10.10.99.97 --netmask 255.255.255.0 --gateway 10.10.99.1

# 启用/禁用网络接口
net --enable
net --disable

# 重置网络
net --reset

# DHCP 服务器管理
net --dhcp --status
net --dhcp --enable
net --dhcp --disable
net --dhcp --clients              # 显示 DHCP 客户端

# 显示网络日志
net --log
net --log --count 20              # 显示20条日志

# 保存网络配置
net --save
```

### 设备控制命令

```bash
# AGX 控制
device --agx --power on
device --agx --power off
device --agx --power restart
device --agx --status

# LPMU 控制
device --lpmu --power on
device --lpmu --power off
device --lpmu --reset
device --lpmu --status

# USB MUX 控制
device --usb-mux --target esp32
device --usb-mux --target agx
device --usb-mux --target lpmu
device --usb-mux --status
```

### 存储管理命令

```bash
# 显示存储状态
storage --status

# 挂载/卸载 SD 卡
storage --mount
storage --unmount

# 格式化 SD 卡
storage --format --confirm

# 列出文件
storage --list --path /sdcard/
storage --list -p /sdcard/ --recursive

# 读取文件
storage --read --file /sdcard/config.json

# 写入文件
storage --write --file /sdcard/test.txt --content "Hello"

# 删除文件
storage --delete --file /sdcard/test.txt

# 创建目录
storage --mkdir --path /sdcard/logs/

# 显示磁盘空间
storage --space
```

### 安全管理命令

```bash
# 密钥管理
security --key --generate --type ed25519 --name device_key
security --key --list
security --key --export-public --name device_key --file /sdcard/device_key.pub
security --key --delete --name old_key

# SSH 客户端
security --ssh --connect --host 10.10.99.98 --user admin --key device_key
security --ssh --exec --host 10.10.99.98 --command "ls -la"
security --ssh --allowed-commands           # 显示允许的命令列表

# mTLS 管理
security --mtls --status
security --mtls --generate-cert             # 生成自签名证书
security --mtls --import-ca --file /sdcard/ca.pem

# 会话管理
security --session --list
security --session --terminate --id 12345
```

### 脚本命令

```bash
# 执行脚本文件
script --run --file /sdcard/scripts/startup.ts

# 执行单行命令
script --exec "fan --set --id 0 --speed 75"

# 列出可用脚本
script --list

# 创建定时任务
script --schedule --name check_temp --file /sdcard/scripts/temp_check.ts --interval 60

# 取消定时任务
script --schedule --cancel --name check_temp
```

### 帮助命令

```bash
# 显示总帮助
help

# 显示命令帮助
help fan
help led --animation

# 搜索命令
help --search "temperature"

# 显示命令示例
help fan --examples
```

---

## 🌍 多语言支持

### 语言切换

```bash
# 切换到中文
config --set --key system.language --value zh-CN --persist

# 切换到英文
config --set --key system.language --value en --persist

# 查看当前语言
config --get --key system.language
```

### 支持的语言

| 代码 | 语言 |
|-----|------|
| `en` | English |
| `zh-CN` | 简体中文 |

---

## 📜 脚本语法

### 基本语法

```bash
# 这是注释
# TianShanOS Script v1.0

# 变量定义
$speed = 75
$device = "matrix"

# 执行命令
fan --set --id 0 --speed $speed
led --brightness --device $device --value 80

# 等待（毫秒）
wait 1000

# 条件判断
if ${temp.current} > 60
    fan --set --id 0 --speed 100
    led --fill --device touch --color red
elif ${temp.current} > 40
    fan --set --id 0 --speed 75
    led --fill --device touch --color yellow
else
    fan --set --id 0 --speed 50
    led --fill --device touch --color green
endif

# 循环
loop 5
    led --animation --device board --name rainbow
    wait 5000
    led --animation --device board --name breathe
    wait 5000
endloop

# 无限循环
loop forever
    # ...
    break              # 跳出循环
endloop

# 调用其他脚本
call /sdcard/scripts/common.ts

# 定义函数
function set_status($color)
    led --fill --device touch --color $color
    led --show --device touch
endfunction

# 调用函数
set_status(red)
set_status(green)
```

### 内置变量

| 变量 | 说明 |
|-----|------|
| `${system.uptime}` | 系统运行时间（秒） |
| `${system.memory.free}` | 可用内存（字节） |
| `${temp.current}` | 当前温度 |
| `${temp.source}` | 温度来源 |
| `${fan.0.speed}` | 风扇0速度 |
| `${net.ip}` | IP 地址 |
| `${net.connected}` | 网络连接状态 |
| `${agx.status}` | AGX 状态 |
| `${lpmu.status}` | LPMU 状态 |

---

## 📊 输出格式

### 标准输出

```
[OK] 操作成功完成
[ERROR] 操作失败: 详细错误信息
[WARN] 警告信息
[INFO] 提示信息
```

### JSON 输出

使用 `--json` 选项时，输出 JSON 格式：

```json
{
  "success": true,
  "code": 0,
  "message": "Operation completed",
  "data": {
    "fan_id": 0,
    "speed": 75,
    "mode": "manual"
  }
}
```

```json
{
  "success": false,
  "code": -1,
  "message": "Invalid parameter: speed must be 0-100",
  "data": null
}
```

---

## 🔐 权限要求

| 权限级别 | 说明 | 可执行命令示例 |
|---------|------|--------------|
| `NONE` | 无权限 | - |
| `READ` | 只读 | `--status`, `--list`, `--info` |
| `OPERATE` | 操作 | `--set`, `--enable`, `--start` |
| `ADMIN` | 管理员 | `--reset`, `--format`, `--key` |

CLI（串口）默认具有 ADMIN 权限。
WebUI 需要 mTLS 认证，权限由证书确定。

---

## 📝 命令开发规范

### 命令注册示例

```c
#include "ts_console.h"
#include "argtable3.h"

// 定义参数
static struct {
    struct arg_str *device;
    struct arg_int *brightness;
    struct arg_lit *help;
    struct arg_end *end;
} led_brightness_args;

// 命令处理函数
static int cmd_led_brightness(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&led_brightness_args);
    
    if (led_brightness_args.help->count > 0) {
        printf("Usage: led --brightness --device <device> --value <0-100>\n");
        return 0;
    }
    
    if (nerrors != 0) {
        arg_print_errors(stderr, led_brightness_args.end, argv[0]);
        return 1;
    }
    
    const char *device = led_brightness_args.device->sval[0];
    int brightness = led_brightness_args.brightness->ival[0];
    
    // 调用 Core API
    ts_api_result_t result = ts_api_led_set_brightness(device, brightness);
    
    return result.code;
}

// 注册命令
void register_led_commands(void) {
    led_brightness_args.device = arg_str1("d", "device", "<device>", "LED device name");
    led_brightness_args.brightness = arg_int1("v", "value", "<0-100>", "Brightness value");
    led_brightness_args.help = arg_lit0("h", "help", "Show help");
    led_brightness_args.end = arg_end(5);
    
    const esp_console_cmd_t cmd = {
        .command = "led",
        .help = "LED control commands",
        .hint = NULL,
        .func = &cmd_led_brightness,
        .argtable = &led_brightness_args
    };
    
    esp_console_cmd_register(&cmd);
}
```

---

## 📚 参考

- [ESP-IDF Console 组件](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/console.html)
- [argtable3 文档](http://www.argtable.org/)

---

**文档版本**：1.0  
**状态**：已确定


---
# 完整命令列表

以下内容来自原 COMMANDS.md，提供所有命令的详细参考。

---


本文档详细描述了 TianShanOS 控制台的所有可用命令及其参数。

---

## 目录

- [system - 系统信息与控制](#system---系统信息与控制)
- [service - 服务管理](#service---服务管理)
- [config - 配置管理](#config---配置管理)
- [led - LED 控制](#led---led-控制)
- [fan - 风扇控制](#fan---风扇控制)
- [storage - 存储管理](#storage---存储管理)
- [fs - 文件系统操作](#fs---文件系统操作)
- [net - 网络管理](#net---网络管理)
- [dhcp - DHCP 服务器管理](#dhcp---dhcp-服务器管理)
- [wifi - WiFi 管理](#wifi---wifi-管理)
- [device - 设备控制](#device---设备控制)
- [gpio - GPIO 直接控制](#gpio---gpio-直接控制)
- [power - 电源管理](#power---电源管理)
- [key - 安全密钥存储](#key---安全密钥存储)
- [ssh - SSH 客户端](#ssh---ssh-客户端)
- [hosts - SSH Known Hosts 管理](#hosts---ssh-known-hosts-管理)
- [ota - OTA 固件升级](#ota---ota-固件升级)

---

## system - 系统信息与控制

查看系统信息、资源使用情况以及执行系统控制操作。

### 语法

```
system [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--info` | `-i` | 显示系统概览（默认） |
| `--version` | `-v` | 显示版本信息 |
| `--uptime` | `-u` | 显示系统运行时间 |
| `--memory` | `-m` | 显示内存使用情况 |
| `--tasks` | `-t` | 显示任务/线程列表 |
| `--reboot` | | 重启系统 |
| `--delay <sec>` | | 重启延时（秒）；正数最多10秒，超过按10秒 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 示例

```bash
# 显示系统概览
system --info

# 显示版本
system --version

# 显示运行时间
system --uptime

# 显示内存使用（JSON 格式）
system --memory --json

# 显示所有任务
system --tasks

# 延时 1 秒后重启
system --reboot --delay 1
```

---

## service - 服务管理

管理 TianShanOS 服务的启动、停止和状态查询。

### 语法

```
service [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--list` | `-l` | 列出所有服务 |
| `--status` | `-s` | 查看服务状态 |
| `--start` | | 启动服务 |
| `--stop` | | 停止服务 |
| `--restart` | | 重启服务 |
| `--deps` | | 显示服务依赖关系 |
| `--name <name>` | `-n` | 指定服务名称 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 可用服务

| 服务名 | 说明 |
|--------|------|
| `hal` | 硬件抽象层 |
| `storage` | 存储服务 |
| `led` | LED 控制服务 |
| `drivers` | 驱动管理服务 |
| `console` | 控制台服务 |

### 示例

```bash
# 列出所有服务
service --list

# 查看特定服务状态
service --status --name led

# 启动服务
service --start --name led

# 停止服务
service --stop --name led

# 重启服务
service --restart --name led

# 显示服务依赖关系
service --deps --name led

# JSON 格式输出
service --list --json
```

---

## config - 配置管理

读取、修改和管理系统配置参数。

### 语法

```
config [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--get` | `-g` | 获取配置值 |
| `--set` | `-s` | 设置配置值 |
| `--list` | `-l` | 列出所有配置 |
| `--reset` | | 重置配置到默认值 |
| `--key <key>` | `-k` | 配置键名 |
| `--value <val>` | `-v` | 配置值 |
| `--persist` | `-p` | 持久化保存到 NVS |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 配置键格式

配置键使用点号分隔的层级结构：`category.subcategory.key`

常见配置键：
- `led.brightness` - LED 默认亮度
- `led.touch.gpio` - Touch LED GPIO 引脚
- `fan.speed` - 风扇速度
- `network.dhcp` - DHCP 开关

### 示例

```bash
# 列出所有配置
config --list

# 获取特定配置
config --get --key led.brightness

# 设置配置值（临时）
config --set --key led.brightness --value 128

# 设置并持久化保存
config --set --key led.brightness --value 128 --persist

# 重置所有配置
config --reset

# 重置特定配置
config --reset --key led.brightness

# JSON 格式输出
config --list --json
```

---

## led - LED 控制

控制 LED 设备的颜色、亮度和特效。

### 语法

```
led [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示 LED 设备状态 |
| `--on` | | 点亮 LED |
| `--off` | | 关闭 LED |
| `--brightness` | `-b` | 获取/设置亮度 |
| `--clear` | `-c` | 清除 LED（熄灭） |
| `--effect` | `-e` | 启动程序动画（Animation） |
| `--stop-effect` | | 停止运行中的程序动画 |
| `--list-effects` | | 列出可用程序动画 |
| `--filter` | | 应用后处理效果（Filter） |
| `--stop-filter` | | 停止后处理效果 |
| `--list-filters` | | 列出可用后处理效果 |
| `--filter-name <name>` | | 后处理效果名称 |
| `--speed <1-100>` | | 速度参数（适用于大部分 filter） |
| `--intensity <0-255>` | | 强度参数（scanline, glitch） |
| `--width <1-16>` | | 宽度参数（scanline） |
| `--angle <0-360>` | | 角度参数（scanline, wave），0°=水平，90°=垂直 |
| `--wavelength <1-32>` | | 波长参数（wave） |
| `--amplitude <0-255>` | | 振幅参数（wave） |
| `--density <0-255>` | | 密度参数（sparkle） |
| `--decay <0-255>` | | 衰减参数（sparkle） |
| `--image` | | 显示图像/动画（仅 matrix） |
| `--draw-text` | | 绘制文本到 matrix（仅 matrix） |
| `--stop-text` | | 停止文本覆盖层 |
| `--font <name>` | | 字体名称（cjk/boutique9x9，默认 cjk） |
| `--text-file <path>` | | 从文件读取文本（UTF-8，支持中文） |
| `--align <mode>` | | 文本对齐：left/center/right |
| `--scroll <dir>` | | 文本滚动方向：left/right/up/down/none |
| `--x <pos>` | | 文本起始 X 位置（默认 0） |
| `--y <pos>` | | 文本起始 Y 位置（默认 0） |
| `--invert` | | 反色覆盖模式（与底层图像叠加时自动反色） |
| `--loop` | | 循环滚动（文本滚出后重新进入） |
| `--qrcode` | | 生成并显示 QR 码（仅 matrix） |
| `--text <string>` | | 文本内容（--draw-text/--qrcode 使用） |
| `--ecc <L\|M\|Q\|H>` | | QR 纠错等级 |
| `--bg <path>` | | QR 前景图片路径（前景像素使用图片颜色） |
| `--file <path>` | `-f` | 图像文件路径（PNG/GIF） |
| `--center <mode>` | | 居中模式：image（图像居中）或 content（内容居中） |
| `--parse-color` | | 解析颜色信息 |
| `--device <name>` | `-d` | 指定设备名称 |
| `--name <effect>` | `-n` | 程序动画名称 |
| `--value <0-255>` | `-v` | 亮度值 |
| `--color <color>` | | 颜色值 |
| `--speed <1-100>` | | 动画/效果/滚动速度（1=慢, 100=快） |
| `--save` | | 保存当前状态为开机配置（含动画和后处理效果） |
| `--show-boot` | | 显示已保存的开机配置 |
| `--clear-boot` | | 清除开机配置 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### Animation vs Filter

TianShanOS LED 系统区分两类效果：

| 类型 | 说明 | 示例 |
|------|------|------|
| **Animation（程序动画）** | 生成帧内容的程序 | rainbow, fire, rain, plasma |
| **Filter（后处理效果）** | 对已有内容进行后处理 | pulse, blink, breathing, color-shift |

- Animation 是内容生成器，可独立运行
- Filter 叠加在 Animation 或 Image 之上，对输出进行调整
- 两者可以组合使用：Animation + Filter

### 可用设备

| 设备名 | 别名 | LED 数量 | 布局 | 说明 |
|--------|------|----------|------|------|
| `led_touch` | `touch` | 1 | 点光源 | 触摸板指示 LED（单颗 WS2812） |
| `led_board` | `board` | 28 | 环形 | PCB 边缘环板灯（28 颗 WS2812 环形排列） |
| `led_matrix` | `matrix` | 1024 | 矩阵 | LED 矩阵屏（32x32 WS2812 网格排列） |

### 可用特效

不同形态的 LED 设备有各自适合的特效：

#### 通用特效（所有设备）

| 特效名 | 说明 |
|--------|------|
| `rainbow` | 彩虹渐变效果 |
| `breathing` | 呼吸灯效果 |
| `solid` | 纯色填充 |
| `sparkle` | 闪烁效果 |

#### Touch 专属（点光源）

| 特效名 | 说明 |
|--------|------|
| `pulse` | 脉冲闪烁（快速闪亮后渐灭） |
| `heartbeat` | 心跳效果（双跳节奏） |
| `color_cycle` | 颜色循环（平滑色相变化） |

#### Board 专属（环形灯带）

| 特效名 | 说明 |
|--------|------|
| `chase` | 追逐/跑马灯（带拖尾） |
| `comet` | 流星效果（单向快速移动） |
| `spin` | 旋转彩虹（半亮半暗旋转） |
| `breathe_wave` | 呼吸波（波浪式亮度变化） |

#### Matrix 专属（矩阵屏）

| 特效名 | 说明 |
|--------|------|
| `fire` | 火焰效果（向上燃烧） |
| `rain` | 数字雨/雨滴（下落动画） |
| `plasma` | 等离子效果（2D 波纹） |
| `ripple` | 水波纹（中心扩散） |

### 可用后处理效果（Filter）

Filter 是叠加在内容之上的后处理效果，可与 Animation 或 Image 组合使用：

| 效果名 | 说明 | 主要参数 |
|--------|------|----------|
| `pulse` | 脉冲亮度（正弦波调节） | `--speed` |
| `blink` | 开关闪烁 | `--speed` |
| `breathing` | 平滑呼吸效果 | `--speed` |
| `fade-in` | 淡入（一次性） | `--speed` |
| `fade-out` | 淡出（一次性） | `--speed` |
| `color-shift` | 色相旋转 | `--speed` |
| `saturation` | 饱和度调整 | - |
| `invert` | 反色 | - |
| `grayscale` | 灰度转换 | - |
| `scanline` | 扫描线效果 | `--speed`, `--angle` (0-360°), `--width` (1-16), `--intensity` (0-255) |
| `wave` | 亮度波浪 | `--speed`, `--angle` (0-360°), `--wavelength` (1-32), `--amplitude` (0-255) |
| `sparkle` | 闪耀效果 | `--speed` (0.1-100，推荐1-10), `--density` (0-255), `--decay` (0-255) |
| `glitch` | 随机故障效果 | `--intensity`, `--frequency` |

**参数说明**：
- **angle**：0°=水平向右，90°=垂直向上，支持0-360°任意角度
- **amplitude**：波浪亮度变化幅度，推荐50-200
- **density**：同时闪烁的像素数量，推荐50-150
- **decay**：余晖衰减速度，值越大衰减越快，推荐100-200

### 颜色格式

支持以下颜色格式：

- **十六进制**：`#RRGGBB`（如 `#FF0000`）
- **颜色名称**：`red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `white`, `black`, `orange`, `purple`, `pink`

### 示例

```bash
# 显示所有 LED 设备状态
led --status

# 显示特定设备状态
led --status --device touch
led --status --device board
led --status --device matrix

# 点亮 LED（默认白色）
led --on --device touch

# 点亮指定颜色
led --on --device touch --color red
led --on --device board --color #00FF00
led --on --device matrix --color blue

# 关闭 LED
led --off --device touch
led --off --device board

# 清除 LED
led --clear --device matrix

# 设置亮度
led --brightness --device touch --value 128
led --brightness --device board --value 200
led --brightness --device matrix --value 64

# 获取亮度
led --brightness --device touch

# 启动特效（使用各设备专属特效）
led --effect --device touch --name heartbeat
led --effect --device board --name spin
led --effect --device matrix --name fire

# 启动特效（带速度控制）
led --effect --device touch --name pulse --speed 50
led --effect --device board --name comet --speed 80
led --effect --device matrix --name rain --speed 30

# 启动特效（带颜色参数）
led --effect --device matrix --name rain --color red
led --effect --device matrix --name rain --color #FF5500 --speed 40

# 停止特效
led --stop-effect --device touch
led --stop-effect --device board

# 列出所有可用特效（按设备分类）
led --list-effects

# 列出特定设备的可用特效
led --list-effects --device touch
led --list-effects --device board
led --list-effects --device matrix

# 解析颜色
led --parse-color --color "#FF5500"

# 显示图像（静态 PNG）
led --image --device matrix --file /sdcard/logo.png

# 播放 GIF 动画
led --image --device matrix --file /sdcard/16mario.gif

# 显示图像（内容居中模式，自动裁剪透明边缘）
led --image --device matrix --file /sdcard/icon.png --center content

# JSON 格式输出
led --status --json
led --list-effects --json

# 保存当前 LED 状态为开机配置（支持图像/动画/后处理效果）
led --save --device matrix         # 保存 matrix（包括当前图像和 filter）
led --save --device touch          # 保存 touch 设备
led --save                          # 保存所有设备

# 查看已保存的开机配置（显示 ANIMATION 和 FILTER 列）
led --show-boot
led --show-boot --device touch

# 清除开机配置
led --clear-boot --device touch    # 清除 touch 配置
led --clear-boot                    # 清除所有配置

# 后处理效果（Filter）命令
led --filter --device matrix --filter-name pulse           # 应用脉冲效果
led --filter --device matrix --filter-name pulse --speed 80  # 带速度参数
led --filter --device matrix --filter-name breathing       # 呼吸效果
led --filter --device matrix --filter-name color-shift     # 色相旋转

# 扫描线效果（支持0-360°角度）
led --filter --device matrix --filter-name scanline --angle 0    # 水平
led --filter --device matrix --filter-name scanline --angle 45   # 45°斜向
led --filter --device matrix --filter-name scanline --angle 90   # 垂直
led --filter --device matrix --filter-name scanline --angle 0 --width 5 --intensity 200

# 波浪效果（支持任意角度）
led --filter --device matrix --filter-name wave --angle 0 --amplitude 128
led --filter --device matrix --filter-name wave --angle 90 --wavelength 10 --amplitude 200

# 闪耀效果（优化后的参数控制）
led --filter --device matrix --filter-name sparkle --speed 1 --density 100 --decay 100   # 极慢
led --filter --device matrix --filter-name sparkle --speed 5 --density 150 --decay 150   # 推荐值
led --filter --device matrix --filter-name sparkle --speed 10 --density 200 --decay 200  # 快速

# 组合使用：先启动动画，再叠加后处理效果
led --effect --device matrix --name fire
led --filter --device matrix --filter-name pulse --speed 50
led --save --device matrix                                  # 保存组合效果

# 停止后处理效果
led --stop-filter --device matrix

# 列出可用后处理效果
led --list-filters
led --list-filters --json

# QR 码生成（仅 matrix 设备，自动选择 v1-v4 版本）
led --qrcode --text "https://rminte.com/"           # 默认白色，ECC=M
led --qrcode --text "HELLO WORLD" --ecc H           # 高纠错等级
led --qrcode --text "192.168.1.100:8080" --color green  # 绿色 QR 码
led --qrcode --text "WIFI:T:WPA;S:MySSID;P:password;;" --ecc M  # WiFi 配置
led --qrcode --text "https://example.com" --bg /sdcard/me.png  # 图片前景
```

### QR Code 功能

Matrix 设备支持生成 QR Code 显示，自动选择最佳版本：

| 版本 | 尺寸 | 适配 32×32 | 容量（字母数字/M级） |
|------|------|-----------|-------------------|
| v1 | 21×21 | ✅ 居中 | 20 字符 |
| v2 | 25×25 | ✅ 居中 | 38 字符 |
| v3 | 29×29 | ✅ 居中 | 61 字符 |
| v4 | 33×33 | ⚠️ 裁剪1px | 90 字符 |

系统会根据文本长度自动选择最小版本，优先使用 v1-v3 以避免裁剪。

| 参数 | 说明 |
|------|------|
| `--text` | 要编码的文本内容（必需） |
| `--ecc` | 纠错等级：L/M/Q/H |
| `--color` | 前景色（模块颜色），默认白色 |
| `--bg` | 前景图片路径，前景像素使用图片对应位置的颜色 |

**纠错等级与容量（字母数字模式，v4 版本）：**

| ECC | 纠错能力 | 最大容量 |
|-----|---------|---------|
| L | ~7% | 114 字符 |
| M | ~15% | 90 字符 |
| Q | ~25% | 67 字符 |
| H | ~30% | 50 字符 |

> **注意**：内容较短时优先使用 v1-v3（完全适配 32×32），长内容自动升级到 v4（裁剪 1 像素边缘）。

### 文本显示功能

Matrix 设备支持在 Layer 1（覆盖层）上显示文本，**不影响底层动画/图像**。所有文本显示统一使用覆盖层机制，`--stop-text` 可随时清除文本恢复底层内容。

#### 基本用法

```bash
# 静态显示
led --draw-text --text "Hello" --color cyan

# 居中显示
led --draw-text --text "OK" --align center --color green

# 指定位置显示
led --draw-text --text "Hi" --x 5 --y 10
```

#### 滚动与反色

```bash
# 滚动文本
led --draw-text --text "Hello World" --scroll left
led --draw-text --text "Super Mario World" --scroll left

# 循环滚动
led --draw-text --text "Breaking News..." --scroll left --loop

# 反色覆盖（在亮色背景上自动反色）
led --draw-text --text "警告" --font cjk --invert

# 停止文本显示（恢复底层内容）
led --stop-text
```

#### 参数说明

| 参数 | 说明 |
|------|------|
| `--draw-text` | 启用文本绘制模式 |
| `--stop-text` | 停止文本覆盖层 |
| `--text <string>` | 要显示的文本（ASCII） |
| `--text-file <path>` | 从文件读取 UTF-8 文本（支持中文） |
| `--font <name>` | 字体名称：cjk（默认）或 boutique9x9 |
| `--align <mode>` | 对齐方式：left/center/right |
| `--color <color>` | 文本颜色 |
| `--x <pos>` | 起始 X 位置（默认 0） |
| `--y <pos>` | 起始 Y 位置（默认 0） |
| `--scroll <dir>` | 滚动方向：left/right/up/down/none |
| `--invert` | 反色覆盖（在亮色背景上自动反色显示） |
| `--loop` | 循环滚动（需配合 --scroll） |
| `--speed <1-100>` | 滚动速度（1=慢, 100=快） |

**可用字体**：

| 字体名 | 说明 | 字符集 |
|--------|------|--------|
| `cjk` | BoutiqueBitmap 9x9 CJK（默认） | GB2312（6763 汉字 + ASCII） |
| `boutique9x9` | BoutiqueBitmap 9x9 | ASCII（95 字符） |

**更多示例**：

```bash
# 显示中文（通过文件，推荐方式）
led --draw-text --text-file /sdcard/msg.txt --font cjk --color yellow

# 调整滚动速度（1=慢, 100=快）
led --draw-text --text "Fast!" --scroll left --font boutique9x9 --speed 80

# 向上滚动
led --draw-text --text "不错" --scroll up --font boutique9x9  --loop

# 覆盖层组合使用
led --image -f /sdcard/images/bg.png               # 先显示背景
led --draw-text --text "提示" --invert --scroll left --loop  # 叠加滚动文本
led --stop-text                                     # 恢复背景
```

> **⚠️ 中文输入限制**：由于 ESP-IDF 串口控制台的 UTF-8 解析限制，直接在命令行输入中文（如 `--text "你好"`）可能导致参数解析错误。**请使用 `--text-file` 从文件读取中文文本**。

---

## fan - 风扇控制

控制系统风扇的速度、运行模式和温度曲线。

> **硬件配置**：Fan 0 使用 GPIO 41 (PWM 25kHz)。系统配置了一个风扇。

### 语法

```
fan [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示风扇状态 |
| `--set` | | 设置风扇速度（手动模式） |
| `--mode` | `-m` | 设置运行模式 |
| `--curve` | | 设置温度曲线 |
| `--hysteresis` | | 设置迟滞参数 |
| `--enable` | | 启用风扇 |
| `--disable` | | 禁用风扇 |
| `--save` | | 保存配置到 NVS |
| `--id <n>` | `-i` | 风扇 ID（0-3） |
| `--speed <0-100>` | `-S` | 速度百分比 |
| `--value <mode>` | | 模式值 |
| `--points <curve>` | | 曲线点（格式见下） |
| `--hyst <0.1°C>` | | 迟滞温度（30=3.0°C） |
| `--interval <ms>` | | 最小调速间隔 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 运行模式

| 模式 | 说明 |
|------|------|
| `off` | 风扇停止 |
| `manual` | 手动模式（使用 --speed 设定固定占空比） |
| `auto` | 自动模式（曲线控制，无迟滞） |
| `curve` | 曲线模式（曲线控制 + 迟滞防抖） |

### 曲线点格式

曲线点使用 `"温度:占空比"` 格式，多个点用逗号分隔：

```
"30:20,50:40,70:80,80:100"
```

含义：
- 30°C 时 20% 转速
- 50°C 时 40% 转速
- 70°C 时 80% 转速
- 80°C 时 100% 转速

中间温度使用**线性插值**计算。

### 迟滞控制

为防止温度小幅波动导致风扇频繁变速：

- `--hyst`：温度变化超过此值才调速（默认 30 = 3.0°C）
- `--interval`：两次调速最小间隔（默认 2000ms）

### 示例

```bash
# 显示所有风扇状态
fan --status

# 显示特定风扇详细状态
fan --status --id 0

# 手动设置风扇速度
fan --set --id 0 --speed 75

# 设置运行模式
fan --mode --id 0 --value auto
fan --mode --id 0 --value curve
fan --mode --id 0 --value off

# 设置温度曲线
fan --curve --id 0 --points "30:20,50:40,70:80,80:100"

# 设置迟滞参数
fan --hysteresis --id 0 --hyst 30 --interval 2000

# 启用/禁用风扇
fan --enable --id 0
fan --disable --id 0

# 保存配置到 NVS
fan --save

# JSON 格式输出
fan --status --json
```

### 状态输出示例

```
Fan Status:

ID    ENABLED  RUNNING    DUTY     RPM   TEMP   MODE
───────────────────────────────────────────────────
0     Yes      Yes        60%     1200   45.0°  curve
```

---

## storage - 存储管理

管理 SD 卡和 SPIFFS 存储。

### 语法

```
storage [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示存储状态 |
| `--mount` | | 挂载 SD 卡 |
| `--unmount` | | 卸载 SD 卡 |
| `--list` | `-l` | 列出文件 |
| `--read` | `-r` | 读取文件内容 |
| `--space` | | 显示磁盘空间 |
| `--format` | | 格式化存储 |
| `--path <path>` | `-p` | 目录路径 |
| `--file <file>` | `-f` | 文件路径 |
| `--recursive` | | 递归列出 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 挂载点

| 路径 | 说明 |
|------|------|
| `/spiffs` | SPIFFS 分区（内置 Flash） |
| `/sdcard` | SD 卡挂载点 |

### 示例

```bash
# 显示存储状态
storage --status

# 挂载 SD 卡
storage --mount

# 卸载 SD 卡
storage --unmount

# 列出根目录文件
storage --list --path /sdcard

# 递归列出目录
storage --list --path /sdcard --recursive

# 读取文件内容
storage --read --file /sdcard/config.json

# 显示磁盘空间
storage --space

# JSON 格式输出
storage --status --json
```

---

## net - 网络管理

管理以太网网络配置。

> **当前状态**：
> - ✅ 以太网（W5500）完全支持
> - ✅ WiFi 请使用 `wifi` 命令
> - ✅ 配置保存/加载支持 NVS 持久化

### 语法

```
net [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示网络状态 |
| `--config` | | 显示接口配置详情 |
| `--set` | | 设置网络参数（需配合其他参数） |
| `--start` | | 启动网络接口 |
| `--stop` | | 停止网络接口 |
| `--restart` | | 重启网络接口 |
| `--save` | | 保存配置到 NVS |
| `--load` | | 从 NVS 加载配置 |
| `--reset` | | 重置为默认配置 |
| `--ip` | | 快速显示 IP 地址 |
| `--iface <if>` | | 指定接口：eth（默认） |
| `--ip <addr>` | | 静态 IP 地址 |
| `--netmask <mask>` | | 子网掩码 |
| `--gateway <gw>` | | 默认网关 |
| `--dns <addr>` | | DNS 服务器地址 |
| `--mode <mode>` | | IP 模式：dhcp, static |
| `--hostname <name>` | | 设置主机名 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 示例

```bash
# 显示网络状态
net --status

# 快速查看 IP
net --ip

# 显示以太网配置详情
net --config --iface eth

# 设置静态 IP
net --set --mode static --ip 10.10.99.97 --netmask 255.255.255.0 --gateway 10.10.99.1

# 切换到 DHCP 客户端模式
net --set --mode dhcp

# 设置 DNS
net --set --dns 8.8.8.8

# 设置主机名
net --set --hostname TianShanOS

# 保存配置到 NVS（重启后生效）
net --save

# 重启接口应用更改
net --restart

# 从 NVS 加载配置
net --load

# 重置为默认配置
net --reset

# JSON 格式输出
net --status --json
```

以太网状态中的 DNS 是接口读回的值，不保证等于 DHCP 客户端收到的 DNS。客户端的主 DNS 由已生效的 DHCP 地址池 DNS 决定；池 DNS 留空时，当前构建下发 TianShanOS 在该接口上的 IP 地址。修改 `net --set --dns` 不会直接替换客户端的主 DNS；要更改客户端 DNS，请设置 DHCP 池的 DNS 并重启 DHCP 服务应用。

以太网启停/重启正在执行时，另一次 manager 操作会立即返回“操作进行中，请稍后”，不会自动排队重试；状态查询仍可使用。DHCP 在链路停止期间接受启动请求时只登记未来意图，不会立即起服，实际运行状态请查看 `dhcp --status --iface eth`。

### 配置持久化

- `net --set` 修改的配置是**临时的**，重启后丢失
- 使用 `net --save` 保存到 NVS，重启后自动加载
- 修改配置后需要 `net --restart` 才能生效

---

## dhcp - DHCP 服务器管理

管理 TianShanOS 内置的 DHCP 服务器，支持多接口（WiFi AP、Ethernet）。

### 语法

```
dhcp [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | | 显示 DHCP 服务器状态 |
| `--list` | | 列出所有接口状态 |
| `--clients` | | 列出已连接的客户端 |
| `--start` | | 启动 DHCP 服务器 |
| `--stop` | | 停止 DHCP 服务器 |
| `--restart` | | 重启 DHCP 服务器 |
| `--pool` | | 配置地址池 |
| `--bind` | | 添加静态绑定 |
| `--unbind` | | 删除静态绑定 |
| `--bindings` | | 列出所有静态绑定 |
| `--save` | | 保存配置到 NVS |
| `--reset` | | 重置为默认配置 |
| `--iface <if>` | | 指定接口：`ap`、`eth`、`all`（默认） |
| `--start-ip <ip>` | | 地址池起始 IP |
| `--end-ip <ip>` | | 地址池结束 IP |
| `--gateway <ip>` | | 网关地址 |
| `--netmask <mask>` | | 子网掩码 |
| `--dns <ip>` | | DNS 服务器地址 |
| `--lease <min>` | | 租约时间（分钟） |
| `--mac <addr>` | | MAC 地址（用于绑定） |
| `--ip <addr>` | | IP 地址（用于绑定） |
| `--hostname <name>` | | 主机名（用于绑定） |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 接口说明

| 接口 | 别名 | 说明 |
|------|------|------|
| `ap` | `wifi`, `wifi_ap` | WiFi 接入点 |
| `eth` | `ethernet` | 以太网接口 |
| `all` | （默认） | 所有接口 |

### 示例

```bash
# 显示所有接口 DHCP 状态
dhcp --status

# 显示以太网接口详细状态
dhcp --status --iface eth

# 列出所有客户端
dhcp --clients

# 列出以太网接口的客户端
dhcp --clients --iface eth

# 启动以太网 DHCP 服务器
dhcp --start --iface eth

# 停止 WiFi AP DHCP 服务器
dhcp --stop --iface ap

# 重启所有接口的 DHCP 服务器
dhcp --restart

# 配置以太网地址池
dhcp --pool --iface eth --start-ip 10.10.99.100 --end-ip 10.10.99.200

# 配置完整地址池
dhcp --pool --iface eth \
    --start-ip 10.10.99.100 \
    --end-ip 10.10.99.200 \
    --gateway 10.10.99.97 \
    --netmask 255.255.255.0 \
    --dns 8.8.8.8 \
    --lease 120

# 添加静态绑定（MAC → IP）
dhcp --bind --iface eth --mac aa:bb:cc:dd:ee:ff --ip 10.10.99.50

# 添加带主机名的静态绑定
dhcp --bind --iface eth --mac aa:bb:cc:dd:ee:ff --ip 10.10.99.50 --hostname jetson-agx

# 删除静态绑定
dhcp --unbind --iface eth --mac aa:bb:cc:dd:ee:ff

# 列出所有静态绑定
dhcp --bindings --iface eth

# 保存配置
dhcp --save

# 重置为默认配置
dhcp --reset

# JSON 格式输出
dhcp --status --json
dhcp --clients --iface eth --json
```

### 配置存储

DHCP 配置存储在 NVS 中，重启后自动加载。使用 `--save` 命令保存当前配置，使用 `--reset` 恢复默认值。

### 默认配置

| 接口 | 地址池 | 网关 | DNS | 租约 |
|------|--------|------|-----|------|
| Ethernet | 10.10.99.100-103 | 10.10.99.100 | 8.8.8.8 | 60 分钟 |
| WiFi AP | 192.168.4.2-10 | 192.168.4.1 | 192.168.4.1 | 60 分钟 |

---

## wifi - WiFi 管理

管理 WiFi AP 模式（热点）和 STA 模式（连接外网）。

### 语法

```
wifi [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示 WiFi 状态 |
| `--scan` | | 扫描附近 WiFi 网络 |
| `--ap` | | 配置/查看 AP 模式 |
| `--connect` | | 连接到 WiFi 网络（STA 模式） |
| `--disconnect` | | 断开 WiFi 连接（STA 模式） |
| `--start` | | 启动 WiFi 接口 |
| `--stop` | | 停止 WiFi 接口 |
| `--save` | | 保存配置到 NVS |
| `--ssid <name>` | | WiFi 网络名称 |
| `--pass <password>` | | WiFi 密码 |
| `--iface <if>` | | 接口：`ap`（默认）或 `sta` |
| `--channel <1-13>` | | WiFi 信道（仅 AP 模式） |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 示例

```bash
# 显示 WiFi 状态
wifi --status

# 扫描附近 WiFi 网络
wifi --scan

# 配置并启动 AP 热点 （尚未测试）
wifi --ap --ssid TianShanOS --pass tianshan123
wifi --start --iface ap

# 连接到外网 WiFi（STA 模式）
wifi --connect --ssid HomeWiFi --pass secret123

# 断开 STA 连接
wifi --disconnect

# 停止 AP
wifi --stop --iface ap

# 保存配置
wifi --save

# JSON 输出
wifi --status --json
```

### AP 模式（热点）

AP 模式用于：
- 备用管理通道（手机直连 ESP32）
- 为以太网设备提供外网（配合 STA 模式 + NAT）

默认配置：
- SSID: `TianShanOS`
- 密码: `tianshan123`
- IP: `192.168.4.1`
- 信道: 1

### STA 模式（客户端）

STA 模式用于连接外部 WiFi 路由器，获取外网访问能力。

---

## fs - 文件系统操作

类 Unix 风格的文件系统操作命令。

### 可用命令

| 命令 | 说明 |
|------|------|
| `ls [path]` | 列出目录内容 |
| `cat <file>` | 显示文件内容 |
| `cd <path>` | 切换工作目录 |
| `pwd` | 显示当前工作目录 |
| `mkdir <path>` | 创建目录 |
| `rm <path>` | 删除文件或目录 |
| `cp <src> <dst>` | 复制文件 |
| `mv <src> <dst>` | 移动/重命名文件 |
| `touch <file>` | 创建空文件 |

### 路径说明

- 支持绝对路径（以 `/` 开头）和相对路径
- 默认工作目录：`/sdcard`
- 支持 `..`（上级目录）和 `.`（当前目录）

### 挂载点

| 路径 | 说明 |
|------|------|
| `/spiffs` | SPIFFS 分区（内置 Flash） |
| `/sdcard` | SD 卡挂载点 |

### 示例

```bash
# 显示当前目录
pwd

# 列出当前目录
ls

# 列出指定目录
ls /sdcard/images

# 详细列表（显示大小、时间）
ls -l
ls -l /sdcard

# 切换目录
cd /sdcard/config
cd ..            # 上级目录
cd images        # 相对路径

# 查看文件内容
cat config.json
cat /sdcard/scripts/startup.sh

# 创建目录
mkdir /sdcard/backup
mkdir logs

# 创建空文件
touch /sdcard/test.txt

# 复制文件
cp config.json config.json.bak
cp /sdcard/a.txt /sdcard/backup/a.txt

# 移动/重命名
mv old.txt new.txt
mv /sdcard/temp.txt /sdcard/archive/temp.txt

# 删除文件
rm /sdcard/temp.txt

# 删除空目录
rm /sdcard/empty_dir
```

### 注意事项

- `rm` 命令只能删除文件或空目录
- SD 卡需要插入才能访问 `/sdcard`
- SPIFFS 分区空间有限，不建议存放大文件

---

## device - 设备控制

控制外部设备（AGX、LPMU、USB MUX）。

> **注意**：当前为模拟实现，未接入真实硬件驱动。

### 语法

```
device [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--agx` | | AGX 设备控制 |
| `--lpmu` | | LPMU 设备控制 |
| `--usb-mux` | | USB MUX 控制 |
| `--power <op>` | | 电源操作（on/off/restart） |
| `--target <target>` | | USB MUX 目标 |
| `--status` | `-s` | 显示设备状态 |
| `--reset` | | 重置设备 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 设备说明

| 设备 | 说明 |
|------|------|
| AGX | NVIDIA Jetson AGX 模块 |
| LPMU | 低功耗管理单元 |
| USB MUX | USB 多路选择器 |

### USB MUX 目标

| 目标 | 说明 |
|------|------|
| `esp32` | USB 连接到 ESP32 |
| `agx` | USB 连接到 AGX |
| `lpmu` | USB 连接到 LPMU |

### 示例

```bash
# 显示所有设备状态
device

# 显示 AGX 状态
device --agx --status

# 开启 AGX 电源
device --agx --power on

# 关闭 AGX 电源
device --agx --power off

# 重启 AGX
device --agx --power restart

# 重置 AGX
device --agx --reset

# 显示 LPMU 状态
device --lpmu --status

# 开启 LPMU
device --lpmu --power on

# 显示 USB MUX 状态
device --usb-mux --status

# 切换 USB MUX 目标
device --usb-mux --target agx
device --usb-mux --target esp32

# JSON 格式输出
device --agx --status --json
```

---

## gpio - GPIO 直接控制

底层 GPIO 引脚直接控制，用于调试和硬件测试。此命令优先级高于其他驱动程序，可直接覆盖 GPIO 状态。

> **⚠️ 警告**：此命令直接操作硬件，不当使用可能导致设备损坏或系统不稳定。仅限调试和紧急情况使用。

### 语法

```
gpio <pin> <action> [options]
gpio --list
gpio --info <pin>
```

### 操作

| 动作 | 说明 |
|------|------|
| `high [ms]` | 设置高电平（可选保持时间后恢复） |
| `low [ms]` | 设置低电平（可选保持时间后恢复） |
| `pulse <ms>` | 输出正脉冲（HIGH 持续 ms 后恢复 LOW） |
| `pulse <ms> -n` | 输出负脉冲（LOW 持续 ms 后恢复 HIGH） |
| `toggle` | 切换当前电平 |
| `input` | 切换为输入模式并读取 |
| `reset` | 重置引脚到默认状态 |

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--list` | | 列出所有可控引脚 |
| `--info <pin>` | | 显示引脚详情 |
| `--negative` | `-n` | 负脉冲模式（pulse 时先 LOW 后 HIGH） |
| `--no-restore` | | 不恢复原电平 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 可控引脚

出于安全考虑，仅以下设备控制引脚可通过 `gpio` 命令操作：

| GPIO | 名称 | 说明 | 默认电平 |
|------|------|------|----------|
| 1 | AGX_RESET | AGX 复位 (HIGH=复位) | LOW |
| 2 | LPMU_RESET | LPMU 复位 (HIGH=复位) | LOW |
| 3 | AGX_FORCE_SHUTDOWN | AGX 强制关机 (LOW=开机, HIGH=关机) | LOW |
| 8 | USB_MUX_0 | USB MUX 选择位0 | LOW |
| 17 | RTL8367_RST | 网络交换机复位 (HIGH=复位) | LOW |
| 39 | ETH_RST | W5500 以太网复位 (LOW=复位) | HIGH |
| 40 | AGX_RECOVERY | AGX 恢复模式 (HIGH=恢复) | LOW |
| 46 | LPMU_POWER | LPMU 电源键 (HIGH=按下) | LOW |
| 48 | USB_MUX_1 | USB MUX 选择位1 | LOW |

### 示例

```bash
# 列出所有可控引脚
gpio --list

# 查看引脚详情
gpio --info 3

# 设置高电平
gpio 1 high

# 设置低电平
gpio 3 low

# 输出 500ms 正脉冲（常用于复位）
gpio 1 pulse 500

# 输出 100ms 负脉冲
gpio 39 pulse 100 -n

# 设置高电平并保持 2 秒后恢复
gpio 1 high 2000

# 切换电平
gpio 46 toggle

# 读取引脚状态（切换为输入模式）
gpio 3 input

# 重置引脚到默认状态
gpio 1 reset

# JSON 格式输出
gpio --list --json
gpio --info 3 --json
```

### 典型用例

**AGX 复位**（发送 500ms 复位脉冲）：
```bash
gpio 1 pulse 500
```

**AGX 强制关机**（拉高 GPIO3）：
```bash
gpio 3 high
```

**AGX 允许开机**（拉低 GPIO3）：
```bash
gpio 3 low
```

**LPMU 电源键模拟按下**（500ms 脉冲）：
```bash
gpio 46 pulse 500
```

**W5500 复位**（负脉冲，LOW 有效）：
```bash
gpio 39 pulse 100 -n
```

---

## power - 电源管理

查看电源监控数据和管理低电压保护功能。

### 语法

```
power [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | `-s` | 显示电源状态 |
| `--voltage` | | 显示当前电压 |
| `--current` | | 显示当前电流 |
| `--power` | | 显示当前功率 |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 示例

```bash
# 显示电源状态
power --status

# 显示当前电压
power --voltage

# JSON 格式输出
power --status --json
```

---

## voltprot - 低电压保护

管理低电压保护系统，当输入电压低于阈值时自动执行保护性关机。

### 语法

```
voltprot <command>
```

### 命令

| 命令 | 说明 |
|------|------|
| `status` | 显示保护状态 |
| `test` | 触发测试（模拟低电压） |
| `reset` | 重置状态（将重启 ESP32） |
| `debug` | 显示调试信息 |

### 保护阈值

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 低电压阈值 | 12.6V | 触发保护 |
| 恢复电压阈值 | 18.0V | 允许恢复 |
| 关机延时 | 60秒 | 低电压倒计时 |
| 恢复稳定时间 | 5秒 | 恢复前等待 |

### 状态说明

| 状态 | 说明 |
|------|------|
| 正常运行 | 电压正常 |
| 低电压保护 | 电压低于阈值，倒计时中 |
| 关机中 | 正在执行保护性关机 |
| 保护状态 | 设备已关机，等待恢复 |
| 电压恢复中 | 电压恢复，等待稳定 |

### 示例

```bash
# 查看保护状态
voltprot status

# 触发测试（模拟低电压）
voltprot test

# 重置保护状态（将重启系统）
voltprot reset

# 显示调试信息
voltprot debug
```

---

## key - 安全密钥存储

管理存储在 ESP32 NVS 加密分区中的密钥。密钥不仅用于 SSH，还可用于 HTTPS/TLS 证书、API 签名、设备认证等场景。

### 语法

```
key [options]
```

### 选项

| 选项 | 说明 |
|------|------|
| `--list` | 列出所有存储的密钥 |
| `--info --id <name>` | 查看密钥详情 |
| `--import --id <name> --file <path>` | 从文件导入密钥 |
| `--generate --id <name> --type <type>` | 生成新密钥并存储 |
| `--delete --id <name>` | 删除密钥 |
| `--export --id <name> --output <path>` | 导出公钥到文件 |
| `--export-priv --id <name> --output <path>` | 导出私钥到文件（仅限可导出密钥） |
| `--exportable` | 允许私钥导出（与 --generate/--import 配合使用） |
| `--comment <text>` | 密钥注释（用于 generate/import） |
| `--json` | JSON 格式输出 |
| `--help` | 显示帮助 |

### 密钥类型

| 类型 | 说明 |
|------|------|
| `rsa`, `rsa2048` | RSA 2048 位（兼容性好） |
| `rsa4096` | RSA 4096 位（最安全，生成较慢 ~60s） |
| `ecdsa`, `ec256` | ECDSA P-256（快速、安全，推荐） |
| `ec384` | ECDSA P-384（高安全性） |

### 示例

```bash
# 列出所有密钥
key --list

# 生成 ECDSA 密钥并存储（默认不可导出私钥）
key --generate --id agx --type ecdsa --comment "AGX production key"

# 生成可导出的密钥（允许后续导出私钥用于备份）
key --generate --id backup_key --type rsa --exportable --comment "Backup key"

# 生成 RSA 4096 密钥（需要等待约 60 秒）
key --generate --id secure --type rsa4096

# 从文件导入已有密钥（默认不可再导出）
key --import --id legacy --file /sdcard/id_rsa

# 导入并保持可导出（用于密钥迁移场景）
key --import --id migrate --file /sdcard/id_rsa --exportable

# 查看密钥详情（显示是否可导出）
key --info --id agx

# 导出公钥（用于部署到远程服务器）
key --export --id agx --output /sdcard/agx.pub

# 导出私钥（仅限可导出密钥）
key --export-priv --id backup_key --output /sdcard/backup_key.pem

# 删除密钥
key --delete --id old_key

# JSON 格式输出
key --list --json
```

### 安全存储优势

- **硬件保护**：密钥存储在 ESP32 的 NVS 加密分区
- **无需文件**：避免私钥文件泄露风险
- **通用用途**：SSH、HTTPS、API 签名等均可使用
- **使用方便**：SSH 通过 `--keyid` 直接引用密钥

### 与 SSH 命令配合使用

```bash
# 1. 生成密钥
key --generate --id agx --type ecdsa

# 2. 导出公钥用于部署
key --export --id agx --output /sdcard/agx.pub

# 3. 部署公钥到远程服务器
ssh --copyid --host 192.168.1.100 --user nvidia --password secret --key /sdcard/agx

# 4. 使用密钥连接
ssh --host 192.168.1.100 --user nvidia --keyid agx --shell
```

---

## ssh - SSH 客户端

远程 SSH 连接、命令执行、交互式 Shell 和端口转发。密钥管理请使用 `key` 命令。

### 语法

```
ssh [options]
```

### 连接选项

| 选项 | 说明 |
|------|------|
| `--host <ip>` | 远程主机地址 |
| `--port <num>` | SSH 端口（默认 22） |
| `--user <name>` | 用户名 |
| `--password <pwd>` | 密码（密码认证） |
| `--key <path>` | 私钥文件路径（公钥认证） |
| `--keyid <id>` | 使用安全存储中的密钥（参见 `key` 命令） |
| `--exec <cmd>` | 执行远程命令 |
| `--shell` | 启动交互式 Shell |
| `--forward <spec>` | 端口转发（格式：`L<local>:<remote_host>:<remote_port>`） |
| `--test` | 测试 SSH 连接 |
| `--timeout <sec>` | 连接超时（秒） |
| `--verbose` | 详细输出 |

### 密钥文件生成选项

| 选项 | 说明 |
|------|------|
| `--keygen` | 生成 SSH 密钥对到文件 |
| `--type <type>` | 密钥类型：`rsa`, `rsa2048`, `rsa4096`, `ecdsa`, `ec256`, `ec384` |
| `--output <path>` | 私钥输出路径 |
| `--comment <text>` | 公钥注释（可选） |

### 密钥部署选项

| 选项 | 说明 |
|------|------|
| `--copyid` | 部署公钥到远程服务器（类似 ssh-copy-id） |

使用 `--copyid` 时需要同时提供：
- `--host` - 目标服务器
- `--user` - 用户名
- `--password` - 密码（用于初始认证）
- `--key` 或 `--keyid` - 密钥来源

### 公钥撤销选项

| 选项 | 说明 |
|------|------|
| `--revoke` | 从远程服务器撤销（删除）已部署的公钥 |

使用 `--revoke` 时需要同时提供：
- `--host` - 目标服务器
- `--user` - 用户名
- `--password` - 密码（用于认证，因为要删除的密钥可能已失效）
- `--key` 或 `--keyid` - 要撤销的密钥

### 连接示例

```bash
# 使用密码认证执行命令
ssh --host 192.168.1.100 --user root --password secret --exec "uptime"

# 使用安全存储的密钥连接（推荐）
ssh --host 192.168.1.100 --user nvidia --keyid agx --shell
ssh --host 192.168.1.100 --user nvidia --keyid agx --exec "nvidia-smi"

# 使用文件密钥连接
ssh --host 192.168.1.100 --user nvidia --key /sdcard/id_ecdsa --exec "hostname"

# 测试连接
ssh --test --host 192.168.1.100 --user root --keyid agx

# 本地端口转发
ssh --host gateway.local --user admin --keyid agx --forward "L8080:localhost:80"
```

### 密钥文件生成示例

```bash
# 生成 ECDSA 密钥对到文件（推荐）
ssh --keygen --type ecdsa --output /sdcard/id_ecdsa --comment "TianShanOS"

# 生成 RSA 4096 位密钥对
ssh --keygen --type rsa4096 --output /sdcard/id_rsa
```

生成的文件：
- 私钥：`<output>` (PEM 格式)
- 公钥：`<output>.pub` (OpenSSH 格式)

### 公钥部署示例

```bash
# 部署公钥到远程服务器（使用密码进行初始认证）
ssh --copyid --host 192.168.1.100 --user nvidia --password secret --key /sdcard/id_ecdsa
```

### 交互式 Shell

使用 `--shell` 选项启动交互式 Shell：
- 支持 PTY（伪终端），完整的终端体验
- 支持 Ctrl+C 发送中断信号
- 支持 Ctrl+D 退出 Shell
- 实时字符回显

### 端口转发

端口转发格式：`L<localport>:<remotehost>:<remoteport>`

```bash
# 将本地 8080 端口转发到远程的 internal.server:3000
ssh --host bastion --user admin --keyid agx --forward "L8080:internal.server:3000"
```

### Known Hosts 验证

SSH 连接会自动验证远程主机的身份，防止中间人攻击。

**首次连接**（TOFU - Trust On First Use）：
```
┌─────────────────────────────────────────────────────────────┐
│  NEW HOST - First time connecting to this server           │
└─────────────────────────────────────────────────────────────┘

Host: 10.10.99.100:22
Fingerprint: SHA256:xYzAbC123...

Trust this host and continue? (yes/no):
```

输入 `yes` 后指纹保存到 NVS，后续连接自动验证。

**指纹变化警告**（可能的中间人攻击）：
```
┌─────────────────────────────────────────────────────────────┐
│  ⚠️  WARNING: HOST KEY MISMATCH - POSSIBLE MITM ATTACK!    │
└─────────────────────────────────────────────────────────────┘

The host key for 10.10.99.100 has changed!
Stored:  SHA256:OldKey...
Current: SHA256:NewKey...

Accept new key? (yes/no):
```

> **提示**：使用 `hosts` 命令管理已保存的主机指纹。参见 [hosts 命令](#hosts---ssh-known-hosts-管理)。

### 典型工作流（推荐使用安全存储）

1. **生成密钥并存储到安全区域**：
   ```bash
   key --generate --id agx --type rsa --comment "AGX production key"
   ```
   > 提示：密钥生成需要几秒钟，RSA-4096 可能需要 60 秒以上

2. **部署公钥到目标服务器**（使用密码进行初始认证）：
   ```bash
   ssh --copyid --host 192.168.1.100 --user nvidia --password secret --keyid agx
   ```
   > 这一步会将安全存储中的公钥部署到服务器的 `~/.ssh/authorized_keys`

3. **使用安全存储的密钥连接**（无需密码）：
   ```bash
   # 执行单条命令
   ssh --host 192.168.1.100 --user nvidia --keyid agx --exec "nvidia-smi"
   
   # 启动交互式 Shell
   ssh --host 192.168.1.100 --user nvidia --keyid agx --shell
   ```

### 完整命令示例

```bash
# ========== 密钥管理 ==========

# 列出所有存储的密钥
key --list

# 生成 RSA 2048 密钥（推荐，兼容性好）
key --generate --id my_server --type rsa --comment "Server access key"

# 生成 ECDSA P-256 密钥（更快，但某些旧服务器不支持）
key --generate --id fast_key --type ecdsa

# 查看密钥详情
key --info --id my_server

# 导出公钥到文件（可用于手动部署）
key --export --id my_server --output /sdcard/my_server.pub

# 删除不需要的密钥
key --delete --id old_key

# ========== SSH 连接 ==========

# 使用安全存储的密钥连接（推荐）
ssh --host 10.10.99.100 --user thomas --keyid my_server --shell
ssh --host 10.10.99.100 --user thomas --keyid my_server --exec "uptime"

# 使用密码连接（不推荐，仅用于初始配置）
ssh --host 10.10.99.100 --user thomas --password secret --exec "whoami"

# 使用文件密钥连接
ssh --host 10.10.99.100 --user thomas --key /sdcard/id_rsa --shell

# 测试连接
ssh --test --host 10.10.99.100 --user thomas --keyid my_server

# ========== 密钥部署 ==========

# 从安全存储部署密钥（推荐）
ssh --copyid --host 10.10.99.100 --user thomas --password cdromdir --keyid my_server

# 从文件部署密钥
ssh --copyid --host 10.10.99.100 --user thomas --password cdromdir --key /sdcard/id_rsa

# ========== 公钥撤销 ==========

# 撤销已部署的公钥（从服务器的 authorized_keys 中删除）
ssh --revoke --host 10.10.99.100 --user thomas --password cdromdir --keyid my_server

# ========== 端口转发 ==========

# 本地端口转发：访问本机 8080 → 转发到远程 localhost:80
ssh --host gateway --user admin --keyid gw_key --forward "L8080:localhost:80"
```

### 安全存储 vs 文件密钥

| 特性 | 安全存储 (`--keyid`) | 文件密钥 (`--key`) |
|------|---------------------|-------------------|
| 私钥保护 | ✅ NVS 加密分区 | ⚠️ 明文存储在 SD 卡 |
| 使用便捷性 | ✅ 只需记住 ID | 需要管理文件路径 |
| 密钥备份 | ⚠️ 需标记 `--exportable` | ✅ 可以复制文件 |
| 推荐场景 | 生产环境、安全敏感场景 | 临时使用、密钥迁移 |

---

## hosts - SSH 主机管理

管理 SSH 相关的主机信息，包括两类数据：

1. **已知主机指纹（Known Hosts）**：SSH 服务器的指纹记录，用于验证服务器身份，防止中间人攻击
2. **已部署主机记录（Deployed Hosts）**：记录哪些远程主机已部署了我们的公钥

### 语法

```
hosts [options]
```

### 选项

| 选项 | 说明 |
|------|------|
| `--list` | 列出所有已保存的主机指纹 |
| `--info --host <host>` | 查看特定主机的详细信息 |
| `--remove --host <host>` | 删除特定主机的指纹记录 |
| `--clear` | 清除所有主机指纹（需 `--yes` 确认） |
| `--deployed` | 操作已部署主机记录（而非指纹） |
| `--id <id>` | 指定已部署主机 ID |
| `--port <num>` | 指定端口（与 --info/--remove 配合，默认 22） |
| `--yes` | 确认危险操作 |
| `--json` | JSON 格式输出 |
| `--help` | 显示帮助 |

### 已知主机指纹管理

```bash
# 列出所有已知主机指纹
hosts --list

# 列出已知主机（JSON 格式）
hosts --list --json

# 查看特定主机信息
hosts --info --host 10.10.99.100

# 查看非默认端口的主机
hosts --info --host 10.10.99.100 --port 2222

# 删除特定主机记录（服务器重装后需要）
hosts --remove --host 10.10.99.100

# 删除非默认端口的主机记录
hosts --remove --host 10.10.99.100 --port 2222

# 清除所有主机记录（需要确认）
hosts --clear --yes
```

### 已部署主机管理

```bash
# 列出所有已部署公钥的主机
hosts --deployed

# 列出已部署主机（JSON 格式）
hosts --deployed --json

# 移除已部署主机记录（仅删除本地记录，不撤销远程公钥）
hosts --deployed --remove --id agx@10.10.99.100

# 清除所有已部署主机记录
hosts --deployed --clear --yes
```

### 输出示例

**列出已知主机指纹**：
```
SSH Known Hosts
══════════════════════════════════════════════════════════════════════════
  Host              Port   Type       Fingerprint (SHA256)
  ──────────────────────────────────────────────────────────────────────────
  10.10.99.100      22     ssh-rsa    xYzAbC123...
  192.168.1.50      22     ssh-ed25519 DefGhI456...
══════════════════════════════════════════════════════════════════════════
  Total: 2 hosts
```

**列出已部署主机**：
```
Deployed SSH Hosts (Public Key Deployed)
══════════════════════════════════════════════════════════════════════════
  ID               Host                 User     Deployed   Key
  ──────────────────────────────────────────────────────────────────────────
  agx@10.10.99.100 10.10.99.100        nvidia   2026-01-27 agx
══════════════════════════════════════════════════════════════════════════
  Total: 1 deployed hosts
```

### 使用场景

1. **服务器重装后**：删除旧指纹，允许接受新指纹
   ```bash
   hosts --remove --host 10.10.99.100
   ssh --host 10.10.99.100 --user nvidia --keyid agx --shell
   ```

2. **密钥删除后清理**：如果删除了一个密钥，相关的已部署主机记录仍存在，需手动清理
   ```bash
   # 先查看有哪些已部署主机使用了该密钥
   hosts --deployed
   # 移除本地记录（远程公钥需单独处理或保留）
   hosts --deployed --remove --id agx@10.10.99.100
   ```

3. **正常撤销流程**：先撤销远程公钥，记录会自动删除
   ```bash
   ssh --revoke --id agx@10.10.99.100
   ```

4. **批量清理**：测试环境重置
   ```bash
   hosts --clear --yes
   hosts --deployed --clear --yes
   ```

### 安全说明

- **已知主机指纹**：自动在首次 SSH 连接时添加，用于验证服务器身份。如果指纹变化会警告可能的中间人攻击
- **已部署主机记录**：记录哪些主机已部署了公钥，方便管理和撤销。删除记录**不会**从远程主机移除公钥

---

## ota - OTA 固件升级

管理 Over-The-Air (OTA) 固件升级，支持 HTTPS 下载和 SD 卡本地升级。

### 语法

```
ota [options]
```

### 选项

| 选项 | 简写 | 说明 |
|------|------|------|
| `--status` | | 显示 OTA 状态 |
| `--progress` | | 显示升级进度 |
| `--version` | | 显示固件版本 |
| `--partitions` | | 显示分区信息 |
| `--url <url>` | | 从 HTTPS URL 升级 |
| `--file <path>` | | 从 SD 卡文件升级 |
| `--validate` | | 标记当前固件有效（取消回滚） |
| `--rollback` | | 回滚到上一版本 |
| `--abort` | | 中止当前升级 |
| `--no-reboot` | | 升级后不自动重启 |
| `--allow-downgrade` | | 允许降级到旧版本 |
| `--skip-verify` | | 跳过 HTTPS 证书验证（仅调试） |
| `--json` | `-j` | JSON 格式输出 |
| `--help` | `-h` | 显示帮助 |

### 示例

**查看 OTA 状态**：
```bash
ota --status
```

输出示例：
```
╔════════════════════════════════════════╗
║           OTA 状态信息                  ║
╠════════════════════════════════════════╣
║ 当前状态: idle                         ║
╠════════════════════════════════════════╣
║ 运行分区: ota_0                        ║
║ 版本:     0.1.0                        ║
║ 项目:     TianShanOS                   ║
║ 编译日期: Jan 22 2026                  ║
║ IDF版本:  v5.5.1                       ║
╠════════════════════════════════════════╣
║ 下一分区: ota_1                        ║
║ 可启动:   否                           ║
╚════════════════════════════════════════╝
```

**查看固件版本**：
```bash
ota --version
```

**查看分区信息**：
```bash
ota --partitions
```

输出示例：
```
╔══════════════════════════════════════════════════════╗
║                  OTA 分区信息                         ║
╠══════════════════════════════════════════════════════╣
║ 分区        地址         大小       状态              ║
╠══════════════════════════════════════════════════════╣
║ ota_0       0x00020000   3145728    [运行中]         ║
║   版本: 0.1.0                                        ║
╠══════════════════════════════════════════════════════╣
║ ota_1       0x00320000   3145728    [空闲]           ║
╚══════════════════════════════════════════════════════╝
```

**从 URL 升级**：
```bash
# 标准升级（自动重启）
ota --url https://example.com/firmware/tianshanos-v0.2.0.bin

# 不自动重启
ota --url https://example.com/firmware.bin --no-reboot

# 允许降级
ota --url https://example.com/firmware.bin --allow-downgrade

# 跳过证书验证（仅测试环境）
ota --url https://self-signed.example.com/firmware.bin --skip-verify
```

**从 SD 卡升级**：
```bash
# 从 SD 卡升级
ota --file /sdcard/firmware.bin

# 允许降级
ota --file /sdcard/old-firmware.bin --allow-downgrade
```

**查看升级进度**：
```bash
ota --progress
```

输出示例：
```
状态: downloading
进度: 45%
已下载: 921600 / 2048000 字节
消息: 正在下载...
[██████████████████░░░░░░░░░░░░░░░░░░░░░░] 45%
```

**确认新固件有效**：
```bash
# 升级后，新固件启动，需在 60 秒内确认
ota --validate
```

**回滚到上一版本**：
```bash
ota --rollback
```

**中止升级**：
```bash
ota --abort
```

### OTA 工作流程

1. **标准升级流程**：
   ```bash
   # 1. 查看当前版本
   ota --version
   
   # 2. 启动升级
   ota --url https://ota.example.com/firmware.bin
   
   # 3. 设备自动重启到新固件
   
   # 4. 新固件启动后，确认有效
   ota --validate
   ```

2. **回滚保护**：
   - 新固件启动后，默认 60 秒内必须调用 `ota --validate`
   - 超时未确认会自动回滚到上一版本
   - 可在 Kconfig 中配置超时时间

3. **SD 卡升级**：
   ```bash
   # 将固件复制到 SD 卡
   # 然后执行
   ota --file /sdcard/firmware.bin
   ```

### OTA 事件

OTA 过程中会发布以下事件（事件基础：`ts_ota`）：

| 事件 ID | 说明 |
|---------|------|
| `TS_EVENT_OTA_STARTED` | OTA 升级开始 |
| `TS_EVENT_OTA_PROGRESS` | 进度更新 |
| `TS_EVENT_OTA_COMPLETED` | 升级完成 |
| `TS_EVENT_OTA_FAILED` | 升级失败 |
| `TS_EVENT_OTA_ABORTED` | 升级中止 |
| `TS_EVENT_OTA_PENDING_REBOOT` | 等待重启 |
| `TS_EVENT_OTA_ROLLBACK_PENDING` | 待验证，回滚计时中 |
| `TS_EVENT_OTA_ROLLBACK_EXECUTED` | 执行了回滚 |
| `TS_EVENT_OTA_VALIDATED` | 固件已验证 |

---

## 命令状态总览

| 命令 | 状态 | 说明 |
|------|------|------|
| `system` | ✅ 可用 | 完全实现 |
| `service` | ✅ 可用 | 完全实现 |
| `config` | ✅ 可用 | 完全实现 |
| `led` | ✅ 可用 | touch/board/matrix 全部可用 |
| `fan` | ✅ 可用 | 支持 Fan 0（GPIO 41） |
| `storage` | ✅ 可用 | SPIFFS 可用，SD 卡需插入 |
| `net` | ✅ 可用 | 以太网完全可用 |
| `dhcp` | ✅ 可用 | 完整 DHCP 服务器，支持静态绑定 |
| `wifi` | ✅ 可用 | AP/STA 模式，扫描、连接、热点 |
| `fs` | ✅ 可用 | 文件系统操作（ls/cat/rm/mkdir 等） |
| `device` | ⚠️ 模拟 | 未接入真实驱动 |
| `gpio` | ✅ 可用 | GPIO 直接控制（9 个设备控制引脚） |
| `power` | ✅ 可用 | 电源监控（电压/电流/功率） |
| `voltprot` | ✅ 可用 | 低电压保护系统 |
| `key` | ✅ 可用 | 安全密钥存储（NVS 加密分区），支持 exportable 标记 |
| `ssh` | ✅ 可用 | 命令执行、Shell、端口转发、密钥部署、公钥撤销 |
| `hosts` | ✅ 可用 | SSH Known Hosts 管理（TOFU 验证） |
| `ota` | ✅ 可用 | OTA 固件升级（HTTPS/SD卡、回滚保护） |

---

## 通用说明

### JSON 输出

所有命令都支持 `--json` 或 `-j` 选项，输出机器可读的 JSON 格式，便于脚本解析或 API 集成。

### 帮助信息

所有命令都支持 `--help` 或 `-h` 选项，显示命令的详细使用说明。

### 命令风格

TianShanOS CLI 使用**参数风格**（非 POSIX 子命令风格）：

```bash
# 正确 ✓
service --start --name fan_controller
fan --speed 75 --id 0

# 错误 ✗
service start fan_controller
fan set speed 75
```

---

## 版本信息

- **文档版本**: 1.7.0
- **适用版本**: TianShanOS v0.1.0+
- **最后更新**: 2026-01-22
- **变更记录**:
  - v1.7.0: 新增 `ota` 命令（OTA 固件升级、HTTPS/SD卡升级、回滚保护）
  - v1.6.0: SSH Shell WebSocket 支持、Web 终端页面、电压保护事件广播
  - v1.5.0: 新增 `gpio` 命令（GPIO 直接控制）、`power` 命令（电源监控）、`voltprot` 命令（低电压保护）
  - v1.4.0: 新增 `hosts` 命令、`ssh --revoke` 公钥撤销、`key --exportable` 私钥导出控制
  - v1.3.0: 新增独立的 `key` 命令用于密钥管理（从 `ssh --keys` 分离）
