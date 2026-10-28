# TianShanOS 自动化引擎设计文档

> **文档版本**: 2.1  
> **创建日期**: 2026-01-26  
> **更新日期**: 2026-01-28  
> **状态**: 核心功能已实现，规则引擎增强完成

---

## 目录

1. [概述](#1-概述)
2. [系统架构](#2-系统架构)
3. [数据源系统](#3-数据源系统)
4. [规则引擎](#4-规则引擎)
5. [动作系统](#5-动作系统)
6. [配置 Schema](#6-配置-schema)
7. [WebUI Dashboard 设计](#7-webui-dashboard-设计)
8. [组件设计](#8-组件设计)
9. [实现计划](#9-实现计划)
10. [附录](#10-附录)

---

## 1. 概述

### 1.1 设计目标

TianShanOS 自动化引擎是一个**可配置的事件驱动系统**，用于：

- 📊 **监控**: 采集 ESP32 本地数据和远程服务器（AGX/LPMU）运行数据
- 🔄 **自动化**: 基于条件规则自动触发动作
- 🖥️ **显示**: 将监控数据和系统状态展示到 LED 三个显示区域
- 🎮 **控制**: 通过 SSH 执行远程服务器控制命令
- ⚡ **反馈**: 命令执行结果可触发后续规则

### 1.2 核心理念

```
面向配置，而非面向代码
```

用户通过 JSON 配置（可通过 WebUI 可视化编辑）定义：
- 监控什么数据
- 什么条件触发什么动作
- LED 显示什么内容

### 1.3 核心概念

| 概念 | 说明 | 示例 |
|------|------|------|
| **数据源 (Source)** | 数据来源定义 | ESP32 内置、WebSocket、REST API |
| **字段 (Field)** | 数据源中的具体数据点 | `agx.cpu_temp`, `esp32.voltage` |
| **事件 (Event)** | 系统中发生的状态变化 | 数据更新、命令完成、定时器触发 |
| **条件 (Condition)** | 判断表达式 | `agx.cpu_temp > 80 AND agx.gpu_temp > 75` |
| **动作 (Action)** | 要执行的操作 | LED 显示、SSH 命令、日志记录 |
| **规则 (Rule)** | 触发条件 + 动作列表 | 当温度过高时，显示警告并执行降频 |

---

## 2. 系统架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         TianShanOS Automation Engine                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        事件源 (Event Sources)                        │   │
│   ├─────────────────────────────────────────────────────────────────────┤   │
│   │  ESP32 内置         远程数据             命令反馈         系统事件   │   │
│   │  • system.*        • agx.* (WS)        • ssh.result.*  • timer.*   │   │
│   │  • network.*       • lpmu.* (REST)     • command.*     • startup   │   │
│   │  • adc.*           • custom.*                          • service.* │   │
│   │  • gpio.*                                                          │   │
│   │  • service.*                                                       │   │
│   └──────────────────────────────┬──────────────────────────────────────┘   │
│                                  │                                          │
│                                  ▼                                          │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                      变量存储 (Variable Store)                       │   │
│   │                                                                     │   │
│   │   所有数据点统一存储，支持变量引用 ${source.field}                    │   │
│   └──────────────────────────────┬──────────────────────────────────────┘   │
│                                  │                                          │
│                                  ▼                                          │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        规则引擎 (Rule Engine)                        │   │
│   │                                                                     │   │
│   │   • 条件表达式解析与求值                                             │   │
│   │   • 触发类型：condition / event / timer / startup                   │   │
│   │   • 防抖 (debounce) / 持续时间 (sustain) / 节流 (throttle)          │   │
│   └──────────────────────────────┬──────────────────────────────────────┘   │
│                                  │                                          │
│                                  ▼                                          │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                         动作执行 (Actions)                           │   │
│   ├─────────────────────────────────────────────────────────────────────┤   │
│   │  LED 显示         SSH 命令          GPIO 控制       其他动作         │   │
│   │  • led.touch      • ssh.exec        • gpio.set      • log           │   │
│   │  • led.board      • ssh.exec_async  • gpio.toggle   • variable.set  │   │
│   │  • led.matrix                                       • trigger_rule  │   │
│   │                                                     • delay         │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

```
数据采集 ──► 变量存储 ──► 规则评估 ──► 动作执行
    │                        │              │
    │                        │              ▼
    │                        │         命令反馈 ──┐
    │                        │                    │
    └────────────────────────┴────────────────────┘
                    (循环)
```

### 2.3 与现有系统集成

```
┌─────────────────────────────────────────────────────────────────┐
│                      TianShanOS 组件关系                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌─────────────┐                                               │
│   │ ts_event    │◄──── 所有组件通过事件总线通信                   │
│   └──────┬──────┘                                               │
│          │                                                      │
│   ┌──────┴──────┐                                               │
│   │             │                                               │
│   ▼             ▼                                               │
│ ┌─────────┐  ┌─────────────┐                                    │
│ │ ts_led  │  │ts_automation│◄──── 新增：自动化引擎               │
│ │(Core API)│  │             │                                    │
│ └─────────┘  └──────┬──────┘                                    │
│                     │                                           │
│          ┌─────────┬┴─────────┐                                 │
│          ▼         ▼          ▼                                 │
│    ┌──────────┐ ┌──────────┐ ┌──────────┐                       │
│    │ts_source │ │ ts_rule  │ │ts_action │                       │
│    │数据采集   │ │规则引擎   │ │动作执行   │                       │
│    └──────────┘ └──────────┘ └──────────┘                       │
│          │                          │                           │
│          │                          ▼                           │
│          │                   ┌─────────────┐                    │
│          │                   │ ts_security │                    │
│          │                   │ (SSH 执行)  │                    │
│          │                   └─────────────┘                    │
│          │                                                      │
│          ▼                                                      │
│   ┌─────────────┐                                               │
│   │   ts_net    │                                               │
│   │(WS/HTTP客户端)│                                              │
│   └─────────────┘                                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 数据源系统

### 3.1 数据源类型

| 类型 | 标识 | 说明 | 实现方式 |
|------|------|------|----------|
| **内置** | `builtin` | ESP32 本地数据 | 直接调用 ESP-IDF API |
| **WebSocket** | `websocket` | 实时推送数据 | esp_websocket_client |
| **REST** | `rest` | HTTP 轮询数据 | esp_http_client |
| **MQTT** | `mqtt` | MQTT 订阅 (可选) | esp_mqtt_client |

### 3.2 内置数据源（基于现有 HAL/驱动）

ESP32 本地数据通过 `builtin` 类型定义，**基于项目中已实现的 HAL 和驱动模块**：

#### 3.2.1 系统数据 (`system.*`)

| builtin ID | 说明 | 类型 | 单位 | 来源 API |
|-----------|------|------|------|----------|
| `system.heap_free` | 可用堆内存 | number | KB | `esp_get_free_heap_size()` |
| `system.heap_min_free` | 历史最小可用内存 | number | KB | `esp_get_minimum_free_heap_size()` |
| `system.psram_free` | 可用 PSRAM | number | KB | `heap_caps_get_free_size(SPIRAM)` |
| `system.psram_total` | PSRAM 总量 | number | KB | `heap_caps_get_total_size(SPIRAM)` |
| `system.cpu_freq` | CPU 频率 | number | MHz | `ts_hal_get_capabilities()` |
| `system.uptime` | 运行时间 | number | s | `esp_timer_get_time()` |
| `system.chip_temp` | 芯片温度 | number | °C | 温度传感器 API |
| `system.reset_reason` | 上次重启原因 | string | - | `esp_reset_reason()` |

#### 3.2.2 网络数据 (`network.*`) - 来自 ts_net

| builtin ID | 说明 | 类型 | 单位 | 来源 API |
|-----------|------|------|------|----------|
| `network.wifi_connected` | WiFi 连接状态 | bool | - | `ts_wifi_is_connected()` |
| `network.wifi_rssi` | WiFi 信号强度 | number | dBm | `ts_wifi_get_rssi()` |
| `network.wifi_ssid` | 当前 SSID | string | - | `ts_wifi_get_ssid()` |
| `network.ip_address` | IP 地址 | string | - | `ts_net_get_ip()` |
| `network.eth_connected` | 以太网连接状态 | bool | - | `ts_eth_is_connected()` |
| `network.eth_ip` | 以太网 IP | string | - | `ts_eth_get_ip()` |

#### 3.2.3 电源监控 (`power.*`) - 来自 ts_power_monitor

> **硬件**: ADC GPIO18 (分压比 11.4:1), UART GPIO47 (9600 8N1 电源芯片)

| builtin ID | 说明 | 类型 | 单位 | 来源 API |
|-----------|------|------|------|----------|
| `power.voltage` | 供电电压 | number | V | `ts_power_monitor_get_voltage_data()` |
| `power.current` | 电流 | number | A | `ts_power_monitor_get_power_chip_data()` |
| `power.wattage` | 功率 | number | W | 计算值 |
| `power.raw_adc` | ADC 原始值 | number | - | `ts_adc_read_raw()` |

#### 3.2.4 设备控制状态 (`device.*`) - 来自 ts_device_ctrl

> **硬件引脚** (来自 `boards/rm01_esp32s3/pins.json`):
> - AGX: FORCE_SHUTDOWN(GPIO3), RESET(GPIO1), RECOVERY(GPIO40)
> - LPMU: POWER(GPIO46), RESET(GPIO2)

| builtin ID | 说明 | 类型 | 来源 API |
|-----------|------|------|----------|
| `device.agx.state` | AGX 电源状态 | string | `ts_device_get_status(TS_DEVICE_AGX)` |
| `device.agx.power_good` | AGX 电源正常 | bool | GPIO 读取 |
| `device.agx.uptime` | AGX 运行时间 | number | 内部计时 |
| `device.lpmu.state` | LPMU 电源状态 | string | `ts_device_get_status(TS_DEVICE_LPMU)` |

#### 3.2.5 GPIO 状态 (`gpio.*`) - 来自 ts_hal_gpio

| builtin ID | 说明 | 类型 | 来源 API |
|-----------|------|------|----------|
| `gpio.<function_name>` | 逻辑引脚电平 | bool | `ts_gpio_get_level()` |

> **示例**: `gpio.AGX_RESET`, `gpio.LPMU_POWER`, `gpio.ETH_RST`

#### 3.2.6 服务状态 (`service.*`) - 来自 ts_service

| builtin ID | 说明 | 类型 | 来源 API |
|-----------|------|------|----------|
| `service.<name>.running` | 服务是否运行 | bool | `ts_service_is_running()` |
| `service.<name>.healthy` | 服务是否健康 | bool | `ts_service_health_check()` |
| `service.pki.status` | PKI 状态 | string | `ts_cert_get_status()` |
| `service.pki.cert_days_left` | 证书剩余天数 | number | 证书解析 |

#### 3.2.7 LED 状态 (`led.*`) - 来自 ts_led

| builtin ID | 说明 | 类型 | 来源 API |
|-----------|------|------|----------|
| `led.touch.brightness` | Touch 亮度 | number | `ts_led_get_brightness()` |
| `led.touch.effect` | Touch 当前效果 | string | `ts_led_get_current_effect()` |
| `led.board.color` | Board LED 颜色 | string | 内部状态 |
| `led.matrix.effect` | Matrix 当前效果 | string | `ts_led_get_current_effect()` |

### 3.3 远程数据源配置

#### WebSocket 数据源

```json
{
  "id": "agx",
  "name": "AGX Orin",
  "type": "websocket",
  "config": {
    "url": "ws://192.168.1.100:58090/socket.io/",
    "reconnect": true,
    "reconnect_interval": 5000,
    "ping_interval": 30000
  },
  "interval": 1000,
  "enabled": true,
  "fields": [
    {"id": "cpu_usage", "path": "cpu.percent", "name": "CPU使用率", "unit": "%", "type": "number"},
    {"id": "cpu_temp", "path": "cpu.temp", "name": "CPU温度", "unit": "°C", "type": "number"},
    {"id": "gpu_usage", "path": "gpu.percent", "name": "GPU使用率", "unit": "%", "type": "number"},
    {"id": "gpu_temp", "path": "gpu.temp", "name": "GPU温度", "unit": "°C", "type": "number"},
    {"id": "mem_used", "path": "memory.used_gb", "name": "内存使用", "unit": "GB", "type": "number"},
    {"id": "mem_total", "path": "memory.total_gb", "name": "内存总量", "unit": "GB", "type": "number"},
    {"id": "power", "path": "power.watts", "name": "功耗", "unit": "W", "type": "number"}
  ]
}
```

#### REST 数据源

```json
{
  "id": "lpmu",
  "name": "LPMU 服务器",
  "type": "rest",
  "config": {
    "url": "http://192.168.1.101:8080/api/status",
    "method": "GET",
    "headers": {
      "Authorization": "Bearer ${secrets.lpmu_token}"
    },
    "timeout": 5000
  },
  "interval": 5000,
  "enabled": true,
  "fields": [
    {"id": "status", "path": "status", "name": "运行状态", "type": "string"},
    {"id": "model_name", "path": "inference.model", "name": "当前模型", "type": "string"},
    {"id": "model_fps", "path": "inference.fps", "name": "推理FPS", "type": "number"},
    {"id": "queue_length", "path": "inference.queue", "name": "队列长度", "type": "number"}
  ]
}
```

### 3.4 字段路径解析 (JSON Path)

支持简化的 JSON Path 语法从响应中提取数据：

| 语法 | 示例 | 说明 |
|------|------|------|
| `key` | `status` | 顶层字段 |
| `key.subkey` | `cpu.temp` | 嵌套字段 |
| `key[0]` | `gpus[0]` | 数组索引 |
| `key[0].subkey` | `gpus[0].temp` | 数组元素的字段 |

---

## 4. 规则引擎

### 4.1 规则结构

```json
{
  "id": "rule_id",
  "name": "规则名称",
  "enabled": true,
  "priority": 0,
  "trigger": { ... },
  "actions": [ ... ]
}
```

### 4.2 触发器类型

#### 4.2.1 条件触发 (`condition`)

当表达式结果从 false 变为 true 时触发：

```json
{
  "type": "condition",
  "expression": "agx.cpu_temp > 80 AND agx.gpu_temp > 75",
  "debounce": 3000,
  "sustain": 0
}
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `expression` | 条件表达式 | 必填 |
| `debounce` | 防抖时间 (ms)，触发后 N 毫秒内不重复触发 | 0 |
| `sustain` | 持续时间 (ms)，条件需持续满足 N 毫秒才触发 | 0 |

#### 4.2.2 事件触发 (`event`)

当特定事件发生时触发：

```json
{
  "type": "event",
  "event": "ssh.completed",
  "filter": "ssh.last.host == 'agx_ssh' AND ssh.last.exit_code != 0"
}
```

**支持的事件**:

| 事件 | 说明 | 附加数据 |
|------|------|---------|
| `data.updated.<source_id>` | 数据源更新 | 更新的字段列表 |
| `ssh.completed` | SSH 命令完成 | exit_code, stdout, stderr |
| `network.wifi_connected` | WiFi 连接成功 | IP 地址 |
| `network.wifi_disconnected` | WiFi 断开 | - |
| `service.started.<name>` | 服务启动 | - |
| `service.stopped.<name>` | 服务停止 | - |
| `startup` | 系统启动完成 | - |

#### 4.2.3 定时触发 (`timer`)

按时间间隔或 Cron 表达式触发：

```json
{
  "type": "timer",
  "interval": 60000
}
```

或

```json
{
  "type": "timer",
  "cron": "0 * * * *"
}
```

#### 4.2.4 启动触发 (`startup`)

系统启动完成后触发一次：

```json
{
  "type": "startup",
  "delay": 5000
}
```

### 4.3 条件表达式语法

#### 支持的运算符

| 类型 | 运算符 | 示例 |
|------|--------|------|
| 比较 | `>`, `<`, `>=`, `<=`, `==`, `!=` | `agx.cpu_temp > 80` |
| 逻辑 | `AND`, `OR`, `NOT` | `a > 1 AND b < 2` |
| 括号 | `(`, `)` | `(a > 1 OR b > 2) AND c == 0` |
| 字符串 | `==`, `!=`, `contains` | `lpmu.status == "running"` |
| 存在性 | `exists`, `!exists` | `exists(agx.cpu_temp)` |

#### 变量引用

```
${source_id.field_id}   # 完整引用
source_id.field_id      # 简写（条件表达式中）
```

#### 示例

```
# 简单比较
agx.cpu_temp > 80

# 复合条件
agx.cpu_temp > 80 AND agx.gpu_temp > 75

# 范围判断
agx.cpu_temp >= 70 AND agx.cpu_temp < 85

# 字符串比较
lpmu.status == "running"

# 带括号的复杂条件
(agx.cpu_temp > 85 OR agx.gpu_temp > 85) AND esp32.power_monitor.voltage > 12

# 存在性检查
exists(agx.cpu_temp) AND agx.cpu_temp > 80
```

---

## 5. 动作系统

### 5.1 动作模板系统（2026-01-28 新增）

动作模板是可复用的动作配置，支持手动执行和规则触发。

#### 5.1.1 模板结构

```json
{
  "id": "action_001",
  "name": "AGX 重启命令",
  "description": "安全重启 AGX Orin",
  "type": "ssh_cmd_ref",
  "enabled": true,
  "delay_ms": 0,
  "ssh_ref": {
    "cmd_id": "agx_reboot"
  }
}
```

#### 5.1.2 支持的动作类型

| 类型 | 说明 | 配置参数 |
|------|------|----------|
| `cli` | 本地 CLI 命令 | `command`, `var_name`, `timeout_ms` |
| `ssh_cmd_ref` | SSH 命令引用 | `cmd_id`（引用已配置的 SSH 命令） |
| `led` | LED 控制 | `device`, `color`, `effect`, `brightness` |
| `log` | 日志记录 | `level`, `message` |
| `set_var` | 设置变量 | `variable`, `value` |
| `webhook` | HTTP 请求 | `url`, `method`, `body_template` |

#### 5.1.3 SSH 命令引用机制

动作模板通过 `cmd_id` 引用 SSH 管理页面中已配置的命令：

```
┌─────────────────────┐     ┌─────────────────────┐     ┌─────────────────────┐
│  SSH 命令配置       │     │  动作模板           │     │  执行时             │
│  (ts_ssh_commands)  │────▶│  (ssh_cmd_ref)      │────▶│  (ts_action_exec)   │
│                     │     │                     │     │                     │
│  id: agx_reboot     │     │  cmd_id: agx_reboot │     │  查找命令配置       │
│  host: agx0         │     │                     │     │  获取主机信息       │
│  command: reboot    │     │                     │     │  keystore 认证      │
│  var_name: result   │     │                     │     │  执行并存储结果     │
└─────────────────────┘     └─────────────────────┘     └─────────────────────┘
```

#### 5.1.4 配置保存与引用

规则、动作模板、SSH 命令、SSH 主机、数据源和用户变量作为一个完整配置集合保存。正常使用 SD 卡的 `/sdcard/config/automation/state.json`；内部 `/autocfg/state.json` 是用户手动保存的应急副本。无卡时自动使用有效内部配置，编辑直接保存到内部。

所有修改经过统一配置入口，在同一次提交中处理引用并保存完整集合。规则上限为 64 条，不代表可以同时执行 64 个任务。列表和编辑页的写入携带 `active_version`，避免旧页面覆盖新配置。

删除配置不连接远端等待服务停止。强制删除命令会保留关联模板为停用草稿、清空命令引用，并解除受影响规则的关联和停用规则；远端服务记录仍可独立核验、停止。详见 [自动化配置使用与存储](AUTOMATION_STORAGE.md)。

#### 5.1.5 API 端点

| 端点 | 说明 |
|------|------|
| `automation.actions.list` | 列出所有动作模板 |
| `automation.actions.add` | 创建模板 |
| `automation.actions.get` | 获取模板详情 |
| `automation.actions.delete` | 删除模板 |
| `automation.actions.execute` | 执行模板 |

### 5.2 动作类型概览

| 类型 | 说明 | 示例 |
|------|------|------|
| `led.touch` | Touch 屏幕显示 | 显示文字、数值 |
| `led.board` | 板载 LED | 状态颜色、闪烁 |
| `led.matrix` | 矩阵屏显示 | 效果、动画、文字 |
| `ssh` | SSH 命令执行 | 远程控制命令 |
| `gpio` | GPIO 控制 | 电平设置 |
| `log` | 日志记录 | 记录到系统日志 |
| `variable` | 变量操作 | 设置/清除变量 |
| `delay` | 延时 | 等待指定时间 |
| `trigger_rule` | 触发规则 | 链式触发其他规则 |
| `conditional` | 条件动作 | 根据条件执行不同动作 |

### 5.2 LED 动作

#### 5.2.1 Touch 显示 (`led.touch`)

```json
{
  "type": "led.touch",
  "mode": "text",
  "content": "CPU: ${agx.cpu_temp}°C\nGPU: ${agx.gpu_temp}°C",
  "font_size": 16,
  "color": "#FFFFFF",
  "background": "#000000",
  "duration": 0,
  "page": 0
}
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `mode` | 显示模式: `text`, `value`, `chart` | `text` |
| `content` | 显示内容，支持变量替换和 `\n` 换行 | 必填 |
| `font_size` | 字体大小 | 16 |
| `color` | 文字颜色 | `#FFFFFF` |
| `background` | 背景颜色 | `#000000` |
| `duration` | 显示持续时间 (ms)，0 = 持续显示 | 0 |
| `page` | 显示页面编号 | 0 |

#### 5.2.2 Board LED (`led.board`)

```json
{
  "type": "led.board",
  "color": "#FF0000",
  "blink": true,
  "blink_interval": 500,
  "duration": 5000
}
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `color` | LED 颜色 (十六进制或命名颜色) | 必填 |
| `blink` | 是否闪烁 | false |
| `blink_interval` | 闪烁间隔 (ms) | 500 |
| `duration` | 持续时间 (ms)，0 = 持续 | 0 |

#### 5.2.3 Matrix 显示 (`led.matrix`)

```json
{
  "type": "led.matrix",
  "effect": "alert",
  "params": {
    "color": "red",
    "text": "OVERHEAT",
    "speed": 50
  },
  "duration": 0
}
```

**支持的效果**:

| effect | 说明 | 参数 |
|--------|------|------|
| `solid` | 纯色 | `color` |
| `alert` | 警报效果 | `color`, `text` |
| `pulse` | 呼吸效果 | `color`, `speed` |
| `scroll` | 滚动文字 | `text`, `color`, `speed` |
| `value` | 数值显示 | `source`, `format`, `color` |
| `progress` | 进度条 | `value`, `max`, `color` |
| `custom` | 自定义图案 | `pattern` (数组) |

### 5.3 SSH 动作

```json
{
  "type": "ssh",
  "host": "agx_ssh",
  "command": "sudo jetson_clocks --fan",
  "timeout": 30000,
  "async": false,
  "store_result": "fan_result",
  "on_success": [...],
  "on_failure": [...]
}
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `host` | SSH 主机 ID (在 ssh_hosts 中定义) | 必填 |
| `command` | 要执行的命令，支持变量替换 | 必填 |
| `timeout` | 超时时间 (ms) | 30000 |
| `async` | 是否异步执行 | false |
| `store_result` | 存储结果的变量名 | - |
| `on_success` | 成功时执行的动作列表 | - |
| `on_failure` | 失败时执行的动作列表 | - |

**结果变量** (当 `store_result` 设置时):

```
${fan_result.exit_code}   # 退出码
${fan_result.stdout}      # 标准输出
${fan_result.stderr}      # 标准错误
${fan_result.duration}    # 执行时间 (ms)
```

### 5.4 条件动作 (`conditional`)

根据条件执行不同的动作：

```json
{
  "type": "conditional",
  "conditions": [
    {
      "if": "ssh.last.exit_code == 0",
      "then": [
        {"type": "led.touch", "content": "✓ 命令成功"}
      ]
    },
    {
      "if": "ssh.last.exit_code != 0",
      "then": [
        {"type": "led.touch", "content": "✗ 命令失败: ${ssh.last.stderr}"},
        {"type": "led.board", "color": "#FF0000", "blink": true}
      ]
    }
  ],
  "else": [
    {"type": "log", "message": "未匹配任何条件"}
  ]
}
```

### 5.5 其他动作

#### 日志 (`log`)

```json
{
  "type": "log",
  "level": "warn",
  "message": "温度过高: CPU=${agx.cpu_temp}°C GPU=${agx.gpu_temp}°C"
}
```

#### 变量设置 (`variable`)

```json
{
  "type": "variable",
  "action": "set",
  "name": "alert_count",
  "value": "${alert_count} + 1"
}
```

#### 延时 (`delay`)

```json
{
  "type": "delay",
  "ms": 5000
}
```

#### 触发规则 (`trigger_rule`)

```json
{
  "type": "trigger_rule",
  "rule_id": "recovery_procedure"
}
```

---

## 6. 配置 Schema

### 6.1 完整配置结构

```json
{
  "version": "1.0",
  "sources": [...],
  "ssh_hosts": [...],
  "rules": [...],
  "commands": [...],
  "displays": {...},
  "secrets": {...}
}
```

### 6.2 完整配置示例

```json
{
  "version": "1.0",
  
  "sources": [
    {
      "id": "esp32",
      "name": "ESP32 本机",
      "type": "builtin",
      "interval": 1000,
      "fields": [
        {"id": "heap_free", "builtin": "system.heap_free", "name": "可用内存"},
        {"id": "voltage", "builtin": "adc.voltage_main", "name": "主电压"},
        {"id": "wifi_rssi", "builtin": "network.wifi_rssi", "name": "WiFi信号"}
      ]
    },
    {
      "id": "agx",
      "name": "AGX Orin",
      "type": "websocket",
      "config": {
        "url": "ws://192.168.1.100:58090/socket.io/",
        "reconnect": true
      },
      "interval": 1000,
      "enabled": true,
      "fields": [
        {"id": "cpu_temp", "path": "cpu.temp", "name": "CPU温度", "unit": "°C"},
        {"id": "gpu_temp", "path": "gpu.temp", "name": "GPU温度", "unit": "°C"},
        {"id": "gpu_usage", "path": "gpu.percent", "name": "GPU使用率", "unit": "%"},
        {"id": "power", "path": "power.watts", "name": "功耗", "unit": "W"}
      ]
    },
    {
      "id": "lpmu",
      "name": "LPMU 服务器",
      "type": "rest",
      "config": {
        "url": "http://192.168.1.101:8080/api/status",
        "method": "GET"
      },
      "interval": 5000,
      "enabled": true,
      "fields": [
        {"id": "status", "path": "status", "name": "运行状态"},
        {"id": "fps", "path": "inference.fps", "name": "推理FPS"}
      ]
    }
  ],
  
  "ssh_hosts": [
    {
      "id": "agx_ssh",
      "name": "AGX Orin",
      "host": "192.168.1.100",
      "port": 22,
      "username": "nvidia",
      "auth": "key",
      "key_path": "/sdcard/ssh/agx_key"
    },
    {
      "id": "lpmu_ssh",
      "name": "LPMU",
      "host": "192.168.1.101",
      "port": 22,
      "username": "admin",
      "auth": "password",
      "password_key": "lpmu_password"
    }
  ],
  
  "rules": [
    {
      "id": "normal_display",
      "name": "正常状态显示",
      "enabled": true,
      "trigger": {
        "type": "condition",
        "expression": "agx.cpu_temp <= 85 AND agx.gpu_temp <= 85"
      },
      "actions": [
        {
          "type": "led.touch",
          "content": "GPU ${agx.gpu_usage}%\n${agx.gpu_temp}°C"
        },
        {
          "type": "led.board",
          "color": "#00FF00"
        },
        {
          "type": "led.matrix",
          "effect": "value",
          "params": {"source": "agx.gpu_usage", "color": "gradient"}
        }
      ]
    },
    {
      "id": "high_temp_alert",
      "name": "高温警报",
      "enabled": true,
      "priority": 10,
      "trigger": {
        "type": "condition",
        "expression": "agx.cpu_temp > 85 OR agx.gpu_temp > 85",
        "debounce": 3000
      },
      "actions": [
        {
          "type": "led.matrix",
          "effect": "alert",
          "params": {"color": "red", "text": "OVERHEAT"}
        },
        {
          "type": "led.board",
          "color": "#FF0000",
          "blink": true
        },
        {
          "type": "ssh",
          "host": "agx_ssh",
          "command": "sudo jetson_clocks --fan"
        },
        {
          "type": "log",
          "level": "warn",
          "message": "高温警报: CPU=${agx.cpu_temp}°C GPU=${agx.gpu_temp}°C"
        }
      ]
    },
    {
      "id": "low_voltage_protection",
      "name": "低电压保护",
      "enabled": true,
      "priority": 100,
      "trigger": {
        "type": "condition",
        "expression": "esp32.voltage < 11.5",
        "sustain": 10000
      },
      "actions": [
        {
          "type": "led.matrix",
          "effect": "alert",
          "params": {"color": "red", "text": "LOW POWER"}
        },
        {
          "type": "ssh",
          "host": "agx_ssh",
          "command": "sudo shutdown -h now",
          "async": true
        },
        {
          "type": "ssh",
          "host": "lpmu_ssh",
          "command": "sudo shutdown -h now",
          "async": true
        },
        {
          "type": "log",
          "level": "error",
          "message": "低电压保护触发: ${esp32.voltage}V"
        }
      ]
    },
    {
      "id": "ssh_feedback",
      "name": "SSH 执行反馈",
      "enabled": true,
      "trigger": {
        "type": "event",
        "event": "ssh.completed"
      },
      "actions": [
        {
          "type": "conditional",
          "conditions": [
            {
              "if": "ssh.last.exit_code == 0",
              "then": [
                {"type": "led.touch", "content": "✓ 命令成功", "duration": 3000}
              ]
            },
            {
              "if": "ssh.last.exit_code != 0",
              "then": [
                {"type": "led.touch", "content": "✗ 失败", "duration": 5000},
                {"type": "led.board", "color": "#FF0000", "duration": 3000}
              ]
            }
          ]
        }
      ]
    }
  ],
  
  "commands": [
    {
      "id": "reboot_agx",
      "name": "重启 AGX",
      "icon": "restart",
      "category": "power",
      "host": "agx_ssh",
      "command": "sudo reboot",
      "confirm": "确定要重启 AGX 吗？",
      "require_role": "operator"
    },
    {
      "id": "fan_max",
      "name": "风扇全速",
      "icon": "fan",
      "category": "cooling",
      "host": "agx_ssh",
      "command": "sudo jetson_clocks --fan"
    },
    {
      "id": "check_gpu",
      "name": "GPU 状态",
      "icon": "gpu",
      "category": "info",
      "host": "agx_ssh",
      "command": "nvidia-smi --query-gpu=temperature.gpu,utilization.gpu --format=csv,noheader",
      "show_output": true
    },
    {
      "id": "restart_inference",
      "name": "重启推理服务",
      "icon": "refresh",
      "category": "service",
      "host": "lpmu_ssh",
      "command": "sudo systemctl restart inference.service"
    }
  ],
  
  "displays": {
    "default_page_interval": 5000,
    "brightness": 80
  },
  
  "secrets": {
    "storage": "nvs",
    "keys": ["lpmu_password", "lpmu_token"]
  }
}
```

### 6.3 配置文件位置

| 位置 | 优先级 | 说明 |
|------|--------|------|
| `/sdcard/config/automation.json` | 1 (最高) | SD 卡配置，可热更新 |
| NVS `automation` | 2 | 持久化配置 |
| 内置默认配置 | 3 | 编译时默认值 |

---

## 7. WebUI Dashboard 设计

> **重要**: Dashboard 是 WebUI 的核心界面，**所有配置都在前端定义**，可导出为 JSON。
> 
> **导航栏变更**: 删除原有的"监控"、"配置"页面，合并为统一的 **Dashboard** 入口。

### 7.1 导航栏结构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ⛰️ TianShanOS                                                              │
├─────────────────────────────────────────────────────────────────────────────┤
│  [Dashboard] [网络] [文件] [终端] [安全] [系统]         🔌 已连接  👤 admin  │
└─────────────────────────────────────────────────────────────────────────────┘
```

**页面说明**:
- **Dashboard** (新): 统一的监控、控制、自动化配置界面 ⭐
- **网络**: 网络配置（WiFi/以太网/NAT）
- **文件**: SD 卡文件管理
- **终端**: 串口终端 + 日志查看
- **安全**: PKI 证书、SSH 密钥管理
- **系统**: 系统信息、OTA 更新、重启

### 7.2 Dashboard 主界面

Dashboard 采用**可配置的卡片布局**，用户可以：
- 添加/删除/拖拽卡片
- 自定义卡片显示内容
- 配置数据源和规则
- 实时预览效果

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Dashboard                                    [编辑模式] [导出JSON] [导入]   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────┐  ┌─────────────────────────┐  ┌─────────────┐  │
│  │ 📊 AGX 状态             │  │ 📊 LPMU 状态            │  │ 🔋 电源     │  │
│  │ ─────────────────────  │  │ ─────────────────────  │  │ ─────────  │  │
│  │ CPU: ████████░░ 78%    │  │ 状态: 🟢 运行中         │  │ 12.8V     │  │
│  │ GPU: ██████████ 95%    │  │ FPS:  45               │  │ 2.1A      │  │
│  │ 温度: CPU 72°C GPU 80°C │  │ 模型: YOLOv8           │  │ 26.9W     │  │
│  │ 功耗: 45W              │  │ 队列: 3                │  │           │  │
│  │ [详情] [SSH终端]       │  │ [详情] [SSH终端]       │  │ [历史]    │  │
│  └─────────────────────────┘  └─────────────────────────┘  └─────────────┘  │
│                                                                             │
│  ┌─────────────────────────┐  ┌─────────────────────────────────────────┐  │
│  │ 🎮 快捷命令             │  │ 📋 自动化规则                           │  │
│  │ ─────────────────────  │  │ ─────────────────────────────────────  │  │
│  │ ┌─────┐ ┌─────┐ ┌─────┐│  │ ● 正常显示      条件: temp <= 85       │  │
│  │ │重启  ││关机  ││风扇  ││  │ ○ 高温警报      条件: temp > 85        │  │
│  │ │ AGX ││ AGX ││全速  ││  │ ○ 低电压保护    条件: voltage < 11.5   │  │
│  │ └─────┘ └─────┘ └─────┘│  │ ○ SSH反馈       事件: ssh.completed    │  │
│  │ ┌─────┐ ┌─────┐ ┌─────┐│  │                                         │  │
│  │ │重启  ││GPU   ││自定义││  │ [+ 添加规则] [编辑规则]                  │  │
│  │ │LPMU ││状态  ││命令  ││  │                                         │  │
│  │ └─────┘ └─────┘ └─────┘│  └─────────────────────────────────────────┘  │
│  └─────────────────────────┘                                               │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 💡 LED 显示预览                                                      │   │
│  │ ───────────────────────────────────────────────────────────────────  │   │
│  │  Touch          Board           Matrix                               │   │
│  │  ┌──────────┐   ┌────┐         ┌────────────────────────────────┐   │   │
│  │  │GPU: 95%  │   │ 🟢 │         │ ████████████████████████████  │   │   │
│  │  │ 80°C     │   └────┘         │ ████████  GPU: 95%  ████████  │   │   │
│  │  └──────────┘                  │ ████████████████████████████  │   │   │
│  │  [配置Touch]   [配置Board]     └────────────────────────────────┘   │   │
│  │                                [配置Matrix]                          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.3 编辑模式

点击 **[编辑模式]** 后，进入配置界面：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Dashboard 配置                                   [预览] [保存] [取消]       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  左侧面板                              右侧编辑区                            │
│  ┌─────────────────┐                  ┌─────────────────────────────────┐  │
│  │ 📂 数据源       │                  │ 编辑数据源: AGX Orin            │  │
│  │   ├─ ESP32 内置 │                  │ ──────────────────────────────  │  │
│  │   ├─ AGX (WS)  ◄│ ← 选中          │ 名称:    [AGX Orin           ]  │  │
│  │   └─ LPMU (REST)│                  │ 类型:    [WebSocket ▼]          │  │
│  │ [+ 添加]        │                  │ URL:     [ws://192.168.1.100..] │  │
│  │                 │                  │ 间隔:    [1000] ms              │  │
│  │ 📂 SSH 主机     │                  │ 启用:    [✓]                    │  │
│  │   ├─ agx_ssh    │                  │                                 │  │
│  │   └─ lpmu_ssh   │                  │ 数据字段:                        │  │
│  │ [+ 添加]        │                  │ ┌─────────────────────────────┐ │  │
│  │                 │                  │ │ cpu_temp | cpu.temp | °C   │ │  │
│  │ 📂 规则        │                  │ │ gpu_temp | gpu.temp | °C   │ │  │
│  │   ├─ 正常显示   │                  │ │ gpu_usage| gpu.percent| %  │ │  │
│  │   ├─ 高温警报   │                  │ │ [+ 添加字段]                │ │  │
│  │   └─ 低电压保护 │                  │ └─────────────────────────────┘ │  │
│  │ [+ 添加]        │                  │                                 │  │
│  │                 │                  │ [测试连接]  [删除数据源]        │  │
│  │ 📂 快捷命令     │                  └─────────────────────────────────┘  │
│  │   ├─ 重启AGX    │                                                       │
│  │   ├─ 风扇全速   │                                                       │
│  │   └─ ...        │                                                       │
│  │ [+ 添加]        │                                                       │
│  └─────────────────┘                                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.4 规则编辑器 (可视化)

```
┌─ 编辑规则: 高温警报 ─────────────────────────────────────────────────────────┐
│                                                                             │
│  基本信息                                                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 规则ID:  [high_temp_alert    ]    优先级: [10   ]    启用: [✓]      │   │
│  │ 名称:    [高温警报                                              ]   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  触发条件                                                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 类型: [◉ 条件触发  ○ 事件触发  ○ 定时触发]                          │   │
│  │                                                                     │   │
│  │ 条件构建器 (可视化):                                                 │   │
│  │ ┌─────────────────────────────────────────────────────────────────┐ │   │
│  │ │ [agx.cpu_temp ▼]  [>  ▼]  [85    ]                              │ │   │
│  │ │                                                    [OR ▼] [✕]   │ │   │
│  │ │ [agx.gpu_temp ▼]  [>  ▼]  [85    ]                        [✕]   │ │   │
│  │ │                                                                 │ │   │
│  │ │ [+ 添加条件]                                                    │ │   │
│  │ └─────────────────────────────────────────────────────────────────┘ │   │
│  │                                                                     │   │
│  │ 或手动输入表达式:                                                    │   │
│  │ [agx.cpu_temp > 85 OR agx.gpu_temp > 85                          ] │   │
│  │                                                                     │   │
│  │ 高级选项:  防抖 [3000] ms    持续 [0   ] ms                         │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  执行动作                                                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 1. LED Matrix                                              [↑↓] [✕] │   │
│  │    效果: [alert ▼]  颜色: [🔴 red]  文字: [OVERHEAT        ]       │   │
│  │ ──────────────────────────────────────────────────────────────────  │   │
│  │ 2. LED Board                                               [↑↓] [✕] │   │
│  │    颜色: [#FF0000  🎨]  闪烁: [✓]  间隔: [500] ms                  │   │
│  │ ──────────────────────────────────────────────────────────────────  │   │
│  │ 3. SSH 命令                                                [↑↓] [✕] │   │
│  │    主机: [agx_ssh ▼]  命令: [sudo jetson_clocks --fan       ]      │   │
│  │ ──────────────────────────────────────────────────────────────────  │   │
│  │ 4. 日志                                                    [↑↓] [✕] │   │
│  │    级别: [warn ▼]  消息: [高温警报: CPU=${agx.cpu_temp}°C...]      │   │
│  │                                                                     │   │
│  │ [+ 添加动作 ▼]                                                      │   │
│  │   ├─ LED Touch                                                      │   │
│  │   ├─ LED Board                                                      │   │
│  │   ├─ LED Matrix                                                     │   │
│  │   ├─ SSH 命令                                                       │   │
│  │   ├─ 日志                                                           │   │
│  │   ├─ 延时                                                           │   │
│  │   └─ 触发其他规则                                                    │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│                                     [测试规则] [删除规则] [取消] [保存]      │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.5 实时监控面板

```
┌─ 实时监控 ───────────────────────────────────────────────────────────────────┐
│                                                                             │
│  数据源状态                              当前变量值                          │
│  ┌─────────────────────────────┐       ┌─────────────────────────────────┐  │
│  │ 📟 ESP32   ✅ 运行   1.0s   │       │ 🔍 筛选: [              ] 🔎   │  │
│  │ 🖥️ AGX     ✅ 连接   1.0s   │       │ ──────────────────────────────  │  │
│  │ 🖥️ LPMU    ⚠️ 超时   5.0s   │       │ esp32.heap_free     = 156 KB   │  │
│  └─────────────────────────────┘       │ esp32.power.voltage = 12.8 V   │  │
│                                        │ agx.cpu_temp        = 67 °C    │  │
│  规则状态                               │ agx.gpu_temp        = 72 °C    │  │
│  ┌─────────────────────────────┐       │ agx.gpu_usage       = 45 %     │  │
│  │ 🟢 正常状态显示  ● 激活中   │       │ lpmu.status         = (超时)   │  │
│  │ ⚪ 高温警报      ○ 待命     │       └─────────────────────────────────┘  │
│  │ ⚪ 低电压保护    ○ 待命     │                                           │  │
│  │ ⭕ SSH反馈       ○ 禁用     │       最近动作日志                         │  │
│  └─────────────────────────────┘       ┌─────────────────────────────────┐  │
│                                        │ 12:34:56 [LED] touch 更新显示  │  │
│                                        │ 12:34:55 [LED] matrix 更新效果 │  │
│                                        │ 12:34:50 [SSH] agx 执行完成 ✓  │  │
│                                        │ 12:34:45 [规则] 正常显示 触发  │  │
│                                        └─────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.6 配置导出/导入

Dashboard 配置可完整导出为 JSON 文件：

```json
{
  "version": "1.0",
  "dashboard": {
    "layout": "grid",
    "cards": [
      {"id": "agx_status", "type": "device_status", "source": "agx", "position": {"x": 0, "y": 0}},
      {"id": "lpmu_status", "type": "device_status", "source": "lpmu", "position": {"x": 1, "y": 0}},
      {"id": "power_card", "type": "power_monitor", "position": {"x": 2, "y": 0}},
      {"id": "commands", "type": "command_panel", "position": {"x": 0, "y": 1}},
      {"id": "rules", "type": "rule_list", "position": {"x": 1, "y": 1, "width": 2}},
      {"id": "led_preview", "type": "led_preview", "position": {"x": 0, "y": 2, "width": 3}}
    ]
  },
  "sources": [...],
  "ssh_hosts": [...],
  "rules": [...],
  "commands": [...]
}
```

---

## 8. 组件设计

### 8.1 组件列表

| 组件 | 路径 | 职责 | 状态 |
|------|------|------|------|
| `ts_automation` | `components/ts_automation/` | 核心引擎：配置加载、调度协调、**变量存储** | ✅ 已实现 |
| `ts_source_manager` | `components/ts_automation/src/` | 数据源管理：内置/WS/REST/SSH 采集 | ✅ 已实现 |
| `ts_rule_engine` | `components/ts_automation/src/` | 规则引擎：条件解析、表达式求值 | ✅ 已实现 |
| `ts_action_manager` | `components/ts_automation/src/` | 动作执行：统一动作接口 | ✅ 已实现 |
| `ts_variable` | `components/ts_automation/src/` | 变量存储：统一数据点管理、变量引用 | ✅ 已实现 |

### 8.2 组件依赖

```
ts_automation (核心引擎)
    ├── ts_source_manager (数据源管理)
    │   ├── ts_net (WS/HTTP 客户端)
    │   ├── ts_security (SSH)
    │   └── ts_variable (变量存储) ✅
    ├── ts_rule_engine (规则引擎)
    │   └── ts_variable ✅
    ├── ts_action_manager (动作执行)
    │   ├── ts_led (LED API)
    │   ├── ts_security (SSH)
    │   └── ts_variable ✅
    └── ts_storage (配置持久化)
```

> **注意**：`ts_var` 组件已废弃（2026-01-27），变量系统统一使用 `ts_variable`（位于 `ts_automation` 组件内）。

### 8.3 核心 API 设计

#### ts_automation.h

```c
/**
 * @brief 初始化自动化引擎
 * @param config_path 配置文件路径，NULL 使用默认路径
 * @return ESP_OK 成功
 */
esp_err_t ts_automation_init(const char *config_path);

/**
 * @brief 启动自动化引擎
 */
esp_err_t ts_automation_start(void);

/**
 * @brief 停止自动化引擎
 */
esp_err_t ts_automation_stop(void);

/**
 * @brief 重新加载配置
 */
esp_err_t ts_automation_reload(void);

/**
 * @brief 获取引擎状态
 */
ts_automation_status_t ts_automation_get_status(void);
```

#### ts_source.h

```c
/**
 * @brief 注册数据源
 */
esp_err_t ts_source_register(const ts_source_config_t *config);

/**
 * @brief 获取变量值
 * @param source_id 数据源 ID
 * @param field_id 字段 ID
 * @param value 输出值
 */
esp_err_t ts_source_get_value(const char *source_id, const char *field_id, 
                               ts_value_t *value);

/**
 * @brief 注册内置数据提供者
 */
esp_err_t ts_source_register_builtin(const char *builtin_id, 
                                      ts_builtin_provider_fn provider);
```

#### ts_rule.h

```c
/**
 * @brief 注册规则
 */
esp_err_t ts_rule_register(const ts_rule_config_t *config);

/**
 * @brief 启用/禁用规则
 */
esp_err_t ts_rule_set_enabled(const char *rule_id, bool enabled);

/**
 * @brief 手动触发规则（测试用）
 */
esp_err_t ts_rule_trigger(const char *rule_id);
```

#### ts_action.h

```c
/**
 * @brief 注册动作执行器
 */
esp_err_t ts_action_register(const char *type, ts_action_handler_fn handler);

/**
 * @brief 执行动作
 */
esp_err_t ts_action_execute(const ts_action_t *action);
```

### 8.4 ts_variable 统一变量系统（✅ 已实现）

`ts_variable` 是自动化引擎的**唯一**变量存储层，提供类型安全的数据点管理和变量引用功能。

> **历史变更**：原 `ts_var` 组件已于 2026-01-27 废弃并删除，所有功能统一到 `ts_variable`。

#### 组件路径

```
components/ts_automation/
├── CMakeLists.txt
├── include/
│   └── ts_variable.h     # 统一变量 API
└── src/
    └── ts_variable.c     # 实现（PSRAM 分配、类型安全）
```

#### 核心 API

```c
#include "ts_variable.h"

// 初始化（ts_automation 服务启动时自动调用）
esp_err_t ts_variable_init(void);

// 检查初始化状态（用于跨阶段调用）
bool ts_variable_is_initialized(void);

// 类型安全的设置 API（受 READONLY 标志保护）
esp_err_t ts_variable_set_string(const char *name, const char *value, const char *source_id);
esp_err_t ts_variable_set_int(const char *name, int value, const char *source_id);
esp_err_t ts_variable_set_float(const char *name, float value, const char *source_id);
esp_err_t ts_variable_set_bool(const char *name, bool value, const char *source_id);

// 内部 API（绕过 READONLY 检查，仅限系统组件使用）
esp_err_t ts_variable_set_internal(const char *name, const ts_auto_value_t *value);

// 类型安全的获取 API
esp_err_t ts_variable_get(const char *name, ts_auto_value_t *value);
const char* ts_variable_get_string(const char *name, const char *default_val);
int ts_variable_get_int(const char *name, int default_val);
float ts_variable_get_float(const char *name, float default_val);
bool ts_variable_get_bool(const char *name, bool default_val);

// 检查存在性
bool ts_variable_exists(const char *name);

// 枚举变量
esp_err_t ts_variable_iterate(ts_variable_iterate_fn callback, void *user_data);

// 按源删除变量
esp_err_t ts_variable_delete_by_source(const char *source_id);
```

#### 只读变量与内部更新

某些系统变量（如 `power_policy.*`）被标记为 `TS_AUTO_VAR_READONLY`，用户无法通过 API 修改。
系统组件需要更新这些变量时，使用 `ts_variable_set_internal()` 绕过只读检查：

```c
// 电压保护模块更新只读变量
void ts_power_policy_update_variables(void) {
    ts_auto_value_t val;
    val.type = TS_AUTO_VAL_FLOAT;
    val.value.f = current_voltage;
    ts_variable_set_internal("power_policy.voltage", &val);  // 绕过只读检查
}
```

#### SSH 命令结果变量

SSH 命令执行后自动存储 7 个标准变量（通过 `source_id` 分组）：

| 变量名 | 类型 | 描述 |
|--------|------|------|
| `<source_id>.status` | STRING | 执行状态：running/completed/cancelled/failed |
| `<source_id>.exit_code` | INT | 退出码（0=成功） |
| `<source_id>.extracted` | STRING | 正则提取的内容 |
| `<source_id>.expect_matched` | BOOL | 是否匹配成功模式 |
| `<source_id>.fail_matched` | BOOL | 是否匹配失败模式 |
| `<source_id>.host` | STRING | 执行主机 |
| `<source_id>.timestamp` | INT | 执行时间戳 |

#### 数据源集成示例

```c
// ts_source_manager 将远程数据映射为变量
void on_data_received(const char *source_id, cJSON *data) {
    // 使用 JSONPath 提取值
    double temp = ts_jsonpath_get_number(data, "temperature.cpu", 0.0);
    
    // 直接存储为 float 类型变量
    ts_variable_set_float("agx.cpu_temp", (float)temp, source_id);
}
```

#### 规则引擎集成示例

```c
// ts_rule_engine 评估条件表达式
bool evaluate_condition(const char *expr) {
    // 解析表达式: "agx.cpu_temp > 80"
    char var_name[64], op[8];
    double threshold;
    parse_expression(expr, var_name, op, &threshold);
    
    // 使用类型安全的 API 获取值
    float value = ts_variable_get_float(var_name, 0.0f);
    
    // 比较
    return compare(value, op, threshold);
}
```

#### 动作执行集成示例

```c
// ts_action_manager 执行 SSH 命令时使用变量引用
esp_err_t execute_ssh_action(const ts_action_t *action) {
    // 原始命令: "echo Temperature: ${agx.cpu_temp}"
    // 获取变量值
    const char *temp = ts_variable_get_string("agx.cpu_temp", "N/A");
    
    // 构建最终命令
    char expanded[512];
    snprintf(expanded, sizeof(expanded), "echo Temperature: %s", temp);
    
    return ts_ssh_execute(action->host, expanded, ...);
}
```

#### 变量命名约定

| 前缀 | 来源 | 示例 |
|------|------|------|
| `sys.*` | 系统内置 | `sys.time`, `sys.ip` |
| `agx.*` | Jetson AGX 数据 | `agx.cpu_temp`, `agx.gpu_usage` |
| `esp32.*` | ESP32 本地数据 | `esp32.heap_free`, `esp32.chip_temp` |
| `last.*` | 最后一次 SSH 结果 | `last.extracted`, `last.status` |
| `<cmd>.*` | 指定命令的结果 | `ping_test.exit_code` |

#### 内存特性

- 64 个变量槽位，每个约 336 字节
- 总计约 21KB，优先分配到 **PSRAM**
- 自动 fallback 到 DRAM

---

## 9. 实现计划

### Phase 1: 核心框架 ✅ **已完成**

**目标**: 建立基本架构，配置加载

- [x] 创建 `ts_automation` 组件骨架 ✅
- [x] 定义配置 JSON Schema ✅
- [x] 实现配置文件加载 (SD 卡 / NVS) ✅
- [x] 实现配置验证 ✅
- [x] 创建 `ts_variable` 统一变量存储 ✅ (2026-01-27)

> **注意**：原 `ts_var` 组件已废弃，统一使用 `ts_variable`。

**验收标准**: 能够加载并解析 JSON 配置文件 ✅

### Phase 2: 数据源系统 ✅ **已完成**

**目标**: 实现数据采集

- [x] 创建 `ts_source_manager` 组件 ✅
- [x] 实现内置数据源 (ESP32 系统/网络/ADC) ✅
- [x] 实现 WebSocket/Socket.IO 数据源 ✅
- [x] 实现 REST 数据源 ✅
- [x] 实现 SSH 命令数据源 ✅
- [x] JSON Path 字段提取 ✅
- [x] 数据更新事件发布 ✅

**验收标准**: 能够采集 ESP32 本地数据和远程数据 ✅

### Phase 3: 规则引擎 (预计 3 天)

**目标**: 实现条件判断

- [x] 创建 `ts_rule_engine` 组件 ✅
- [ ] 实现表达式解析器
- [ ] 实现条件求值
- [ ] 实现触发器类型 (condition/event/timer)
- [ ] 防抖/持续时间逻辑
- [ ] 规则优先级处理

**验收标准**: 能够根据条件触发规则

### Phase 4: 动作系统 (预计 2 天)

**目标**: 实现动作执行

- [x] 创建 `ts_action_manager` 组件 ✅
- [ ] 实现 LED 动作 (touch/board/matrix)
- [x] 实现 SSH 动作 (集成现有 API) ✅
- [ ] 实现日志/变量/延时动作
- [ ] 条件动作支持
- [ ] 动作链式执行

**验收标准**: 规则触发后能正确执行动作

### Phase 5: WebUI (预计 4 天)

**目标**: 可视化配置界面

- [ ] 数据源配置页面
- [ ] 规则编辑器
- [ ] 命令面板配置
- [ ] SSH 主机管理
- [ ] 实时监控面板
- [ ] 配置导入/导出

**验收标准**: 能够通过 WebUI 完整配置自动化系统

### Phase 6: 测试与优化 (预计 2 天)

- [ ] 单元测试
- [ ] 集成测试
- [ ] 性能优化
- [ ] 内存使用优化
- [ ] 文档完善

---

## 10. 附录

### A. 变量引用语法

```
${source_id.field_id}           # 基本引用
${source_id.field_id:%.2f}      # 带格式化
${source_id.field_id:default}   # 默认值
```

### B. 内置事件列表

| 事件 | 说明 |
|------|------|
| `startup` | 系统启动完成 |
| `data.updated.<source_id>` | 数据源更新 |
| `ssh.completed` | SSH 命令完成 |
| `ssh.timeout` | SSH 命令超时 |
| `network.wifi_connected` | WiFi 连接 |
| `network.wifi_disconnected` | WiFi 断开 |
| `network.ip_changed` | IP 地址变更 |
| `service.started.<name>` | 服务启动 |
| `service.stopped.<name>` | 服务停止 |
| `timer.<interval>` | 定时器触发 |

### C. 颜色格式

支持以下颜色格式：

- 十六进制: `#FF0000`, `#F00`
- RGB: `rgb(255, 0, 0)`
- 命名颜色: `red`, `green`, `blue`, `yellow`, `orange`, `white`, `black`
- 渐变: `gradient` (根据数值自动映射)

### D. 现有 HAL/驱动组件映射

| 组件 | 头文件 | 用于内置数据源 |
|------|--------|---------------|
| `ts_hal` | `ts_hal.h` | 系统能力、GPIO、ADC、PWM |
| `ts_hal_gpio` | `ts_hal_gpio.h` | GPIO 状态读取/控制 |
| `ts_hal_adc` | `ts_hal_adc.h` | ADC 电压读取 |
| `ts_power_monitor` | `ts_power_monitor.h` | 电源电压/电流监控 |
| `ts_device_ctrl` | `ts_device_ctrl.h` | AGX/LPMU 电源控制 |
| `ts_net` | `ts_net.h` | 网络状态 |
| `ts_wifi` | `ts_wifi.h` | WiFi 连接状态 |
| `ts_eth` | `ts_eth.h` | 以太网状态 |
| `ts_led` | `ts_led.h` | LED 控制 |
| `ts_service` | `ts_service.h` | 服务状态 |
| `ts_cert` | `ts_cert.h` | PKI 证书状态 |

### E. 引脚配置参考 (rm01_esp32s3)

| 功能 | GPIO | 说明 |
|------|------|------|
| LED_TOUCH | 45 | Touch LED WS2812 |
| LED_BOARD | 42 | Board LED WS2812 |
| LED_MATRIX | 9 | Matrix LED WS2812 |
| POWER_ADC | 18 | 电压监控 ADC (11.4:1 分压) |
| POWER_UART_RX | 47 | 电源芯片 UART |
| AGX_FORCE_SHUTDOWN | 3 | AGX 强制关机 |
| AGX_RESET | 1 | AGX 复位 |
| AGX_RECOVERY | 40 | AGX 恢复模式 |
| LPMU_POWER | 46 | LPMU 电源按钮 |
| LPMU_RESET | 2 | LPMU 复位 |
| FAN_PWM_0 | 41 | 风扇 PWM |

### F. 配置导入导出

自动化配置支持通过 Config Pack 系统进行加密导入导出：

#### API 端点

| API | 功能 |
|-----|------|
| `automation.sources.export` | 导出数据源为 .tscfg |
| `automation.sources.import` | 导入数据源配置包 |
| `automation.rules.export` | 导出规则为 .tscfg |
| `automation.rules.import` | 导入规则配置包 |
| `automation.actions.export` | 导出动作模板为 .tscfg |
| `automation.actions.import` | 导入动作模板配置包 |

#### 存储路径

| 配置类型 | SD 卡路径 |
|---------|----------|
| 数据源 | `/sdcard/config/sources/` |
| 规则 | `/sdcard/config/rules/` |
| 动作模板 | `/sdcard/config/actions/` |

#### 导入流程

1. 上传 .tscfg 文件
2. 后端验证签名（不解密）
3. 预览配置 ID、签名者信息
4. 确认导入 → 保存到 SD 卡
5. 重启系统后自动解密并加载

详细 API 文档参见：[API_REFERENCE.md](API_REFERENCE.md#自动化配置导入导出-api)

### G. 修订历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0 | 2026-01-26 | 初始版本 |
| 1.1 | 2026-01-26 | 更新内置数据源映射到实际代码；重新设计 Dashboard WebUI |
| 2.2 | 2026-02-04 | 添加配置导入导出功能文档 |
