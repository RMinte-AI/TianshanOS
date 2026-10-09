# TianShanOS 文档导航

**欢迎来到 TianShanOS 文档中心！**

本目录包含 TianShanOS 的完整技术文档。根据您的需求选择合适的文档：

---

## 📘 用户文档

**适合对象**：系统使用者、运维人员、初次接触者

| 文档 | 描述 |
|------|------|
| [快速入门](QUICK_START.md) | 5 分钟快速上手指南：环境准备、编译烧录、首次配置 |
| [串口刷机指南](FLASH_VIA_SERIAL.md) | 串口烧录、手动分区偏移和常见刷机问题 |
| [Root/Admin 密码恢复固件](ROOT_PASSWORD_RESET_FIRMWARE.md) | root/admin 用户密码恢复固件的构建、OTA 和串口使用流程 |
| [命令参考](COMMAND_REFERENCE.md) | CLI 命令完整手册：系统、网络、LED、设备、配置等 |
| [配置指南](CONFIGURATION_GUIDE.md) | 板级配置、GPIO 映射、运行时配置说明 |

**用户操作类帮助**：WebUI 各页面的详细使用说明见项目根目录下的 `help/` 目录（如 [安全页面操作指南](../help/security-guide.mdx)）。

**推荐阅读顺序**：QUICK_START → CONFIGURATION_GUIDE → COMMAND_REFERENCE

---

## 📗 开发文档

**适合对象**：开发者、贡献者、系统维护者

| 文档 | 描述 |
|------|------|
| [架构设计](ARCHITECTURE_DESIGN.md) | 系统架构、服务管理、事件系统、Core API 设计 |
| [开发进度](DEVELOPMENT_PROGRESS.md) | 各阶段完成情况、代码统计、开发日志（Phase 0-15）|
| [故障排除](TROUBLESHOOTING.md) | 常见问题、调试技巧、已知 Bug 及解决方案 |
| [测试计划](TEST_PLAN.md) | 单元测试、集成测试、性能测试计划 |

**推荐阅读顺序**：ARCHITECTURE_DESIGN → DEVELOPMENT_PROGRESS → TROUBLESHOOTING

---

## 📕 API 参考

**适合对象**：二次开发者、脚本编写者、集成开发者

| 文档 | 描述 |
|------|------|
| [API 参考](API_REFERENCE.md) | REST API、WebSocket API、Core API 完整接口文档 |
| [LED 架构](LED_ARCHITECTURE.md) | LED 子系统设计：动画引擎、特效系统、设备抽象 |

**推荐阅读顺序**：API_REFERENCE → LED_ARCHITECTURE（如需 LED 开发）

---

## 📙 项目文档

**适合对象**：项目管理者、架构师、新团队成员

| 文档 | 描述 |
|------|------|
| [项目概述](PROJECT_DESCRIPTION.md) | 项目背景、目标、核心理念、技术选型 |
| [安全实现](SECURITY_IMPLEMENTATION.md) | 认证机制、SSH 客户端、安全策略 |
| [板级配置](BOARD_CONFIGURATION.md) | 板级特性定义、JSON 配置格式、扩展方法 |
| [GPIO 映射](GPIO_MAPPING.md) | 引脚分配表、配置方法、最佳实践 |

**推荐阅读顺序**：PROJECT_DESCRIPTION → ARCHITECTURE_DESIGN → 其他

---

## 🗂️ 文档分类速查

### 按角色查找

#### 🎯 我是新用户
1. [快速入门](QUICK_START.md) - 从零开始
2. [配置指南](CONFIGURATION_GUIDE.md) - 配置系统
3. [命令参考](COMMAND_REFERENCE.md) - 使用命令

#### 🔧 我是开发者
1. [架构设计](ARCHITECTURE_DESIGN.md) - 理解系统
2. [开发进度](DEVELOPMENT_PROGRESS.md) - 了解历史
3. [API 参考](API_REFERENCE.md) - 集成开发
4. [故障排除](TROUBLESHOOTING.md) - 解决问题

#### 🏗️ 我是贡献者
1. [项目概述](PROJECT_DESCRIPTION.md) - 理解愿景
2. [架构设计](ARCHITECTURE_DESIGN.md) - 掌握架构
3. [开发进度](DEVELOPMENT_PROGRESS.md) - 当前状态
4. [测试计划](TEST_PLAN.md) - 测试规范

#### 🔐 我关注安全
1. [安全实现](SECURITY_IMPLEMENTATION.md) - 安全机制
2. [API 参考](API_REFERENCE.md) - 认证接口

---

## 📊 文档更新日志

| 日期 | 版本 | 主要更新 |
|------|------|---------|
| 2026-01-24 | v2.0 | 文档重组：20 → 13 个文档，优化分类和导航 |
| 2026-01-23 | v1.5 | 新增 Phase 14-15 内容，日志系统和 LED 优化 |
| 2026-01-22 | v1.4 | 新增 OTA 升级文档 |
| 2026-01-21 | v1.3 | 新增 WebUI 增强和 SSH Shell 文档 |
| 2026-01-15 | v1.0 | 初版文档完成（Phase 0-8）|

---

## 🔗 相关资源

### 外部链接
- [GitHub 仓库](https://github.com/RMinte-AI/TianshanOS)
- [ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [ESP32-S3 技术规格](https://www.espressif.com/en/products/socs/esp32-s3)

### 组件文档
- LED 子系统：见 [LED_ARCHITECTURE.md](LED_ARCHITECTURE.md)
- 安全组件：见 [SECURITY_IMPLEMENTATION.md](SECURITY_IMPLEMENTATION.md)
- Core API：见 [ARCHITECTURE_DESIGN.md](ARCHITECTURE_DESIGN.md)

---

## 💡 文档贡献指南

### 文档编写规范
- **中文为主**：面向中文用户，代码注释英文
- **Markdown 格式**：使用标准 Markdown 语法
- **代码示例**：提供可运行的完整示例
- **更新日志**：每次修改记录变更内容

### 文档结构约定
```markdown
# 文档标题

**简介**（1-2 句话说明文档用途）

---

## 章节 1
...

## 章节 2
...

---

## 参考资源
- 相关文档链接
- 外部资源
```

### 需要改进的文档
如果您发现文档有以下问题，欢迎提交 PR：
- ❌ 描述不清晰或过时
- ❌ 缺少代码示例
- ❌ 链接失效
- ❌ 排版错误

---

## 📮 反馈与支持

- **问题反馈**：[GitHub Issues](https://github.com/RMinte-AI/TianshanOS/issues)
- **功能建议**：[GitHub Discussions](https://github.com/RMinte-AI/TianshanOS/discussions)
- **文档问题**：在对应文档的 Issue 中注明 `[docs]`

---

**最后更新**：2026年1月24日  
**文档版本**：v2.0  
**维护者**：TianShanOS 开发团队
