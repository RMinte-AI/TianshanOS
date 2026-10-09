# TianshanOS 项目描述文件

> 基于 robOS 项目的重构规划文档
> 
> 创建日期：2026年1月14日

---

## 📋 项目概述

### 原始项目
**项目名称：** robOS  
**GitHub仓库：** https://github.com/RMinte-AI/TianshanOS<br>
**项目定位：** RM-01 板上机架操作系统  
**开发平台：** ESP32S3  
**开发框架：** ESP-IDF v5.5.1  
**最新版本：** v1.1.0 (2025年10月6日)

### 核心设计理念
- **完全模块化**：每个功能都是独立组件
- **控制台为核心**：统一的命令行交互接口
- **事件驱动**：组件间通过事件系统通信
- **平台级组件化**：连硬件抽象层也是独立组件
- **可扩展性优先**：为未来功能扩展预留接口

---

## 🔧 硬件配置

### ESP32S3 开发板配置
- **开发板型号**：ESP32S3
- **串口连接**：`/dev/cu.usbmodem01234567891`
- **波特率**：115200
- **配置文件**：`.esp_config`

### 核心硬件资源

#### 1. LED 显示系统
| 组件 | GPIO引脚 | 数量 | 驱动芯片 | 用途 |
|------|---------|------|---------|------|
| 板载 LED | GPIO 42 | 28颗 | WS2812 | 系统状态指示和装饰照明 |
| 触控 LED | GPIO 45 | 1颗 | WS2812 | 用户交互反馈 |
| 矩阵 LED | GPIO 9 | 1024颗(32x32) | WS2812 | 显示屏和图形界面 |

#### 2. 风扇控制系统
- **PWM引脚**：GPIO 41
- **PWM规格**：25kHz频率，10位分辨率 (0-1023)
- **控制模式**：
  - 手动模式（固定转速）
  - 自动温度模式（基于传感器）
  - 自定义曲线模式（温度曲线）
  - 关闭模式

#### 3. USB MUX 控制
- **MUX1引脚**：GPIO 8
- **MUX2引脚**：GPIO 48
- **支持目标**：ESP32S3、AGX、N305

#### 4. 以太网控制器（W5500）
- **接口类型**：SPI2_HOST
- **引脚配置**：
  - RST: GPIO 39
  - INT: GPIO 38
  - MISO: GPIO 13
  - SCLK: GPIO 12
  - MOSI: GPIO 11
  - CS: GPIO 10
- **网络配置**：
  - 默认IP：10.10.99.97
  - 子网掩码：255.255.255.0
  - 网关：10.10.99.100
- **DHCP服务器**：IP池 10.10.99.100-110

#### 5. TF卡存储（SDMMC 4-bit）
- **接口类型**：SDMMC 4-bit模式
- **时钟频率**：40MHz（高速模式）
- **引脚配置**：
  - CMD: GPIO 4
  - CLK: GPIO 5
  - D0: GPIO 6
  - D1: GPIO 7
  - D2: GPIO 15
  - D3: GPIO 16
- **文件系统**：FAT32

#### 6. 电源监控系统
- **电压监测**：GPIO 18 (ADC2_CHANNEL_7)
  - 分压比：11.4:1
  - 支持高电压检测
- **电源芯片通信**：GPIO 47 (UART1_RX)
  - 波特率：9600，8N1
  - 数据格式：`[0xFF帧头][电压][电流][CRC]` (4字节)

#### 7. 设备控制引脚
- **AGX电源控制**：GPIO 1
- **LPMU电源控制**：GPIO 2
- **LPMU复位控制**：GPIO 3

---

## 🏗️ 软件架构

### 组件层次结构

