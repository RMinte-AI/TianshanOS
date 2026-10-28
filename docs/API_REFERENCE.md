# TianShanOS API 参考文档

## 概述

TianShanOS 提供统一的 API 接口，支持通过控制台命令或 REST API 调用。

## 认证

### 登录
```
POST /api/v1/auth/login
Content-Type: application/json

{
  "username": "admin",
  "password": "rm01"
}

Response:
{
  "token": "...",
  "session_id": 12345
}
```

### Root 设置 admin 新密码
```
POST /api/v1/auth/admin/set_password
Content-Type: application/json

{
  "token": "<root-token>",
  "new_password": "new-password"
}

Response:
{
  "code": 0,
  "data": {
    "success": true,
    "username": "admin",
    "password_changed": true
  }
}
```

### Root 重置 admin 为默认密码
```
POST /api/v1/auth/admin/reset_password
Content-Type: application/json

{
  "token": "<root-token>"
}

Response:
{
  "code": 0,
  "data": {
    "success": true,
    "username": "admin",
    "password_changed": false
  }
}
```

### 登出
```
POST /api/v1/auth/logout
Authorization: Bearer <token>

Response:
{
  "success": true
}
```

## 系统 API

### system.info
获取系统信息。

**请求**: `GET /api/v1/system/info`

**响应**:
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "chip": "ESP32S3",
    "version": "0.1.0",
    "uptime_ms": 123456,
    "free_heap": 200000,
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

### system.memory
获取内存信息。

**请求**: `GET /api/v1/system/memory`

**响应**:
```json
{
  "code": 0,
  "data": {
    "total_heap": 327680,
    "free_heap": 200000,
    "min_free_heap": 180000,
    "largest_block": 65536
  }
}
```

### system.tasks
获取任务列表。

**请求**: `GET /api/v1/system/tasks`

**响应**:
```json
{
  "code": 0,
  "data": {
    "tasks": [
      {"name": "main", "state": "Running", "priority": 5, "stack_free": 2048},
      {"name": "IDLE", "state": "Ready", "priority": 0, "stack_free": 1024}
    ]
  }
}
```

### system.reboot
重启系统。

**请求**: `POST /api/v1/system/reboot`

**响应**:
```json
{
  "code": 0,
  "message": "Rebooting..."
}
```

### system.log.level
设置日志级别。

**请求**:
```
POST /api/v1/system/log/level
{
  "level": "debug"
}
```

**响应**:
```json
{
  "code": 0,
  "message": "Log level set to debug"
}
```

## 配置 API

### config.get
获取配置值。

**请求**: `GET /api/v1/config/get?key=wifi.ssid`

**响应**:
```json
{
  "code": 0,
  "data": {
    "key": "wifi.ssid",
    "value": "MyNetwork"
  }
}
```

### config.set
设置配置值。

**请求**:
```
POST /api/v1/config/set
{
  "key": "wifi.ssid",
  "value": "NewNetwork"
}
```

**响应**:
```json
{
  "code": 0,
  "message": "Configuration saved"
}
```

### config.delete
删除配置项。

**请求**:
```
POST /api/v1/config/delete
{
  "key": "wifi.ssid"
}
```

### config.list
列出配置项。

**请求**: `GET /api/v1/config/list?prefix=wifi`

**响应**:
```json
{
  "code": 0,
  "data": {
    "items": [
      {"key": "wifi.ssid", "value": "MyNetwork"},
      {"key": "wifi.password", "value": "***"}
    ]
  }
}
```

### config.save
保存配置到存储。

**请求**: `POST /api/v1/config/save`

## 变量 API

变量系统用于存储和管理动态数据，主要应用场景：
- SSH 命令执行结果存储
- 自动化引擎数据源
- 命令参数动态替换

### automation.variables.get
获取变量值。

**请求**:
```json
POST /api/v1/automation.variables.get
{
  "name": "cpu_temp.extracted"
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "name": "cpu_temp.extracted",
    "value": "45000"
  }
}
```

**错误响应** (变量不存在):
```json
{
  "code": 404,
  "message": "Variable not found: cpu_temp.extracted"
}
```

### automation.variables.set
设置变量值。

**请求**:
```json
POST /api/v1/automation.variables.set
{
  "name": "my_variable",
  "value": "hello world",
  "create_only": false
}
```

