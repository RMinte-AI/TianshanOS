# 提交前独立核对

结论：可以推送。最终生产 diff 只改变本地资源来源、缓存标识、vendor 文件和跳过第三方 minify；无 CMake、后端、SSH/J1 状态机或工作流改动。短路径由用户确认，发行文件 SHA256 未变化。

正向核对 loader 同源 URL → vendor → 原字节优化副本 → gzip → SPIFFS → www.bin → flasher_args；反向核对最终镜像与独立重建字节一致、实际优化资源在全新 Chrome context 禁止全部外部 origin 时正常初始化与输入。匹配/非匹配分支分别有 vendor skip/项目资源 minify、首次/重复加载、故障/恢复证据。

本次源与优化 Chrome、Node、J1、v3 跨层、固定 SDK 构建、产物/发布配置检查见 README。核对 build-inputs.json 全部生产哈希一致，当前 diff --check 通过。没有以旧 CI 或旧构建代替本轮验证。

SDK/网络边界是模拟，真实 ESP 挂载、HTTP、浏览器、SSH 和缓存升级待实机验收；有限 SPIFFS 剩余空间不代表未来可无限添加资源。只提交明确文件，原未跟踪 output/、tmp/、runtime-repair 文件保持排除。不合并、不发布、不操作设备。
