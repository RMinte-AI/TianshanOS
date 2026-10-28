# TianShanOS 板级配置指南

## 概述

TianShanOS 使用 JSON 配置文件定义板级硬件配置，实现"配置驱动"的设计理念。

## 配置存储

系统设置保存到内部的 128 KiB 系统 NVS。网络、风扇、温度、电压保护、LED、UI 等设置保存成功后，无卡启动也能读取。出厂默认值只在尚未配置时使用。

SD 卡上的旧系统配置仅用于首次迁移，之后不会因插卡或重启覆盖已保存的系统设置。配置文件仍可通过明确的导入、导出操作使用。

自动化使用另一套明确的存储规则：平时保存到 SD 卡，点击“保存应急副本”才更新内部 2 MiB 配置区；无卡时可直接编辑内部配置。详见 [自动化配置使用与存储](AUTOMATION_STORAGE.md)。密钥保留在独立的原密钥区。

### 配置模块分类

**带字段定义的系统配置模块**（8个）：
- `net.json`, `dhcp.json`, `wifi.json`, `nat.json`
- `led.json`, `fan.json`, `device.json`, `system.json`
- 使用 `ts_config_module_persist()` 保存到系统 NVS；文件名是显式导入、导出的格式约定。

**由各模块管理的系统设置**：温度来源、电压保护、LED 色彩校正、UI 卡片布局等也保存在系统 NVS。它们不会自动与 SD 卡来回同步。

**自动化配置集合**：数据源、规则、动作模板、SSH 命令、SSH 主机以及需要保存的用户变量，由同一个配置保存入口管理，退出旧的分列表 NVS 保存方式。

### 电压保护配置示例

可导入的 `power_policy.json`：

```json
{
  "low_voltage_threshold": 12.6,
  "recovery_voltage_threshold": 18.0,
  "shutdown_delay_sec": 60,
  "recovery_hold_sec": 5,
  "protection_enabled": true
}
```

## 配置文件结构

每个板级配置目录包含三个文件：

```
boards/<board_name>/
├── board.json      # 板级特性
├── pins.json       # 引脚映射
└── services.json   # 服务配置
```

## board.json

定义板级硬件特性和能力。

```json
{
  "name": "rm01_esp32s3",
  "description": "RoboMaster 01 Carrier Board with ESP32S3",
  "chip": "esp32s3",
  "features": {
    "ethernet": true,
    "wifi": true,
    "bluetooth": true,
    "usb_otg": true,
    "sd_card": true,
    "psram": true,
    "led_touch": true,
    "led_board": true,
    "led_matrix": false,
    "fan_count": 4,
    "usb_mux_count": 2,
    "power_monitor": true
  },
  "memory": {
    "flash_size": "16MB",
    "psram_size": "8MB"
  },
  "network": {
    "ethernet_chip": "W5500",
    "ethernet_spi_host": 2
  }
}
```

### 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| name | string | 板级唯一标识符 |
| description | string | 人类可读描述 |
| chip | string | 目标芯片 (esp32s3, esp32p4) |
| features | object | 硬件特性开关 |
| memory | object | 内存配置 |
| network | object | 网络配置 |

## pins.json

定义所有引脚的功能映射。