**参数**:
| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 变量名（最大 64 字符） |
| `value` | bool/int/float/string/null | 是 | 变量值 |
| `create_only` | bool | 否 | 仅在不存在时创建，不覆盖已有变量 |

**响应**:
```json
{
  "code": 0,
  "message": "Variable set successfully",
  "data": {
    "name": "my_variable",
    "value": "hello world"
  }
}
```

### automation.variables.list
列出自动化变量，支持按前缀或来源过滤。默认返回变量值和更新时间元数据；资源敏感的选择器类 UI 可传 `include_value:false, include_meta:false`。

**请求**:
```json
POST /api/v1/automation.variables.list
{
  "prefix": "agx.",
  "source_id": "agx",
  "include_value": true,
  "include_meta": true
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "count": 3,
    "variables": [
      {
        "name": "agx.tj.temp",
        "value": 72.5,
        "type": "float",
        "persistent": false,
        "readonly": false,
        "last_change_ms": 123456,
        "last_update_ms": 123900,
        "age_ms": 250,
        "stale": false,
        "source_id": "agx"
      },
      {
        "name": "cpu_temp.status",
        "value": "running",
        "type": "string",
        "persistent": false,
        "readonly": false,
        "last_change_ms": 123000,
        "last_update_ms": 123900,
        "age_ms": 250,
        "stale": false,
        "source_id": "cpu_temp"
      }
    ]
  }
}
```

### 变量类型

| 类型 | 说明 | 示例 |
|------|------|------|
| `string` | 字符串 | `"hello"` |
| `int` | 整数 | `45000` |
| `float` | 浮点数 | `72.5` |
| `bool` | 布尔值 | `true`, `false` |

### 变量来源

| 来源 | 说明 |
|------|------|
| `user` | 用户通过 API 或 CLI 设置 |
| `ssh` | SSH 命令执行结果自动生成 |
| `system` | 系统内置变量（如 `sys.time`） |
| `alias` | 指向其他变量的别名 |

### SSH 结果自动变量

当 SSH 命令执行时指定 `var_name` 参数，系统自动创建以下变量：

| 变量名 | 说明 |
|--------|------|
| `${var_name}.extracted` | 从输出中提取的值 |
| `${var_name}.status` | 执行状态（success/error/timeout） |
| `${var_name}.exit_code` | 命令退出码 |
| `${var_name}.host` | 执行主机 |
| `${var_name}.timestamp` | 执行时间戳 |
| `${last}.extracted` | 最后一次执行的提取值 |
| `${last}.*` | 最后一次执行的所有属性 |

### 变量替换语法

在支持变量替换的场景中，使用 `${变量名}` 语法：

```
// 示例命令
echo "CPU temperature: ${cpu_temp.extracted}"

// 替换后
echo "CPU temperature: 45000"
```

## LED API

### led.devices
获取 LED 设备列表。

**请求**: `GET /api/v1/led/devices`

**响应**:
```json
{
  "code": 0,
  "data": {
    "devices": [
      {"name": "touch", "count": 16, "brightness": 80},
      {"name": "board", "count": 32, "brightness": 60}
    ]
  }
}
```

### led.brightness
设置亮度。

**请求**:
```
POST /api/v1/led/brightness
{
  "device": "touch",
  "brightness": 100
}
```

### led.color
设置颜色。

**请求**:
```
POST /api/v1/led/color
{
  "device": "touch",
  "color": "#FF0000"
}
```

### led.effect
设置效果。

**请求**:
```
POST /api/v1/led/effect
{
  "device": "touch",
  "effect": "rainbow"
}
```

可用效果: `rainbow`, `breathing`, `chase`, `fire`, `sparkle`, `solid`

## 网络 API

### network.status
获取网络状态。

**请求**: `GET /api/v1/network/status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "eth_connected": true,
    "wifi_connected": false,
    "ip": "192.168.1.100",
    "gateway": "192.168.1.1",
    "dns": "8.8.8.8"
  }
}
```

### network.wifi.scan
扫描 WiFi 网络。

**请求**: `GET /api/v1/network/wifi/scan`

**响应**:
```json
{
  "code": 0,
  "data": {
    "networks": [
      {"ssid": "Network1", "rssi": -45, "channel": 6, "auth": "WPA2"},
      {"ssid": "Network2", "rssi": -70, "channel": 11, "auth": "OPEN"}
    ]
  }
}
```

### network.wifi.connect
连接 WiFi。

