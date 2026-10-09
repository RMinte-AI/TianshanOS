# TianShanOS Copilot 指南

## 项目概述

TianShanOS 是**面向配置而非面向代码**的嵌入式 OS，基于 ESP-IDF v5.5+，用于 NVIDIA Jetson AGX 载板机架管理。目标芯片：ESP32-S3 / ESP32-P4。

## 核心架构

```
CLI/WebUI → Core API (ts_api) → 服务管理(8阶段) → 事件总线 → HAL → 平台适配层
```

**关键约束**：
- 所有组件通过事件总线（`ts_event`）解耦，**禁止**直接调用其他组件内部函数
- GPIO 引脚**禁止硬编码**，必须通过 `boards/*/pins.json` 配置
- 命名规范：组件前缀 `ts_`，宏/枚举前缀 `TS_`

## 事件系统（组件通信唯一方式）

```c
// 发布事件（参考 ts_event.h）
ts_event_post(TS_EVENT_BASE_LED, TS_EVENT_LED_CHANGED, &data, sizeof(data), timeout_ms);

// 订阅事件
ts_event_handler_register(TS_EVENT_BASE_LED, TS_EVENT_ANY_ID, handler_fn, user_data);
```

## Core API 层（CLI/WebUI 统一接口）

CLI 和 WebUI **必须**通过 `ts_api` 调用功能，禁止直接调用组件函数（参考 `ts_api.h`）：

```c
// API 命名格式：<category>.<action>
// 示例：system.reboot, led.set_color, device.agx.power

// 注册 API
ts_api_endpoint_t ep = {
    .name = "led.set_brightness",
    .category = TS_API_CAT_LED,
    .handler = api_led_set_brightness,
    .requires_auth = false,
};
ts_api_register(&ep);

// 调用 API（返回 ts_api_result_t 包含 code/message/JSON data）
ts_api_call("led.set_brightness", params_json, &result);
```

## 服务系统（main/ts_services.c）

服务是系统基本单元，必须实现 4 个回调：

```c
static esp_err_t xxx_service_init(ts_service_handle_t h, void *d);   // 初始化资源
static esp_err_t xxx_service_start(ts_service_handle_t h, void *d);  // 启动服务
static esp_err_t xxx_service_stop(ts_service_handle_t h, void *d);   // 停止服务
static bool xxx_service_health(ts_service_handle_t h, void *d);      // 健康检查
```

**启动阶段**（按顺序）：`PLATFORM → CORE → HAL → DRIVER → NETWORK → SECURITY → SERVICE → UI`

新服务在 `main/ts_services.c` 注册，使用 `ts_service_config_t` 指定 `.phase` 和 `.depends_on`。

## 配置驱动开发

- `boards/rm01_esp32s3/pins.json` - 逻辑名 → GPIO 映射（如 `"FAN_PWM_0": {"gpio": 41}`）
- `boards/rm01_esp32s3/services.json` - 服务启用、依赖、运行时参数
- 配置优先级：CLI/API 实时修改 > NVS 持久化 > SD 卡文件 > 代码默认值

## CLI 命令实现（argtable3 风格）

命令使用**参数风格**（非子命令），示例：

```bash
fan --status --id 0 --json   # ✓ 正确
led --effect --device matrix --name fire --speed 50
```

**新增命令步骤**：
1. 创建 `components/ts_console/commands/ts_cmd_xxx.c`
2. 使用 `argtable3` 定义参数结构（参考 `ts_cmd_led.c` 的 `s_led_args`）
3. 在 `ts_cmd_register.c` 调用 `ts_cmd_xxx_register()`
4. 在 `include/ts_cmd_all.h` 添加函数声明

## 版本管理

版本号格式：`MAJOR.MINOR.PATCH+HASH.TIMESTAMP`，例如 `0.3.1+7fdcca4d.01311234`

- `version.txt` - 语义版本号（手动维护）
- `tools/gen_version.py` - 自动生成完整版本字符串
- 版本号嵌入 `esp_app_desc_t`，可通过 `system --info` 或 WebUI 查看

**版本号只在 CMake 配置阶段更新**，增量编译不会改变版本号。

## 开发工作流

### 构建命令

```bash
# 日常开发 - 快速增量编译（版本号不变）
idf.py build

# 发布构建 - 强制更新版本号（推荐用于 OTA 发布）
./tools/build.sh --fresh

# 完整清理构建（最干净但最慢）
./tools/build.sh --clean
# 或
idf.py fullclean && idf.py build
```

### 重要：Copilot 构建规则

**当用户要求构建/编译项目时，Copilot 必须执行以下操作：**

1. **始终使用 `./tools/build.sh --fresh`** 而不是 `idf.py build`
2. 这确保每次构建都有唯一的版本号时间戳
3. 构建完成后，如果 OTA 服务器正在运行，提示用户重启服务器