```
robOS 架构
├── 硬件抽象层 (Hardware HAL)
│   ├── GPIO抽象
│   ├── PWM抽象
│   ├── SPI抽象
│   ├── ADC抽象
│   └── UART抽象
│
├── 核心组件
│   ├── 事件管理器 (Event Manager)
│   ├── 配置管理器 (Config Manager)
│   ├── 控制台核心 (Console Core)
│   └── 存储管理器 (Storage Manager)
│
├── 硬件控制组件
│   ├── GPIO控制器 (GPIO Controller)
│   ├── USB MUX控制器 (USB MUX Controller)
│   ├── 设备控制器 (Device Controller)
│   └── 风扇控制器 (Fan Controller)
│
├── LED系统组件
│   ├── 触控LED (Touch LED)
│   ├── 板载LED (Board LED)
│   ├── 矩阵LED (Matrix LED)
│   └── 色彩校正 (Color Correction)
│
├── 网络通信组件
│   ├── 以太网管理器 (Ethernet Manager)
│   ├── AGX监控 (AGX Monitor)
│   └── Web服务器 (Web Server)
│
└── 监控组件
    ├── 电源监控 (Power Monitor)
    └── 系统监控 (System Monitor)
```

### 核心组件详细说明

#### 1. 硬件抽象层 (hardware_hal)
**功能职责**：
- 统一的GPIO、PWM、SPI、ADC、UART接口
- 硬件资源初始化和配置
- 底层驱动封装

**API设计**：
```c
esp_err_t hardware_hal_init(void);
esp_err_t hal_gpio_configure(const hal_gpio_config_t *config);
esp_err_t hal_pwm_configure(const hal_pwm_config_t *config);
esp_err_t hal_adc_configure(const hal_adc_config_t *config);
```

#### 2. 事件管理器 (event_manager)
**功能职责**：
- 组件间事件驱动通信
- 事件发布和订阅机制
- 异步事件处理

**特性**：
- 9个测试用例全部通过
- 线程安全的事件分发
- 支持事件优先级

#### 3. 控制台核心 (console_core)
**功能职责**：
- UART命令行接口
- 命令解析和执行
- 帮助系统和历史记录

**特性**：
- 8个测试用例全部通过
- 支持命令自动补全
- 命令历史记录功能

#### 4. 配置管理器 (config_manager)
**功能职责**：
- 统一的NVS配置管理
- 多数据类型支持
- 配置持久化存储

**特性**：
- 支持布尔、整数、浮点、字符串、二进制数据
- 自动提交机制
- 版本控制

#### 5. 风扇控制器 (fan_controller)
**功能职责**：
- PWM风扇控制
- 多模式运行（手动、自动、曲线、关闭）
- 温度曲线管理（2-10个控制点，线性插值）

**智能温度管理**：
- 温度源优先级：手动 > AGX自动 > 安全默认
- 分层安全保护：
  - 启动保护（60秒内75°C）
  - 离线保护（AGX未连接85°C）
  - 过期保护（数据超时65°C）
  - 备用温度（45°C）

**命令系统**：
```bash
fan status                  # 显示风扇状态
fan set <id> <speed>        # 设置转速
fan mode <id> <mode>        # 设置工作模式
fan config curve <id> ...   # 配置温度曲线
temp set <value>            # 设置测试温度
temp auto/manual            # 切换温度模式
```

#### 6. AGX监控器 (agx_monitor)
**功能职责**：
- WebSocket实时监控AGX系统
- CPU温度数据采集和推送
- 静默运行模式

**技术特性**：
- 连接协议：WebSocket over Socket.IO
- 服务器地址：ws://10.10.99.98:58090/socket.io/
- 数据更新频率：1Hz
- 启动保护：45秒AGX系统启动延迟
- 自动重连：3次快速重试（1秒间隔）+ 固定间隔（3秒）

**监控数据**：
- CPU信息（多核心使用率、频率）
- 内存信息（RAM、SWAP）
- 温度监控（CPU、SoC0-2、Junction）
- 功耗监控（GPU+SoC、CPU、系统5V、内存）
- GPU信息（3D GPU频率）

#### 7. LED系统组件

##### 触控LED (touch_led)
- **硬件**：GPIO 45，单颗WS2812
- **功能**：
  - 触摸响应（<50ms延迟）
  - 长按功能（>1000ms触发彩虹动画）
  - 双击亮度切换（低亮30 ↔ 高亮200）
  - 多种动画效果（呼吸、彩虹、渐变、脉冲）