**请求**:
```
POST /api/v1/network/wifi/connect
{
  "ssid": "MyNetwork",
  "password": "password123"
}
```

## 设备 API

### device.status
获取设备状态。

**请求**: `GET /api/v1/device/status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "agx_powered": true,
    "agx_uptime_ms": 3600000,
    "power_good": true
  }
}
```

### device.power
控制设备电源。

**请求**:
```
POST /api/v1/device/power
{
  "device": "agx",
  "on": true
}
```

### device.fan.status
获取风扇状态。

**请求**: `GET /api/v1/device/fan/status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "fans": [
      {
        "id": 0,
        "mode": "auto",
        "duty": 50,
        "target_duty": 55,
        "rpm": 2500,
        "temp": 58.2,
        "running": true,
        "control_temperature": 58.2,
        "guard_temperature": 72.5,
        "predicted_temperature": 64.0,
        "slope_c_per_min": 0.8,
        "controller_gain": 0.75,
        "cooling_response": 0.4,
        "auto_state": "active",
        "guard_active": false,
        "temp_stale": false
      }
    ]
  }
}
```

### device.fan.speed
设置风扇速度。

**请求**:
```
POST /api/v1/device/fan/speed
{
  "fan": 0,
  "speed": 80
}
```

## 风扇控制 API

### fan.status
获取风扇状态。

**请求**: `POST /api/v1/fan.status`

**参数**:
```json
{}                    // 获取所有风扇
{"id": 0}            // 获取指定风扇
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "fans": [
      {
        "id": 0,
        "mode": "auto",
        "duty": 30,
        "target_duty": 25,
        "rpm": 0,
        "temperature": 35.5,
        "enabled": true,
        "running": true,
        "fault": false,
        "control_temperature": 35.5,
        "guard_temperature": 52.3,
        "predicted_temperature": 58.0,
        "slope_c_per_min": 1.2,
        "controller_gain": 0.75,
        "cooling_response": 0.4,
        "auto_state": "active",
        "guard_active": false,
        "temp_stale": false
      }
    ],
    "temperature": 35.5,
    "temp_valid": true,
    "temp_source": "agx.cpu_temp"
  }
}
```

`control_temperature`、`guard_temperature`、`predicted_temperature`、`slope_c_per_min`、`controller_gain`、`cooling_response`、`auto_state`、`guard_active`、`temp_stale` 是自动模式的状态字段；非 `auto` 模式下这些字段仅表示上一次自动控制快照或默认值，不参与当前调速。

### fan.mode
设置风扇工作模式。

**请求**:
```json
POST /api/v1/fan.mode
{
  "id": 0,
  "mode": "curve"    // "off", "manual", "auto", "curve"
}
```

### fan.set
设置风扇速度（手动模式）。

**请求**:
```json
POST /api/v1/fan.set
{
  "id": 0,
  "duty": 75
}
```

### fan.curve
设置温度-转速曲线。

**请求**:
```json
POST /api/v1/fan.curve
{
  "id": 0,
  "curve": [
    {"temp": 30, "duty": 10},
    {"temp": 50, "duty": 40},
    {"temp": 70, "duty": 80}
  ],
  "hysteresis": 3.0,
  "min_interval": 2000
}
```

**参数说明**:
- `curve`: 温度-占空比曲线点（温度°C，占空比 0-100%）
- `hysteresis`: 温度迟滞，防止频繁调速（°C）
- `min_interval`: 最小调速间隔（毫秒）

### fan.limits
设置风扇占空比限制。

**请求**:
```json
POST /api/v1/fan.limits
{
  "id": 0,
  "min_duty": 10,
  "max_duty": 100
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "id": 0,
    "min_duty": 10,
    "max_duty": 100
  }
}
```

### fan.config
获取风扇完整配置。

**请求**:
```json
POST /api/v1/fan.config
{
  "id": 0
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "id": 0,
    "mode": "curve",
    "min_duty": 10,
    "max_duty": 100,
    "hysteresis": 3.0,
    "min_interval": 2000,
    "invert_pwm": false,
    "curve": [
      {"temp": 30, "duty": 10},
      {"temp": 50, "duty": 40},
      {"temp": 70, "duty": 80}
    ]
  }
}
```

### fan.save
保存风扇配置到 NVS。

**请求**:
```json
POST /api/v1/fan.save
{"id": 0}   // 保存指定风扇
{}          // 保存所有风扇
```

## 温度源 API