```json
{
  "pins": {
    "LED_TOUCH": {
      "gpio": 45,
      "function": "led_data",
      "description": "Touch LED WS2812 data"
    },
    "LED_BOARD": {
      "gpio": 42,
      "function": "led_data",
      "description": "Board LED WS2812 data"
    },
    "FAN_1_PWM": {
      "gpio": 4,
      "function": "pwm_output",
      "description": "Fan 1 PWM control"
    },
    "FAN_1_TACH": {
      "gpio": 5,
      "function": "gpio_input",
      "description": "Fan 1 tachometer"
    },
    "ETH_MOSI": {
      "gpio": 11,
      "function": "spi_mosi",
      "spi_host": 2
    },
    "ETH_MISO": {
      "gpio": 13,
      "function": "spi_miso",
      "spi_host": 2
    },
    "ETH_SCLK": {
      "gpio": 12,
      "function": "spi_sclk",
      "spi_host": 2
    },
    "ETH_CS": {
      "gpio": 10,
      "function": "spi_cs"
    },
    "ETH_INT": {
      "gpio": 14,
      "function": "gpio_input",
      "interrupt": true
    },
    "ETH_RST": {
      "gpio": 9,
      "function": "gpio_output"
    },
    "AGX_POWER_EN": {
      "gpio": 1,
      "function": "gpio_output",
      "description": "AGX power enable"
    },
    "AGX_RESET": {
      "gpio": 2,
      "function": "gpio_output",
      "description": "AGX reset"
    },
    "AGX_POWER_GOOD": {
      "gpio": 3,
      "function": "gpio_input",
      "description": "AGX power good"
    },
    "USB_MUX_1_SEL": {
      "gpio": 39,
      "function": "gpio_output"
    },
    "USB_MUX_1_OE": {
      "gpio": 40,
      "function": "gpio_output"
    },
    "SD_CMD": {
      "gpio": 35,
      "function": "sdmmc_cmd"
    },
    "SD_CLK": {
      "gpio": 36,
      "function": "sdmmc_clk"
    },
    "SD_D0": {
      "gpio": 37,
      "function": "sdmmc_d0"
    },
    "I2C_SDA": {
      "gpio": 8,
      "function": "i2c_sda",
      "i2c_port": 0
    },
    "I2C_SCL": {
      "gpio": 18,
      "function": "i2c_scl",
      "i2c_port": 0
    }
  }
}
```

### 引脚功能类型

| 功能 | 说明 |
|------|------|
| gpio_input | GPIO 输入 |
| gpio_output | GPIO 输出 |
| led_data | WS2812 LED 数据线 |
| pwm_output | PWM 输出 |
| spi_mosi | SPI MOSI |
| spi_miso | SPI MISO |
| spi_sclk | SPI 时钟 |
| spi_cs | SPI 片选 |
| i2c_sda | I2C 数据 |
| i2c_scl | I2C 时钟 |
| uart_tx | UART 发送 |
| uart_rx | UART 接收 |
| adc_input | ADC 输入 |
| sdmmc_cmd | SD 命令 |
| sdmmc_clk | SD 时钟 |
| sdmmc_d0-d3 | SD 数据线 |

## services.json

定义系统服务及其配置。

```json
{
  "services": {
    "led_touch": {
      "enabled": true,
      "auto_start": true,
      "priority": 5,
      "depends": ["hal"],
      "config": {
        "led_count": 1,
        "brightness": 80,
        "default_effect": "rainbow"
      }
    },
    "led_board": {
      "enabled": true,
      "auto_start": true,
      "priority": 5,
      "depends": ["hal"],
      "config": {
        "led_count": 28,
        "brightness": 60
      }
    },
    "fan_control": {
      "enabled": true,
      "auto_start": true,
      "priority": 3,
      "depends": ["hal"],
      "config": {
        "update_interval_ms": 1000,
        "default_mode": "auto"
      }
    },
    "ethernet": {
      "enabled": true,
      "auto_start": true,
      "priority": 2,
      "depends": ["hal"],
      "config": {
        "dhcp": true,
        "hostname": "tianshanOS"
      }
    },
    "wifi_ap": {
      "enabled": true,
      "auto_start": false,
      "priority": 2,
      "depends": ["hal"],
      "config": {
        "ssid": "TianShanOS",
        "password": "tianshan123",
        "channel": 1,
        "max_connections": 4
      }
    },
    "webui": {
      "enabled": true,
      "auto_start": true,
      "priority": 6,
      "depends": ["ethernet", "storage"],
      "config": {
        "port": 80,
        "auth_required": true
      }
    },
    "console": {
      "enabled": true,
      "auto_start": true,
      "priority": 1,
      "config": {
        "uart_num": 0,
        "baud_rate": 115200
      }
    },
    "storage": {
      "enabled": true,
      "auto_start": true,
      "priority": 1,
      "config": {
        "spiffs_mount": "/data",
        "sd_mount": "/sd"
      }
    },
    "agx_control": {
      "enabled": true,
      "auto_start": true,
      "priority": 4,
      "depends": ["hal"],
      "config": {
        "power_on_delay_ms": 100,
        "shutdown_timeout_ms": 30000
      }
    }
  }
}
```