```bash
# Copilot 执行构建的标准流程
./tools/build.sh --fresh
# 如果需要更新 OTA 服务器
sudo systemctl restart tianshan-ota
```

### 重要：OTA 发布前必须刷新版本号

**发布 OTA 更新时，必须使用 `--fresh` 或 `--clean` 选项确保版本号更新**：

```bash
./tools/build.sh --fresh          # 刷新版本号并构建
sudo systemctl restart tianshan-ota  # 重启 OTA 服务器
```

### 其他常用命令

```bash
idf.py set-target esp32s3      # 设置目标芯片
idf.py menuconfig              # TianShanOS 选项在顶层菜单
idf.py flash monitor           # 烧录并监控
```

## 代码约定

```c
static const char *TAG = "模块名";  // 每个 .c 文件必须定义

// 错误处理 - 返回 esp_err_t，使用 esp_err_to_name() 打印
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret));
    return ret;
}
```

- 头文件：Doxygen 注释（`@brief`, `@param`, `@return`），**英文**
- 源文件：中文注释用于架构说明

## 内存分配规范（PSRAM 优先）

ESP32-S3 配备 8MB PSRAM，**所有 ≥128 字节的动态分配应优先使用 PSRAM**，以减少 DRAM 碎片。

### 强制规则

```c
// ❌ 禁止：大型静态数组直接占用 DRAM
static char large_buffer[8192];
static uint8_t frame_buffer[32 * 8 * 3];

// ✅ 正确：动态分配到 PSRAM
static char *large_buffer = NULL;
void init(void) {
    large_buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
}

// ✅ 正确：带 fallback 的安全分配
void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
if (!buf) {
    buf = malloc(size);  // Fallback to DRAM
    ESP_LOGW(TAG, "PSRAM unavailable, using DRAM");
}

// ❌ 禁止：循环中栈分配大型结构体（会导致栈溢出）
for (int i = 0; i < count; i++) {
    ts_action_template_t temp;  // ~1KB 结构体在栈上！
}

// ✅ 正确：循环外堆分配，循环内复用
ts_action_template_t *temp = heap_caps_malloc(sizeof(*temp), MALLOC_CAP_SPIRAM);
for (int i = 0; i < count; i++) {
    memset(temp, 0, sizeof(*temp));
    // 使用 temp...
}
free(temp);
```

### 分配策略

| 大小 | 分配方式 | 说明 |
|------|---------|------|
| < 128 字节 | `malloc()` | 小分配用 DRAM（速度快） |
| ≥ 128 字节 | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | 中大分配用 PSRAM |
| 循环中的大型结构体 | **必须堆分配** | 避免栈溢出 |
| DMA 缓冲区 | `heap_caps_malloc(MALLOC_CAP_DMA)` | DMA 必须用 DRAM |
| 任务栈 | 默认 DRAM，大栈可用 PSRAM | 使用 `xTaskCreateWithCaps()` |

### sdkconfig.defaults 关键配置

```kconfig
# PSRAM 分配策略（已配置，勿修改）
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=128    # <128B 用 DRAM
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=8192 # 保留 8KB DRAM
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y     # WiFi 缓冲区优先 PSRAM
CONFIG_NVS_ALLOCATE_CACHE_IN_SPIRAM=y      # NVS 缓存用 PSRAM
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y        # TLS 用 PSRAM
```

### 检查清单（Code Review）

新增代码时检查：
- [ ] 无 `static char xxx[>128]` 大型静态数组
- [ ] 无 `malloc(>1024)` 直接调用（应使用 `heap_caps_malloc`）
- [ ] 帧缓冲区、日志缓冲区、网络缓冲区使用 PSRAM
- [ ] DMA 缓冲区显式使用 `MALLOC_CAP_DMA`

## 组件结构模板

```
components/ts_xxx/
├── CMakeLists.txt           # REQUIRES/PRIV_REQUIRES 声明依赖
├── Kconfig                  # CONFIG_TS_XXX_* 配置选项
├── include/ts_xxx.h         # 公开 API
└── src/ts_xxx.c             # 实现
```

CMakeLists.txt 示例（参考 `ts_led/CMakeLists.txt`）：
```cmake
idf_component_register(
    SRCS "src/ts_xxx.c"
    INCLUDE_DIRS "include"
    REQUIRES ts_core ts_hal           # 公开依赖（头文件暴露）
    PRIV_REQUIRES ts_storage ts_event # 私有依赖（仅实现使用）
)
```

## 关键文件索引