##### 板载LED (board_led)
- **硬件**：GPIO 42，28颗WS2812
- **功能**：
  - 独立像素控制
  - 丰富动画效果（淡入淡出、彩虹、呼吸、波浪、追逐、闪烁、火焰、脉冲、渐变）
  - 亮度调节（0-255）
  - 配置持久化

##### 矩阵LED (matrix_led)
- **硬件**：GPIO 9，32x32 WS2812矩阵（1024颗）
- **功能**：
  - 像素级精确控制
  - 几何图形绘制（直线、矩形、圆形）
  - 动画播放系统（彩虹、波浪、呼吸、旋转、渐变）
  - 图像导入导出（SD卡）
  - 亮度调节（0-100%）
  - 多种显示模式（静态、动画、自定义、关闭）

##### 色彩校正 (color_correction)
- **功能**：
  - 白点校正（RGB通道独立调节0.0-2.0）
  - 伽马校正（0.1-4.0，推荐2.2）
  - 亮度增强（0.0-2.0）
  - 饱和度增强（0.0-2.0）
  - 配置导入导出

#### 8. 以太网管理器 (ethernet_manager)
**功能职责**：
- W5500以太网控制器驱动
- DHCP服务器和客户端支持
- 网络状态监控和统计
- 活动日志记录

**网络功能**：
- 静态IP配置
- DHCP服务器（IP池管理、租期管理、客户端跟踪）
- 网络诊断工具
- 配置持久化

**命令系统**：
```bash
net status              # 显示网络状态
net config set ...      # 配置网络参数
net dhcp enable/disable # DHCP服务器控制
net log                 # 查看网络日志
net debug               # 网络调试工具
```

#### 9. 存储管理器 (storage_manager)
**功能职责**：
- TF卡自动挂载和卸载
- 文件系统操作（创建、删除、重命名、复制）
- 目录管理
- 空间监控
- 格式化支持

**技术特性**：
- SDMMC 4-bit高速接口（40MHz）
- FAT32文件系统
- POSIX兼容API
- 19个专用控制台命令

#### 10. 电源监控器 (power_monitor)
**功能职责**：
- 实时电压监测（ADC采样）
- 电源芯片数据接收（UART通信）
- 电压、电流、功率计算
- 阈值报警

**技术特性**：
- 电压监测分压比：11.4:1
- UART波特率：9600，8N1
- 数据协议：`[0xFF][电压][电流][CRC]`
- 后台任务持续监控
- 10个专用控制台命令

---

## 🎯 核心功能特性

### 1. 智能温度管理系统
- **温度源优先级策略**：手动测试 > AGX实时 > 安全保护
- **分层安全保护机制**：
  - 启动保护：系统启动60秒内使用75°C
  - 离线保护：AGX未连接时使用85°C
  - 过期保护：数据超时>10秒使用65°C
  - 备用温度：异常情况使用45°C
- **实时温度集成**：AGX CPU温度自动推送到风扇控制（1Hz更新）
- **温度命令系统**：
  ```bash
  temp status     # 显示温度状态
  temp set <值>   # 设置测试温度
  temp auto       # 切换AGX自动模式
  temp manual     # 切换手动模式
  ```

### 2. 完整的LED控制系统
- **三个独立LED子系统**：触控、板载、矩阵
- **全彩色支持**：1677万色（RGB 24位）
- **独立控制**：各子系统完全独立，可同时运行
- **配置持久化**：所有设置自动保存到NVS
- **颜色校正**：统一的色彩校正系统
- **文件支持**：矩阵LED支持图像文件导入导出
- **线程安全**：支持多任务环境

### 3. 网络管理功能
- **完整的网络配置管理**
- **DHCP服务器**（IP池10.10.99.100-110，最多10个客户端）
- **网络活动日志**（最多32条记录）
- **网络调试工具**（时序分析、状态监控）
- **配置持久化**（NVS存储）