### 服务字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| enabled | bool | 是否启用此服务 |
| auto_start | bool | 是否自动启动 |
| priority | int | 启动优先级 (1-8) |
| depends | array | 依赖的其他服务 |
| config | object | 服务特定配置 |

## 添加新板级

1. 创建目录:
```bash
mkdir boards/my_board
```

2. 复制模板:
```bash
cp boards/rm01_esp32s3/*.json boards/my_board/
```

3. 修改配置以匹配硬件

4. 在 `sdkconfig.defaults` 中指定板级:
```
CONFIG_TS_BOARD_NAME="my_board"
```

## 运行时覆盖

引脚配置可以在运行时通过 NVS 覆盖:

```c
// 代码中设置覆盖
ts_pin_set_override("LED_TOUCH", 48);

// 通过命令行
config set pin.LED_TOUCH 48
```

覆盖优先级：NVS > pins.json > 编译时默认值

## 验证配置

使用以下命令验证配置:

```bash
# 检查 JSON 语法
python3 -m json.tool boards/my_board/board.json

# 运行配置验证脚本
python3 scripts/validate_board.py boards/my_board
```

## 最佳实践

1. **引脚冲突检测**: 确保同一 GPIO 不被多个功能使用
2. **保留引脚**: 避免使用 strapping pins (GPIO 0, 3, 45, 46)
3. **文档化**: 为每个引脚添加描述
4. **版本控制**: 将配置文件纳入版本控制
5. **测试**: 在实际硬件上验证配置


---
# GPIO 引脚映射

以下内容来自原 GPIO_MAPPING.md，提供完整的引脚配置参考。

---


> **目标板**: RM01_ESP32S3  
> **芯片**: ESP32-S3  
> **最后更新**: 2026-01-19  
> **验证状态**: ✅ 已与 PCB 图纸及 robOS 代码核对一致

---

## 引脚总览

| GPIO | 功能 | 方向 | 电平逻辑 | 初始状态 | 备注 |
|------|------|------|----------|----------|------|
| 0 | (保留) | - | - | - | Strapping pin |
| 1 | AGX_RESET | OUT | HIGH=复位 | LOW | 脉冲 1000ms |
| 2 | LPMU_RESET | OUT | HIGH=复位 | LOW | 脉冲 300ms |
| 3 | AGX_FORCE_SHUTDOWN | OUT | LOW=开机, HIGH=关机 | LOW | Strapping pin |
| 4 | SD_D0 | IO | - | - | SDMMC |
| 5 | SD_D1 | IO | - | - | SDMMC |
| 6 | SD_D2 | IO | - | - | SDMMC |
| 7 | SD_D3 | IO | - | - | SDMMC |
| 8 | USB_MUX_0 | OUT | - | - | ESP32_MUX0_SEL |
| 9 | LED_MATRIX | OUT | WS2812 | - | 32x32 (1024颗) |
| 10 | ETH_CS | OUT | LOW=选中 | HIGH | W5500 SPI |
| 11 | ETH_MOSI | OUT | - | - | W5500 SPI |
| 12 | ETH_SCLK | OUT | - | - | W5500 SPI |
| 13 | ETH_MISO | IN | - | - | W5500 SPI |
| 14 | (空闲) | - | - | - | - |
| 15 | SD_CMD | IO | - | - | SDMMC |
| 16 | SD_CLK | OUT | - | - | 40MHz |
| 17 | RTL8367_RST | OUT | HIGH=复位 | LOW | 网络交换机 |
| 18 | POWER_ADC | IN | ADC2_CH7 | - | 分压 11.4:1 |
| 19-37 | (空闲/内部) | - | - | - | - |
| 38 | ETH_INT | IN | - | - | W5500 中断 |
| 39 | ETH_RST | OUT | LOW=复位 | HIGH | W5500 复位 |
| 40 | AGX_RECOVERY | OUT | HIGH=恢复模式 | LOW | - |
| 41 | FAN_PWM_0 | OUT | PWM | - | 25kHz, 唯一风扇 |
| 42 | LED_BOARD | OUT | WS2812 | - | 28颗状态灯 |
| 43 | UART0_TX | OUT | - | - | USB 串口 |
| 44 | UART0_RX | IN | - | - | USB 串口 |
| 45 | LED_TOUCH | OUT | WS2812 | - | 1颗, Strapping |
| 46 | LPMU_POWER | OUT | HIGH=电源键 | LOW | 脉冲 300ms, Strapping |
| 47 | POWER_UART_RX | IN | UART1_RX | - | 9600 8N1 |
| 48 | USB_MUX_1 | OUT | - | - | ESP32_MUX1_SEL |