| 用途 | 路径 |
|------|------|
| 服务注册入口 | `main/ts_services.c` |
| 事件系统 API | `components/ts_core/ts_event/include/ts_event.h` |
| 服务管理 API | `components/ts_core/ts_service/include/ts_service.h` |
| 统一 API 层 | `components/ts_api/include/ts_api.h` |
| CLI 命令实现 | `components/ts_console/commands/ts_cmd_*.c` |
| 命令注册汇总 | `components/ts_console/commands/ts_cmd_register.c` |
| 板级引脚配置 | `boards/rm01_esp32s3/pins.json` |
| 安全/SSH 模块 | `components/ts_security/include/ts_ssh_client.h` |
| 设备驱动框架 | `components/ts_drivers/` |
| 分区表 | `partitions.csv`（factory 3MB / storage SPIFFS / www WebUI）|
| 版本号文件 | `version.txt` |
| 版本生成脚本 | `tools/gen_version.py` |
| 构建脚本 | `tools/build.sh` |
| OTA 服务器 | `tools/ota_server/ota_server.py` |
| OTA 服务管理 | `tools/ota_server/ota_service.sh` |

## 参考项目

robOS 是 TianShanOS 的前身项目，包含已验证的硬件驱动和控制逻辑：
- **仓库地址**：https://github.com/RMinte-AI/TianshanOS
- **主要分支**：`ThorPlusBattery`（电池/电压保护功能）
- **本地路径**：`/Users/thomas/rm01/robOS`

## 开发路线图（当前阶段）

| 优先级 | 任务 | 状态 | 关键文件 |
|--------|------|------|----------|
| 0 | SSH 客户端 & 安全功能 | 🚧 框架已有 | `ts_security/`, `ts_ssh_client.h` |
| 1 | device 完整功能（AGX/LPMU/监控） | 🚧 CLI 模拟 | `ts_cmd_device.c`, `ts_drivers/device/` |
| 2 | Core API 规范化 | 🚧 框架已有 | `ts_api/` |
| 3 | WebUI 开发 | ⏳ 待开始 | `ts_webui/` |
| 4 | 测试框架 | ⏳ 待开始 | `tests/` |

### Device 模块现状
- `ts_device_ctrl.c` - AGX 电源控制**已实现**（power_on/off/reset/force_off + GPIO 时序）
- `ts_cmd_device.c` - CLI 命令**模拟实现**，需接入 `ts_device_*` API
- LPMU 配置函数待补充（目前只有 `ts_device_configure_agx`）
- 设备监控待实现（电压/电流通过 ADC 或 UART PZEM 协议）

### Device 模块整合计划（源自 robOS）
需要将 robOS 中分散的功能整合到统一的 device 模块：

| robOS 组件 | 功能 | TianShanOS 目标 |
|-----------|------|----------------|
| `device_controller` | AGX/LPMU 电源控制 | `ts_device_ctrl` ✅ 已有框架 |
| `agx_monitor` | WebSocket 实时监控 | `ts_device_monitor` 待实现 |
| `power_monitor` | ADC/UART 电源监测 | `ts_power_monitor` ✅ 已移植 |
| `voltage_protection` | 低电压保护/自动恢复 | `ts_power_policy` 待实现 |
| `usb_mux_controller` | USB 切换 | `ts_usb_mux` 待实现 |

### 电压保护逻辑（robOS voltage_protection）

**状态机**：
```
NORMAL → LOW_VOLTAGE → SHUTDOWN → PROTECTED → RECOVERY → NORMAL
         (倒计时)      (执行关机)   (等待恢复)   (重启ESP32)
```

**阈值配置**：
- `low_voltage_threshold`: 12.6V（进入 LOW_VOLTAGE 状态）
- `recovery_voltage_threshold`: 18.0V（允许恢复）
- `shutdown_delay_sec`: 60s（关机前倒计时）
- `recovery_hold_sec`: 5s（电压恢复后稳定等待）

**关键行为**：
1. 电压 < 12.6V → 开始 60s 倒计时
2. 倒计时期间电压恢复 ≥ 18V → 取消关机，回到 NORMAL
3. 倒计时归零 → 执行关机（AGX reset HIGH，LPMU toggle）
4. PROTECTED 状态下电压恢复 ≥ 18V → 等待 5s 稳定
5. 稳定后 → **重启 ESP32**（esp_restart）恢复系统

### AGX Monitor 规格（WebSocket）
- 协议：Socket.IO over WebSocket
- 服务器：`ws://<AGX_IP>:58090/socket.io/`
- 数据：CPU/内存/温度/功耗/GPU（JSON，1Hz）
- 启动延迟：45秒（等待 L4T/Ubuntu 启动）

### Core API 设计原则
- **唯一入口**：CLI / WebUI / HTTPS API 全部通过 `ts_api_call()` 调用
- **传输协议**：HTTPS RESTful + 加密 WebSocket
- **认证机制**：用于验证操作者是否有权限调用 Core API（Token/Session 待定）

## 命令参考

完整 CLI 命令文档见 `docs/COMMANDS.md`。常用命令：
- `system --info/--memory/--tasks/--reboot`
- `service --list/--status/--start/--stop --name <name>`
- `led --status/--effect/--image --device <touch|board|matrix>`
- `net --status/--set --mode static --ip x.x.x.x`
- `config --get/--set --key <key> --value <val> --persist`