### temp.status
获取温度状态。

**请求**: `POST /api/v1/temp.status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "initialized": true,
    "manual_mode": false,
    "active_source": "variable",
    "preferred_source": "variable",
    "bound_variable": "agx.cpu_temp",
    "temperature_01c": 355,
    "temperature_c": 35.5,
    "valid": true,
    "timestamp_ms": 123900
  }
}
```

### temp.bind
绑定温度变量，支持单变量兼容模式和多变量加权模式。

**请求**:
```json
POST /api/v1/temp.bind
{
  "variable": "agx.cpu_temp"   // 绑定变量
}

// 加权绑定
POST /api/v1/temp.bind
{
  "variables": [
    { "name": "agx.cpu_temp", "weight": 0.4 },
    { "name": "agx.gpu_temp", "weight": 0.6 }
  ]
}

// 解除绑定
POST /api/v1/temp.bind
{
  "variable": null
}

// 使用新格式解除绑定
POST /api/v1/temp.bind
{
  "variables": []
}
```

**说明**:
- `variables` 为新格式，优先级高于 `variable`
- 每个 `weight` 取值范围为 `0.0 ~ 1.0`
- 所有权重之和必须大于 `0`
- 旧格式 `variable` 仍然兼容，等价于单变量且权重为 `1.0`
- 只有所有正权重绑定变量都 fresh 且数值有效时才返回 `weighted_temp_c`
- 任一正权重绑定变量过期或无效时，响应会标记 `partial_stale: true`
- `weighted_temp_c` 是绑定变量的加权温度；`temperature_c` / `valid` 始终表示当前活动温度源状态

**响应示例**:
```json
{
  "code": 0,
  "data": {
    "bound_variable": "agx.cpu_temp",
    "bound_variables": [
      {
        "name": "agx.cpu_temp",
        "weight": 0.4,
        "value": 52.3,
        "last_update_ms": 123456,
        "age_ms": 250,
        "stale": false,
        "valid": true
      },
      {
        "name": "agx.gpu_temp",
        "weight": 0.6,
        "value": 49.8,
        "last_update_ms": 123460,
        "age_ms": 246,
        "stale": false,
        "valid": true
      }
    ],
    "bound_valid_count": 2,
    "bound_total_count": 2,
    "bound_valid_weight": 1.0,
    "bound_total_weight": 1.0,
    "partial_stale": false,
    "weighted_temp_c": 50.8,
    "active_source": "variable",
    "temperature_c": 50.8,
    "valid": true
  }
}
```

### temp.select
选择温度源。

**请求**:
```json
POST /api/v1/temp.select
{
  "source": "variable"   // "default", "sensor", "agx", "variable"
}
```

### temp.manual
设置测试温度（手动模式）。

**请求**:
```json
POST /api/v1/temp.manual
{
  "enable": true,
  "temperature": 45.0    // 测试温度 (°C)
}

// 禁用手动模式
POST /api/v1/temp.manual
{
  "enable": false
}
```

## 存储 API

### storage.status
获取存储状态。

**请求**: `GET /api/v1/storage/status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "spiffs": {
      "mounted": true,
      "total": 1048576,
      "used": 524288,
      "free": 524288
    },
    "sd": {
      "mounted": true,
      "total": 32212254720,
      "used": 1073741824,
      "free": 31138512896
    }
  }
}
```

### storage.list
列出目录内容。

**请求**: `GET /api/v1/storage/list?path=/sdcard`

**响应**:
```json
{
  "code": 0,
  "data": {
    "path": "/sdcard",
    "entries": [
      {"name": "config", "type": "dir"},
      {"name": "test.txt", "type": "file", "size": 1024}
    ]
  }
}
```

**错误响应**（SD 卡未挂载）:
```json
{
  "code": 3,
  "message": "SD card not mounted"
}
```

### storage.mount
挂载 SD 卡。

**请求**: `POST /api/v1/storage/mount`

**响应**:
```json
{
  "code": 0,
  "message": "SD card mounted successfully"
}
```

### storage.unmount
卸载 SD 卡。

**请求**: `POST /api/v1/storage/unmount`

**响应**:
```json
{
  "code": 0,
  "message": "SD card unmounted successfully"
}
```

### storage.delete
删除文件或目录。

**请求**:
```
POST /api/v1/storage/delete
{
  "path": "/sdcard/test.txt"
}
```

### storage.mkdir
创建目录。

