# Root/Admin 密码恢复固件

本版本只适用于存储方案 v2。请先用配套 USB／串口包升级并完成首次配置迁移，再使用此恢复固件。它会核对新系统区 `nvs`（`0x9B0000`，128 KiB）和原密钥区 `keys_nvs`（`0x9000`，48 KiB）；旧布局会拒绝执行。

Root/Admin 密码恢复固件是一个 app-only 临时固件，只修改新系统 NVS 中 `ts_auth/cred_root` 和 `ts_auth/cred_admin`，把 `root` 与 `admin` 密码都恢复为默认值 `rm01`。它不会擦除 NVS，也不会主动修改网络配置、证书、SSH key 或其他数据。

该固件不启动正常系统服务，不启动 WebUI、API、网络或控制台。写入并校验 root/admin 凭据后，它会尝试切回另一个有效 OTA app 分区，等待 3 秒后重启。

恢复固件会先检查 `ts_auth/cfg_version` 是否等于当前认证布局版本。若版本缺失或不匹配，它会停机并拒绝写入任何 credential，避免回到正常固件后触发认证模块的全用户重置流程。

## 构建

```bash
./tools/build_root_reset.sh
```

指定恢复固件基础版本号：

```bash
ROOT_PASSWORD_RESET_VERSION=9.9.9 ./tools/build_root_reset.sh
```

构建产物：

- `build-root-reset/TianShanOS.bin`
- `build-root-reset/TianShanOS-RootReset.bin`

人工刷写和归档时统一使用 `build-root-reset/TianShanOS-RootReset.bin`，避免和普通固件混淆。

## 通过 WebUI 手动 OTA

适用条件：当前设备仍可进入 WebUI，且 OTA 所需认证会话可用。

1. 打开 WebUI 的系统升级页面。
2. 手动上传 `build-root-reset/TianShanOS-RootReset.bin`。
3. 取消勾选“包含 WebUI”，不要上传或刷写 `www.bin`。
4. 开始 OTA，随后通过串口日志确认恢复过程。

如果 OTA 需要认证而现有 session 不可用，不能通过 WebUI 完成，只能走串口流程。

## 通过串口强制启动 ota_0

串口恢复用于 WebUI/OTA 不可用的场景。该流程会清除 OTA 选择信息并把恢复 app 写入 `ota_0`，不会擦 NVS。

这个流程只有在 `ota_1` 保留着要返回的正常固件时，才会自动回到那个正常固件。如果设备原本运行在 `ota_0`，下面的命令会覆盖该正常固件；恢复固件随后会尝试切到 `ota_1`，那里可能是旧固件、测试固件或空分区。返回目标必须是兼容存储方案 v2 的正常固件；旧布局固件即使镜像有效，也不能在新布局上运行。无法确认另一个 slot 内容时，串口流程应视为“先恢复 root/admin，再手动刷回正常固件”的救援手段。

```bash
esptool.py --chip esp32s3 -p <PORT> erase_region 0x16000 0x2000
esptool.py --chip esp32s3 -p <PORT> write_flash 0x20000 build-root-reset/TianShanOS-RootReset.bin
```

`0x16000` 是 `otadata` 分区地址，`0x20000` 是 `ota_0` app 分区地址。恢复固件启动后会查找另一个 OTA slot 作为返回目标；如果另一个 slot 没有有效正常固件，或目标版本也是 `root-reset`，它会停机并要求手动刷回正常固件。

## 成功标准

串口日志应出现以下信息：

- NVS 初始化完成。
- 写入 `ts_auth/cred_root`。
- 写入 `ts_auth/cred_admin`。
- `nvs_commit()` 完成。
- root/admin 读回校验通过。
- 打印当前分区、目标分区、目标 project/version。
- 等待 3 秒后重启。

重启回同一认证布局的正常固件后，`root/rm01` 和 `admin/rm01` 都应可登录；网络配置、证书、SSH key 和其他 NVS 配置应保持不变。重复刷入同一个恢复固件应再次成功恢复 root/admin。