### 4. 设备管理功能
- **AGX电源控制**（开机、关机、重启）
- **LPMU电源控制**（开机、关机、复位）
- **USB MUX切换**（ESP32S3/AGX/N305目标）
- **设备状态监控**
- **自动启动配置**

### 5. 存储管理功能
- **TF卡自动挂载**
- **完整的文件操作**（创建、删除、重命名、复制、移动）
- **目录管理**（创建、删除、列表）
- **空间监控**（总容量、已用、可用）
- **格式化支持**

---

## 💻 命令行接口

### 系统命令
```bash
help              # 显示系统概览
version           # 显示版本信息
status            # 显示控制台状态
clear             # 清屏
history           # 显示命令历史
```

### 温度管理命令
```bash
temp status       # 显示温度管理状态
temp get          # 获取当前有效温度
temp set <值>     # 设置手动测试温度
temp auto         # 切换AGX自动模式
temp manual       # 切换手动测试模式
```

### AGX监控命令
```bash
agx_monitor start     # 启动AGX监控
agx_monitor stop      # 停止AGX监控
agx_monitor status    # 显示连接状态
agx_monitor data      # 显示监控数据
agx_monitor config    # 显示配置信息
agx_monitor stats     # 显示统计信息
```

### 风扇控制命令
```bash
fan status                          # 显示风扇状态
fan set <id> <speed>                # 设置转速(0-100%)
fan mode <id> <mode>                # 设置工作模式
fan enable <id> <on/off>            # 启用/禁用风扇
fan gpio <id> <pin> [channel]       # 配置GPIO和PWM通道
fan config curve <id> <points>      # 配置温度曲线
fan config save/load/show           # 配置管理
```

### GPIO控制命令
```bash
gpio <pin> high       # 设置GPIO高电平
gpio <pin> low        # 设置GPIO低电平
gpio <pin> input      # 设置为输入模式
```

### USB MUX控制命令
```bash
usbmux esp32s3        # 切换到ESP32S3
usbmux agx            # 切换到AGX
usbmux n305           # 切换到N305
usbmux status         # 显示当前状态
```

### 设备控制命令
```bash
agx on/off/restart    # AGX电源控制
lpmu on/off/reset     # LPMU电源控制
```

### LED控制命令

#### 触控LED
```bash
led touch status                      # 显示LED状态
led touch set <color>                 # 设置颜色
led touch brightness <0-255>          # 设置亮度
led touch animation start <type>      # 启动动画
led touch sensor enable/disable       # 启用/禁用触摸
led touch config save/load/reset      # 配置管理
```

#### 板载LED
```bash
led board on/off/clear                # 基本控制
led board all <R> <G> <B>             # 设置所有LED
led board set <index> <R> <G> <B>     # 设置特定LED
led board brightness <0-255>          # 设置亮度
led board anim <type>                 # 启动动画
led board config save/load/reset      # 配置管理
```

#### 矩阵LED
```bash
led matrix status                     # 显示矩阵状态
led matrix enable <on/off>            # 启用/禁用矩阵
led matrix brightness <0-100>         # 设置亮度
led matrix clear/fill                 # 清除/填充
led matrix pixel <x> <y> <r> <g> <b>  # 设置像素
led matrix draw <shape>               # 绘制图形
led matrix anim <type>                # 启动动画
led matrix image export/import        # 图像导入导出
led matrix config save/load/reset     # 配置管理
```

#### 色彩校正
```bash
color enable/disable                  # 启用/禁用色彩校正
color status                          # 显示当前配置
color whitepoint <r> <g> <b>          # RGB通道白点校正
color gamma <value>                   # 伽马校正
color brightness <factor>             # 亮度增强
color saturation <factor>             # 饱和度增强
color export/import <file>            # 配置导入导出
color reset                           # 重置为默认
```