**请求**:
```
POST /api/v1/storage/mkdir
{
  "path": "/sdcard/newfolder"
}
```

### storage.rename
重命名文件或目录。

**请求**:
```
POST /api/v1/storage/rename
{
  "old_path": "/sdcard/old.txt",
  "new_path": "/sdcard/new.txt"
}
```

### storage.upload
上传文件（multipart/form-data）。

**请求**: `POST /api/v1/storage/upload?path=/sdcard`

### storage.download
下载文件。

**请求**: `GET /api/v1/storage/download?path=/sdcard/file.txt`

## WebSocket API

连接 `ws://<device-ip>/ws` 进行实时通信。

### 订阅消息
```json
{"type": "subscribe"}
```

### 心跳
```json
{"type": "ping"}
```

响应:
```json
{"type": "pong"}
```

### 事件推送
服务器主动推送:
```json
{
  "type": "event",
  "event": "device.power_on",
  "data": {"device": "agx"}
}
```

### 日志订阅
订阅实时日志流：
```json
{"type": "log_subscribe", "minLevel": 3}
```
- `minLevel`: 最小日志级别（1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE）

订阅确认响应：
```json
{"type": "log_subscribed", "minLevel": 3}
```

### 取消日志订阅
```json
{"type": "log_unsubscribe"}
```

### 更新日志级别过滤
```json
{"type": "log_set_level", "minLevel": 2}
```

### 获取历史日志
```json
{"type": "log_get_history", "limit": 500, "minLevel": 1, "maxLevel": 5}
```

历史日志响应：
```json
{
  "type": "log_history",
  "logs": [
    {
      "timestamp": 12345678,
      "level": 3,
      "levelName": "INFO",
      "tag": "main",
      "message": "System started",
      "task": "main"
    }
  ],
  "total": 150
}
```

### 实时日志推送
服务器主动推送日志：
```json
{
  "type": "log",
  "timestamp": 12345678,
  "level": 3,
  "levelName": "INFO",
  "tag": "ts_net",
  "message": "Ethernet connected",
  "task": "net_task"
}
```

## 日志 API

### log.list
获取日志列表（带过滤）。

**请求**: `GET /api/v1/log/list?limit=50&minLevel=1&maxLevel=5`

**参数**:
| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| offset | number | 0 | 起始偏移 |
| limit | number | 50 | 返回数量（最大200） |
| minLevel | number | 1 | 最小级别 |
| maxLevel | number | 5 | 最大级别 |
| tag | string | - | TAG 过滤（子字符串匹配） |
| keyword | string | - | 关键字搜索 |

**响应**:
```json
{
  "code": 0,
  "data": {
    "logs": [
      {
        "timestamp": 12345678,
        "level": 3,
        "levelName": "INFO",
        "tag": "main",
        "message": "System started",
        "task": "main"
      }
    ],
    "total": 150,
    "offset": 0,
    "returned": 50,
    "bufferCapacity": 1000,
    "bufferCount": 150
  }
}
```

### log.stats
获取日志统计信息。

**请求**: `GET /api/v1/log/stats`

**响应**:
```json
{
  "code": 0,
  "data": {
    "bufferCapacity": 1000,
    "bufferCount": 150,
    "totalCaptured": 1523,
    "dropped": 0,
    "espLogCaptureEnabled": true,
    "currentLevel": 3,
    "currentLevelName": "INFO"
  }
}
```

### log.clear
清空日志缓冲区。

**请求**: `POST /api/v1/log/clear`

**响应**:
```json
{
  "code": 0,
  "data": {
    "success": true,
    "message": "Log buffer cleared"
  }
}
```

### log.setLevel
设置日志级别。

**请求**:
```
POST /api/v1/log/setLevel
{
  "level": "debug",
  "tag": "ts_net"
}
```

**参数**:
- `level`: 日志级别（0-5 或字符串 "error"/"warn"/"info"/"debug"/"verbose"）
- `tag`: 可选，针对特定 TAG 设置级别

**响应**:
```json
{
  "code": 0,
  "data": {
    "success": true,
    "level": "DEBUG",
    "tag": "ts_net"
  }
}
```

### log.capture
控制 ESP_LOG 捕获。

**请求**:
```
POST /api/v1/log/capture
{
  "enable": true
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "success": true,
    "captureEnabled": true
  }
}
```

## OTA API

### ota.status
获取 OTA 状态。