---

## 功能分组

### 1. 设备控制 (AGX/LPMU)

| GPIO | 功能 | 电平逻辑 | 时序 |
|------|------|----------|------|
| 1 | AGX_RESET | HIGH=断电/复位, LOW=正常 | 脉冲或持续 |
| 2 | LPMU_RESET | HIGH=复位, LOW=正常 | 脉冲 300ms |
| 3 | AGX_FORCE_SHUTDOWN | LOW=开机, HIGH=关机 | 持续 |
| 40 | AGX_RECOVERY | HIGH=恢复模式, LOW=正常 | 持续 HIGH |
| 46 | LPMU_POWER | HIGH=按下电源键 | 脉冲 300ms |

#### ⚠️ AGX 模组差异（重要）

不同 NVIDIA 模组的电源控制行为存在差异：

| 模组 | GPIO3 (FORCE_SHUTDOWN) | GPIO1 (RESET) | 电源控制方式 |
|------|------------------------|---------------|--------------|
| **T234 (AGX Orin)** | ✅ 有效：HIGH=关机, LOW=开机 | 脉冲复位 | GPIO3 控制开关机 |
| **T5000/T4000 (NX Orin)** | ⚠️ 无效：必须保持 LOW | **持续 HIGH=断电** | GPIO1 控制供电 |

**T234 模组（AGX Orin 系列）**:
- GPIO3 支持软件电源控制（HIGH=强制关机）
- GPIO1 用于复位（脉冲 HIGH 后恢复 LOW）

**T5000/T4000 模组（NX Orin 系列）**:
- GPIO3 必须**始终保持 LOW**，否则无法开机
- GPIO1 **持续 HIGH = 断电**（实现关机效果）
- 通过 GPIO1 持续拉高来切断模组供电

**操作示例**:
```c
// ========== T234 模组 (AGX Orin) ==========

// AGX 软件关机（GPIO3 控制）
gpio_set_level(3, 1);  // HIGH = force off
// 等待关机完成...
gpio_set_level(3, 0);  // LOW = allow boot (下次开机)

// AGX 复位（脉冲）
gpio_set_level(1, 1);  // HIGH = reset
vTaskDelay(pdMS_TO_TICKS(1000));
gpio_set_level(1, 0);  // LOW = normal

// ========== T5000/T4000 模组 (NX Orin) ==========

// ⚠️ GPIO3 不要动！必须保持 LOW
// gpio_set_level(3, 0);  // 始终保持 LOW

// NX 断电关机（GPIO1 持续拉高）
gpio_set_level(1, 1);  // HIGH = 切断供电（关机）
// 保持 HIGH 状态...

// NX 上电开机（GPIO1 拉低）
gpio_set_level(1, 0);  // LOW = 恢复供电（开机）
```

> **注意**: 当前代码默认使用 T234 逻辑。对于 T5000/T4000 模组，后续将通过配置文件或 menuconfig 选择模组类型。

### 2. SD 卡 (SDMMC 4-bit)

| GPIO | 功能 | 说明 |
|------|------|------|
| 4 | SD_D0 | 数据线 0 |
| 5 | SD_D1 | 数据线 1 |
| 6 | SD_D2 | 数据线 2 |
| 7 | SD_D3 | 数据线 3 |
| 15 | SD_CMD | 命令线 |
| 16 | SD_CLK | 时钟 (40MHz) |