### 网络管理命令
```bash
net status                        # 显示网络状态
net start/stop/reset              # 网络接口控制
net config show/save/load/reset   # 配置管理
net config set <param> <value>    # 设置网络参数
net dhcp enable/disable           # DHCP服务器控制
net log [options]                 # 查看网络日志
net debug [command]               # 网络调试工具
```

---

## 🧪 测试与验证

### 单元测试覆盖
- ✅ 事件管理组件：9个测试用例全部通过
- ✅ 硬件抽象层：5个测试用例全部通过
- ✅ 控制台核心：8个测试用例全部通过

### 功能测试状态
- ✅ 配置管理组件：统一NVS配置管理
- ✅ 风扇控制组件：完整PWM控制、温度曲线、智能温度管理
- ✅ AGX监控组件：WebSocket实时监控、CPU温度集成、静默运行
- ✅ 触控LED组件：触摸交互、动画效果、配置持久化
- ✅ 板载LED组件：独立像素控制、丰富动画、配置管理
- ✅ 矩阵LED组件：像素绘图、动画播放、图像文件支持
- ✅ 色彩校正组件：白点/伽马/亮度/饱和度优化
- ✅ 以太网管理组件：W5500控制、DHCP服务器、网络日志
- ✅ 存储管理组件：TF卡管理、文件系统操作、命令行接口
- ✅ 电源监控组件：电压监测、电源芯片通信、功率监控
- ✅ 智能温度管理：多层次安全保护、AGX数据集成、调试模式

---

## 📦 依赖和组件

### ESP-IDF组件依赖
- `driver` - GPIO、UART、SPI、LEDC驱动
- `esp_timer` - 高精度定时器
- `freertos` - 实时操作系统
- `esp_event` - 事件循环系统
- `nvs_flash` - 非易失性存储
- `esp_adc` - ADC驱动
- `fatfs` - FAT文件系统
- `sdmmc` - SD卡驱动

### 外部组件
- `espressif__led_strip` - WS2812 LED驱动（管理组件）
- Socket.IO客户端（AGX监控）

### 组件依赖关系
```
main
├── event_manager
├── hardware_hal
│   ├── driver
│   ├── esp_driver_gpio
│   ├── esp_driver_uart
│   ├── esp_driver_spi
│   └── esp_driver_ledc
├── console_core
│   └── driver
├── config_manager
│   └── nvs_flash
├── fan_controller
│   ├── hardware_hal
│   ├── console_core
│   └── config_manager
├── agx_monitor
│   ├── fan_controller
│   └── config_manager
├── touch_led
│   ├── driver
│   ├── led_strip
│   └── config_manager
├── board_led
│   ├── led_strip
│   ├── console_core
│   └── config_manager
├── matrix_led
│   ├── hardware_hal
│   ├── led_strip
│   ├── color_correction
│   └── fatfs
├── ethernet_manager
│   ├── driver
│   └── config_manager
├── storage_manager
│   ├── fatfs
│   └── sdmmc
└── power_monitor
    ├── hardware_hal
    └── config_manager
```

---

## 📚 技术文档

### 核心文档
- [项目进度记录](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/PROJECT_PROGRESS.md)
- [技术架构文档](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/TECHNICAL_ARCHITECTURE.md)
- [API参考文档](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/API_REFERENCE.md)
- [代码规范](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/CODING_STANDARDS.md)

### 功能使用指南
- [温度集成指南](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/TEMPERATURE_INTEGRATION_GUIDE.md)
- [智能安全温度策略](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/SMART_SAFETY_TEMPERATURE_STRATEGY.md)
- [色彩校正系统完整指南](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/COLOR_CORRECTION_GUIDE.md)

