# TianShanOS OTA 服务器

一个健壮的固件更新服务器，为 TianShanOS 设备提供 OTA 更新服务。

## 功能特性

- ✅ **版本信息管理** - 自动从固件中提取版本、编译时间等信息
- ✅ **固件下载** - 支持 firmware 和 www.bin
- ✅ **断点续传** - 支持 HTTP Range 请求
- ✅ **CORS 支持** - 允许浏览器跨域访问
- ✅ **完整性校验** - 返回 SHA256 哈希值
- ✅ **详细日志** - 彩色日志输出，便于调试
- ✅ **自动检测** - 可自动检测 build 目录中的文件

## 快速开始

### 1. 安装依赖

```bash
cd tools/ota_server
pip3 install -r requirements.txt
```

### 2. 启动服务器

```bash
# 使用启动脚本（推荐，自动检测 build 目录）
./start.sh

# 或直接运行 Python 脚本
python3 ota_server.py --build-dir ../../build

# 指定具体文件
python3 ota_server.py --firmware /path/to/TianShanOS.bin --www /path/to/www.bin

# 自定义端口
python3 ota_server.py --build-dir ../../build --port 8080
```

### 3. 配置 TianShanOS 设备

在 WebUI 的"系统升级"页面，设置 OTA 服务器地址：
```
http://<服务器IP>:57807
```

然后点击"检查更新"按钮。

### 4. 发布前编译（让设备能检测到新固件）

版本号仅在 **CMake 重新配置** 时更新。若只做增量编译（`idf.py build`），固件版本不变，设备上会显示「已是最新版本」。发布 OTA 前请用：

```bash
./tools/build.sh --fresh
```

或 `rm -f build/CMakeCache.txt && idf.py build`，再用 `--build-dir build` 启动 OTA 服务器。

## API 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 服务器信息 |
| `/version` | GET | 固件版本信息（JSON） |
| `/firmware` | GET | 下载固件文件 |
| `/www.bin` | GET / HEAD | 下载 WebUI 文件（设备网页使用此地址，支持 Range） |
| `/www` | GET / HEAD | 同一 WebUI 下载处理器的兼容地址 |
| `/health` | GET | 健康检查 |
| `/reload` | POST | 重新加载固件信息 |

### /version 响应示例

```json
{
    "version": "0.3.0+7fdcca4d.01310354",
    "project_name": "TianShanOS",
    "compile_date": "Jan 31 2026",
    "compile_time": "03:54:41",
    "idf_version": "v5.5.2-519-g0fd9f0aa93",
    "secure_version": 0,
    "size": 2279232,
    "sha256": "abc123...",
    "www_available": true,
    "www_size": 2097152,
    "www_sha256": "def456..."
}
```

## 命令行参数

```
用法: ota_server.py [-h] [-p PORT] [-H HOST] [-b BUILD_DIR] 
                     [-f FIRMWARE] [-w WWW] [-d] [-v]

选项:
  -p, --port PORT       监听端口 (默认: 57807)
  -H, --host HOST       监听地址 (默认: 0.0.0.0)
  -b, --build-dir DIR   ESP-IDF 构建目录 (自动检测固件文件)
  -f, --firmware PATH   固件文件路径
  -w, --www PATH        WebUI 文件路径
  -d, --debug           启用调试模式
  -v, --verbose         详细输出
```

## 工作原理

### 浏览器代理模式

TianShanOS 使用**浏览器代理模式**进行 OTA 升级，流程如下：

```
┌─────────┐     1. GET /version     ┌─────────────┐
│ 浏览器  │ ──────────────────────► │ OTA 服务器  │
│         │ ◄────────────────────── │ (本服务器)  │
│         │     版本信息 (JSON)     │             │
│         │                         │             │
│         │     2. GET /firmware    │             │
│         │ ──────────────────────► │             │
│         │ ◄────────────────────── │             │
│         │     固件数据 (binary)   │             │
│         │                         └─────────────┘
│         │
│         │     3. POST /api/v1/ota/firmware
│         │ ──────────────────────► ┌─────────────┐
│         │ ◄────────────────────── │   ESP32     │
│         │     上传结果            │ TianShanOS  │
└─────────┘                         └─────────────┘
```

**优势**：
- ESP32 设备无需联网，只需与浏览器在同一局域网
- 浏览器处理所有外部网络请求
- 支持 HTTPS OTA 服务器（浏览器处理证书）
- 可视化进度显示

## 生产部署

### 使用 Gunicorn (Linux/macOS)

```bash
pip3 install gunicorn
gunicorn -w 4 -b 0.0.0.0:57807 "ota_server:OTAServer('./build/TianShanOS.bin', './build/www.bin').app"
```

### 使用 Docker

```dockerfile
FROM python:3.11-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY ota_server.py .
COPY firmware/ /firmware/

EXPOSE 57807
CMD ["python", "ota_server.py", "--firmware", "/firmware/TianShanOS.bin", "--www", "/firmware/www.bin"]
```

### 使用 systemd 服务

```ini
# /etc/systemd/system/tianshanos-ota.service
[Unit]
Description=TianShanOS OTA Server
After=network.target

[Service]
Type=simple
User=ota
WorkingDirectory=/opt/ota-server
ExecStart=/usr/bin/python3 ota_server.py --firmware /opt/firmware/TianShanOS.bin --www /opt/firmware/www.bin
Restart=always

[Install]
WantedBy=multi-user.target
```

## 常见问题

### 1. 检查更新失败：CORS 错误

确保服务器配置了 CORS 支持。本服务器已内置 CORS 支持。

### 2. 下载固件失败

检查：
- 固件文件是否存在
- 文件权限是否正确
- 使用 `/health` 端点检查服务器状态

### 3. 版本信息显示为 0.0.0

固件可能不是有效的 ESP-IDF 应用程序，或者 app_desc 结构被损坏。

## 开发

### 离线下载回归

在已安装 requirements.txt 的 Python 环境中，从仓库根目录运行：

```bash
python3 -m unittest discover -s tools/ota_server -p test_ota_server.py
```

测试使用临时文件和 Flask 进程内 test_client，覆盖版本、固件、两个 WebUI 地址、HEAD、Range 和缺失网页包；不监听端口、不向设备上传。

### 测试 API

```bash
# 获取版本信息
curl http://localhost:57807/version

# 健康检查
curl http://localhost:57807/health

# 下载固件（保存到文件）
curl -o firmware.bin http://localhost:57807/firmware

# 下载网页包（旧地址 /www 仍可用）
curl -o www.bin http://localhost:57807/www.bin

# 断点续传测试
curl -H "Range: bytes=0-1023" http://localhost:57807/firmware -o first_1kb.bin
```

### 日志级别

使用 `-d` 或 `-v` 参数启用详细日志。