**请求**: `GET /api/v1/ota/status`

**响应**:
```json
{
  "code": 0,
  "data": {
    "state": "idle",
    "running_partition": "ota_0",
    "next_partition": "ota_1",
    "pending_verify": false,
    "rollback_possible": true
  }
}
```

### ota.progress
获取升级进度。

**请求**: `GET /api/v1/ota/progress`

**响应**:
```json
{
  "code": 0,
  "data": {
    "state": "downloading",
    "percent": 45,
    "received": 921600,
    "total": 2048000,
    "message": "Downloading firmware..."
  }
}
```

### ota.version
获取固件版本信息。

**请求**: `GET /api/v1/ota/version`

**响应**:
```json
{
  "code": 0,
  "data": {
    "version": "0.2.0",
    "idf_version": "v5.5.2",
    "compile_time": "Jan 23 2026 12:00:00"
  }
}
```

### ota.https.start
从 HTTPS URL 启动 OTA。

**请求**:
```
POST /api/v1/ota/https/start
{
  "url": "https://server/TianShanOS.bin",
  "auto_reboot": true,
  "allow_downgrade": false,
  "skip_verify": false,
  "www_url": "https://server/www.bin"
}
```

**响应**:
```json
{
  "code": 0,
  "message": "OTA started"
}
```

### ota.sdcard.start
从 SD 卡启动 OTA。

**请求**:
```
POST /api/v1/ota/sdcard/start
{
  "path": "/sdcard/TianShanOS.bin",
  "auto_reboot": true
}
```

### ota.www.start
从 HTTPS URL 升级 www 分区。

**请求**:
```
POST /api/v1/ota/www/start
{
  "url": "https://server/www.bin"
}
```

### ota.www.start_sdcard
从 SD 卡升级 www 分区。

**请求**:
```
POST /api/v1/ota/www/start_sdcard
{
  "path": "/sdcard/www.bin"
}
```

**响应**:
```json
{
  "code": 0,
  "message": "WWW OTA started from SD card"
}
```

### ota.validate
标记固件有效（取消回滚）。

**请求**: `POST /api/v1/ota/validate`

**响应**:
```json
{
  "code": 0,
  "message": "Firmware validated, rollback cancelled"
}
```

### ota.rollback
回滚到上一版本。

**请求**: `POST /api/v1/ota/rollback`

**响应**:
```json
{
  "code": 0,
  "message": "Rolling back to previous firmware..."
}
```

### ota.abort
中止升级。

**请求**: `POST /api/v1/ota/abort`

## UI 配置 API

### ui.widgets.get
获取 WebUI 数据监控组件配置。

**请求**: `POST /api/v1/call`
```json
{
  "method": "ui.widgets.get"
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "widgets": [
      {"id": "w1", "type": "power", "title": "总功耗", "config": {...}},
      {"id": "w2", "type": "temp", "title": "CPU温度", "config": {...}}
    ],
    "refresh_interval": 5000,
    "source": "sdcard"
  }
}
```

**source 字段说明**:
- `"sdcard"`: 从 SD 卡加载
- `"nvs"`: 从 NVS 加载（并已同步到 SD 卡）
- `"default"`: 使用默认空配置

### ui.widgets.set
保存 WebUI 数据监控组件配置（双写 SD 卡 + NVS）。

**请求**: `POST /api/v1/call`
```json
{
  "method": "ui.widgets.set",
  "params": {
    "widgets": [
      {"id": "w1", "type": "power", "title": "总功耗", "config": {...}}
    ],
    "refresh_interval": 5000
  }
}
```

**响应**:
```json
{
  "code": 0,
  "data": {
    "nvs_saved": true
  }
}
```

**存储位置**：系统 NVS，namespace `ts_ui`、key `widgets`。旧 SD 文件只在首次迁移时读取，保存与插卡不再双向同步。

---

## 自动化配置与删除 API

以下方法支持 `POST /api/v1/call` 的 `{ "method": "…", "params": { … } }` 格式；HTTP 路径使用 `/api/v1/automation/config/status` 这种斜线形式。需使用登录后的 Bearer token。

所有配置修改携带当前 `active_version`。该版本从最新列表或 `automation.config.status` 读取；收到 `configuration_changed` 后重新读取并确认操作，不能直接重发旧修改。成功响应的 `data.saved` 说明是否实际保存，不能仅因 HTTP 请求完成就提示保存成功。