**配置** (`sdkconfig.defaults`):
```
CONFIG_TS_STORAGE_SD_CMD_GPIO=15
CONFIG_TS_STORAGE_SD_CLK_GPIO=16
CONFIG_TS_STORAGE_SD_D0_GPIO=4
CONFIG_TS_STORAGE_SD_D1_GPIO=5
CONFIG_TS_STORAGE_SD_D2_GPIO=6
CONFIG_TS_STORAGE_SD_D3_GPIO=7
```

### 3. USB MUX

| GPIO | 功能 | 说明 |
|------|------|------|
| 8 | USB_MUX_0 | ESP32_MUX0_SEL |
| 48 | USB_MUX_1 | ESP32_MUX1_SEL |

### 4. LED 系统 (WS2812)

| GPIO | 功能 | 数量 | 用途 |
|------|------|------|------|
| 9 | LED_MATRIX | 1024颗 (32x32) | 主显示屏 |
| 42 | LED_BOARD | 28颗 | 状态指示灯条 |
| 45 | LED_TOUCH | 1颗 | 触摸指示灯 |

**总计**: 1053 颗 WS2812 LED

### 5. W5500 以太网 (SPI2)

| GPIO | 功能 | 方向 | 说明 |
|------|------|------|------|
| 10 | ETH_CS | OUT | 片选 (LOW=选中) |
| 11 | ETH_MOSI | OUT | 主出从入 |
| 12 | ETH_SCLK | OUT | 时钟 |
| 13 | ETH_MISO | IN | 主入从出 |
| 38 | ETH_INT | IN | 中断信号 |
| 39 | ETH_RST | OUT | 复位 (LOW=复位, HIGH=正常) |

**SPI 配置**: SPI2_HOST, 25MHz

### 6. 网络交换机 (RTL8367)

| GPIO | 功能 | 电平逻辑 |
|------|------|----------|
| 17 | RTL8367_RST | HIGH=复位, LOW=正常 |

### 7. 风扇控制

| GPIO | 功能 | 参数 |
|------|------|------|
| 41 | FAN_PWM_0 | 25kHz, 10-bit 分辨率 |

**注意**: 板上只有一个风扇，无 FAN_PWM_1

### 8. 电源监控

| GPIO | 功能 | 协议/参数 |
|------|------|-----------|
| 18 | POWER_ADC | ADC2_CH7, 分压比 11.4:1, 最高 72V |
| 47 | POWER_UART_RX | UART1_RX, 9600 8N1 |

**电源芯片数据格式**:
```
[0xFF][电压][电流][CRC]  (4字节)
```

---

## Strapping Pins 注意事项

以下 GPIO 在启动时有特殊用途，需注意初始状态：

| GPIO | 功能 | 启动影响 | 使用建议 |
|------|------|----------|----------|
| 0 | Boot mode | 下拉=下载模式 | 避免使用 |
| 3 | AGX_FORCE_SHUTDOWN | - | 初始 LOW |
| 45 | LED_TOUCH | VDD_SPI | 初始不驱动 |
| 46 | LPMU_POWER | Boot mode | 初始 LOW |

---

## 相关文件

| 文件 | 用途 |
|------|------|
| `boards/rm01_esp32s3/pins.json` | 引脚定义 (JSON) |
| `boards/rm01_esp32s3/devices.json` | 设备配置 |
| `components/ts_hal/include/ts_pin_manager.h` | 引脚函数枚举 |
| `components/ts_hal/src/ts_pin_manager.c` | 默认映射代码 |
| `sdkconfig.defaults` | SD 卡 GPIO 配置 |

---

## 变更历史

| 日期 | 变更内容 |
|------|----------|
| 2026-01-20 | 添加 T234 vs T5000/T4000 模组电源控制差异说明 |
| 2026-01-19 | 初始版本，与 PCB 图纸及 robOS 核对完成 |
| 2026-01-19 | 修正 SD 卡引脚 (4,5,6,7,15,16) |
| 2026-01-19 | 移除不存在的 FAN_PWM_1 (GPIO 40) |
| 2026-01-19 | 添加 RTL8367_RST (GPIO 17) |
| 2026-01-19 | 添加 POWER_ADC (GPIO 18) 和 POWER_UART_RX (GPIO 47) |