### LED系统文档
- [Touch LED组件](https://github.com/RMinte-AI/TianshanOS/blob/main/components/touch_led/README.md)
- [Matrix LED组件](https://github.com/RMinte-AI/TianshanOS/blob/main/components/matrix_led/README.md)
- [Board LED组件](https://github.com/RMinte-AI/TianshanOS/blob/main/components/board_led)
- [色彩校正组件](https://github.com/RMinte-AI/TianshanOS/blob/main/components/color_correction)

### 特性开发记录
- [AGX监控更新](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/AGX_MONITOR_UPDATES.md)
- [AGX启动延迟功能](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/AGX_STARTUP_DELAY_FEATURE.md)
- [绝对静默模式](https://github.com/RMinte-AI/TianshanOS/blob/main/docs/ABSOLUTE_SILENCE_FINAL.md)

---

## 🎯 项目优势与特色

### 1. 模块化设计
- 每个功能都是独立的组件
- 清晰的依赖关系
- 支持单独开发、测试和维护
- 易于扩展和升级

### 2. 完整的硬件抽象
- 统一的硬件访问接口
- 降低上层组件的硬件依赖
- 便于移植到其他硬件平台

### 3. 强大的LED系统
- 三个独立LED子系统
- 1052颗可编程LED（1+28+1024）
- 丰富的动画效果
- 专业的色彩校正系统

### 4. 智能温度管理
- 多温度源优先级策略
- 分层安全保护机制
- AGX实时温度集成
- 灵活的温度曲线配置

### 5. 完整的网络功能
- W5500硬件以太网
- DHCP服务器和客户端
- 网络活动日志
- 网络诊断工具

### 6. 丰富的命令行接口
- 100+控制台命令
- 命令自动补全
- 历史记录功能
- 完整的帮助系统

### 7. 配置持久化
- 统一的NVS配置管理
- 所有组件配置可保存
- 支持配置导入导出
- 版本控制和迁移

### 8. 实时监控
- AGX系统WebSocket监控
- 电源状态实时监测
- 网络活动日志记录
- 系统运行状态跟踪

---

## 🔄 版本历史

### v1.1.0 (2025年10月6日)
- 完整的模块化架构
- 智能温度管理系统
- 完整的LED控制系统
- 网络管理功能
- 存储管理功能
- 电源监控功能

### v1.0.0
- 基础架构搭建
- 核心组件实现
- 硬件抽象层
- 控制台系统

---

## 📋 重构考虑因素

### 需要保留的核心特性
1. ✅ **模块化架构设计**
2. ✅ **硬件抽象层设计**
3. ✅ **事件驱动通信机制**
4. ✅ **统一配置管理系统**
5. ✅ **完整的命令行接口**
6. ✅ **LED系统的三层架构**
7. ✅ **智能温度管理策略**
8. ✅ **网络管理功能**
9. ✅ **配置持久化机制**
10. ✅ **AGX监控集成**

### 可能需要优化的部分
1. ⚠️ **RMT资源分配**（三个LED子系统可能存在资源竞争）
2. ⚠️ **内存使用优化**（1024颗LED的缓冲区较大）
3. ⚠️ **GPIO引脚冲突管理**
4. ⚠️ **错误处理和恢复机制**
5. ⚠️ **代码重复度**（部分组件有相似功能）
6. ⚠️ **文档完整性**（部分组件文档不完整）
7. ⚠️ **测试覆盖率**（部分组件缺少单元测试）

### 重构目标建议
1. 🎯 **保持核心架构不变**
2. 🎯 **优化资源使用效率**
3. 🎯 **提高代码复用性**
4. 🎯 **完善错误处理机制**
5. 🎯 **增强测试覆盖率**
6. 🎯 **改进文档完整性**
7. 🎯 **优化性能和响应速度**

---

## 📞 联系信息

**项目团队：** robOS Team  
**GitHub仓库：** https://github.com/RMinte-AI/TianshanOS<br>
**最新版本：** v1.1.0  
**更新日期：** 2025年10月5日

---

## 📝 备注

本文档基于 robOS v1.1.0 版本分析整理，旨在为 TianshanOS 重构项目提供详细的原始项目参考资料。文档包含了硬件配置、软件架构、功能特性、命令接口等完整信息，可作为重构规划的基础文档。

---

**文档版本：** 1.0  
**创建日期：** 2026年1月14日  
**创建人：** GitHub Copilot  
**项目代号：** TianshanOS