| 方法 | 用途／主要参数 |
|---|---|
| `automation.config.status` | 当前来源、启停、内部副本、SD 可用性和 `active_version` |
| `automation.config.backup` | 手动保存完整应急副本；`active_version` |
| `automation.config.preview` | 来源切换／恢复预览；`operation`：`use_sd`、`save_to_sd`、`restore_internal` |
| `automation.config.apply` | 确认预览；同一 `operation`、预览返回的 `active_version` 和 `target_version` |
| `automation.config.enable` | 显式启用／暂停；`enabled`、`active_version` |

操作在当前配置集合内处理规则、模板、命令、主机、数据源和用户变量，不修改另一份手动副本。`restore_internal` 恢复当前工作位置；`use_sd` 只切换来源，因此可以成功但 `saved=false`。`save_to_sd` 写卡成功、保存来源选择失败时会分别报告结果，不能把它提示为完整切换成功。

`automation.rules.delete`、`automation.actions.delete`、`ssh.commands.remove` 使用目标 `id` 和 `active_version`：

- `preview=true`：返回影响清单，不删除。
- 无引用时可普通删除；Rule 直接移除。
- 有引用时可用 `replacement_id` 替换引用，或 `mode="cascade"` 解除引用并停用受影响规则。
- `force=true`：最终强删，不等待远端核验。删除命令后，关联模板保留为停用草稿并清空命令引用。
- 影响范围无法确认时，确认后增加 `pause_automation=true`，暂停自动化并删除；重启不会自动取消此次暂停。

以上删除均保留真实写入错误。独立后台服务记录由 `automation.services.list` 列出；核验和停止使用 `automation.services.status`（`verify=true`）、`automation.services.stop` 及 `run_id`，不再用命令 ID 代替服务实例。

详见 [自动化配置使用与存储](AUTOMATION_STORAGE.md)。

## 自动化配置导入导出 API

### 导出

`automation.sources.export`、`automation.rules.export`、`automation.actions.export` 使用 `id` 和可选的 `recipient_cert`（PEM；省略时使用本机证书）。成功返回 `data.tscfg` 和 `data.filename`。

```json
{
  "method": "automation.actions.export",
  "params": { "id": "notify_slack" }
}
```

### 数据源、模板、SSH 主机和命令导入

`automation.sources.import`、`automation.actions.import`、`ssh.hosts.import`、`ssh.commands.import` 先预览，再确认。包必须满足当前配置签名信任和设备解密策略；不能用 TLS CA 或普通 JSON 替代配置签名授权。

```json
{
  "method": "automation.sources.import",
  "params": { "tscfg": "TSCFG01…", "preview": true }
}
```

预览会解密并校验候选配置及引用，返回 `valid`、`items`、`exists`、`unchanged`、`signer`、`official`、`package_digest`、`credential_generation`、`active_version`。命令包中携带的依赖主机一并显示。

确认时使用同一个 `tscfg`，设置 `preview=false`，并提供：

| 参数 | 值 |
|---|---|
| `active_version` | 预览返回的配置版本 |
| `expected_digest` | 预览返回的 `package_digest` |
| `expected_credential` | 预览返回的 `credential_generation` |
| `overwrite` | 覆盖已有且内容不同的对象时为 `true` |

导入保存到当前完整配置集合并立即生效；成功返回 `imported=true`、`overwritten`、`saved`、`restart_required=false`。不再将独立旧文件落卡后要求重启。

### automation.rules.import

规则包同样先以 `tscfg`、`preview=true` 预览。确认时携带同一个包、`active_version`、`overwrite`，以及预览返回的 `expected_revision`、`expected_generation`、`credential_generation`、`package_digest`。规则存在并变化时必须明确允许覆盖。凭据或配置变化会拒绝过期确认，需重新预览。

---

## 错误码

| 代码 | 含义 |
|------|------|
| 0 | 成功 |
| 1 | 通用错误 |
| 2 | 无效参数 |
| 3 | 未找到 |
| 4 | 未授权 |
| 5 | 权限不足 |
| 6 | 超时 |
| 7 | 状态错误 |

## HTTP 状态码

| 状态码 | 含义 |
|--------|------|
| 200 | 成功 |
| 201 | 已创建 |
| 400 | 请求错误 |
| 401 | 未授权 |
| 403 | 禁止访问 |
| 404 | 未找到 |
| 500 | 服务器错误 |
