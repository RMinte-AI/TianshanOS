> 当前返工候选为 build-v19；有效身份绑定与隔离演练见 [当前回退说明](../../output/macos27-20260929-native-rework/diff-audit-v19/README.md)。下文全部是历史版本，旧补丁不得应用于当前源码。真实工作区未回退。

> 2026-09-29：本文记录的是用户已否决版本的历史检查，不是当前修订版验收。当前实施及未解决项见 [CARD-REVISION.md](CARD-REVISION.md)。

# 精确回退

只撤回本轮三个生产文件的视觉差异。补丁不含后端、分区、API、路由、语言包、设备配置、测试资料或其他用户文件。

基线 HEAD：`ac109b00dbbba5654c611b746b7ef7228b05987c`，分支 `feat/macos27-webui`。

在仓库根目录执行（当前尚未执行实际工作区回退）：

```sh
git apply --check output/macos27-20260928/rollback-visual.patch
git apply output/macos27-20260928/rollback-visual.patch
```

如果 `--check` 失败，说明后续改动与本次范围重叠；停止并逐块处理，不能使用 `reset --hard`、强制覆盖或清理用户目录。需要重新应用本轮视觉时使用同目录 `visual.patch`，也先执行 `git apply --check`。

已验证：当前工作区逆补丁检查通过；在独立临时副本中实际应用逆补丁后，三个文件 SHA-256 均恢复为本轮基线。真实工作区保持新版。证据：`output/macos27-20260928/rollback-proof.json`。

回退源文件后，若需新发布产物，应重新运行原 minify/gzip/SPIFFS 构建；此文不授权部署或刷机。`after/www.bin` 是本轮新版产物，不能作为回退镜像；`baseline/www.bin` 是本轮离线生成的基线对照，也未做设备升级验证。
