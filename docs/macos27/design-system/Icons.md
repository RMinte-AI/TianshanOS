# 图标清单（remixicon → 内联 SVG 精灵）

来源：web/index.html + web/js/*.js 静态扫描。实际用到 **107** 个图标（已排除 `ri-xxx`、`ri-arrow-`、`ri-text` 三个非图标字符串）。
运行时拼接：`ri-${isReading ? 'stop' : 'play'}-line`（app.js 3386、3777），即 `ri-stop-line` / `ri-play-line`，已计入。

## 现状问题
- 字体文件 `remixicon.woff2` 189,216 B 是完整字体，`remixicon.css` 只声明了 114 个类；实际只用 107 个。
- **9 个图标在 CSS 里没有对应类，按静态扫描推断现在不显示（未在浏览器实测）**：`ri-archive-line`, `ri-arrow-down-line`, `ri-arrow-up-line`, `ri-file-list-3-line`, `ri-inbox-line`, `ri-loader-4-line`, `ri-router-line`, `ri-tornado-line`, `ri-wifi-line`。改成精灵时需要顺带补上（这是现有缺陷，不是新问题）。
- 3 处状态点用 `ri-checkbox-blank-circle-fill`（15 次）、`ri-record-circle-fill`、`ri-stop-fill`：设计稿里状态点是纯 CSS 圆点，不需要图标。

## 替换方案
- 一个 `<svg style="display:none">` 精灵放进 index.html，`<symbol id="i-close">`…，使用处 `<svg class="i"><use href="#i-close"/></svg>`。
- 风格与设计稿一致：16×16 viewBox、描边 1.35、圆角端点，继承 `currentColor`。
- 体积【估算，未实测】：每个图标约 150–300 B，合计约 20–30 KB 未压缩；删除 woff2 + remixicon.css 后净减约 160–170 KB。SPIFFS 与 HTTP 请求数各少 2 个（字体 + css）。
- `<i class="ri-…">` 共出现约 560 处，动态拼接的 2 处要单独改。

## 清单（按使用次数）

| 图标 | 次数 |
|---|---|
| ri-close-line | 42 |
| ri-refresh-line | 32 |
| ri-file-text-line | 28 |
| ri-download-line | 24 |
| ri-delete-bin-line | 22 |
| ri-information-line | 18 |
| ri-rocket-line | 18 |
| ri-checkbox-blank-circle-fill | 15 |
| ri-thunderstorms-line | 15 |
| ri-upload-line | 15 |
| ri-bar-chart-line | 13 |
| ri-check-line | 13 |
| ri-add-line | 12 |
| ri-folder-open-line | 11 |
| ri-lightbulb-line | 11 |
| ri-alert-line | 10 |
| ri-eye-line | 9 |
| ri-file-list-line | 9 |
| ri-play-line | 9 |
| ri-key-line | 8 |
| ri-save-line | 7 |
| ri-search-line | 7 |
| ri-error-warning-line | 6 |
| ri-folder-line | 6 |
| ri-global-line | 6 |
| ri-stop-line | 6 |
| ri-temp-hot-line | 6 |
| ri-computer-line | 5 |
| ri-dashboard-line | 5 |
| ri-lock-line | 5 |
| ri-question-line | 5 |
| ri-record-circle-fill | 5 |
| ri-stop-circle-line | 5 |
| ri-box-3-line | 4 |
| ri-edit-line | 4 |
| ri-hourglass-line | 4 |
| ri-inbox-line ⚠️无CSS | 4 |
| ri-numbers-line | 4 |
| ri-settings-line | 4 |
| ri-smartphone-line | 4 |
| ri-stop-fill | 4 |
| ri-tools-line | 4 |
| ri-apps-line | 3 |
| ri-archive-line ⚠️无CSS | 3 |
| ri-arrow-up-s-line | 3 |
| ri-dashboard-3-line | 3 |
| ri-database-2-line | 3 |
| ri-focus-line | 3 |
| ri-line-chart-line | 3 |
| ri-movie-line | 3 |
| ri-plug-line | 3 |
| ri-server-line | 3 |
| ri-settings-3-line | 3 |
| ri-shield-keyhole-line | 3 |
| ri-signal-wifi-3-line | 3 |
| ri-sun-line | 3 |
| ri-time-line | 3 |
| ri-timer-line | 3 |
| ri-arrow | 2 |
| ri-bar-chart-box-line | 2 |
| ri-broadcast-line | 2 |
| ri-contrast-line | 2 |
| ri-emotion-line | 2 |
| ri-image-line | 2 |
| ri-lightbulb-fill | 2 |
| ri-loader-4-line ⚠️无CSS | 2 |
| ri-lock-unlock-line | 2 |
| ri-music-line | 2 |
| ri-notification-line | 2 |
| ri-play-circle-line | 2 |
| ri-run-line | 2 |
| ri-scan-line | 2 |
| ri-service-line | 2 |
| ri-shield-line | 2 |
| ri-terminal-box-line | 2 |
| ri-toggle-line | 2 |
| ri-user-line | 2 |
| ri-arrow-down-line ⚠️无CSS | 1 |
| ri-arrow-right-line | 1 |
| ri-arrow-up-line ⚠️无CSS | 1 |
| ri-brain-line | 1 |
| ri-color-filter-line | 1 |
| ri-cpu-line | 1 |
| ri-door-open-line | 1 |
| ri-download-cloud-line | 1 |
| ri-eject-line | 1 |
| ri-file-list-3-line ⚠️无CSS | 1 |
| ri-folder-add-line | 1 |
| ri-gamepad-line | 1 |
| ri-hard-drive-line | 1 |
| ri-home-line | 1 |
| ri-lightbulb-flash-line | 1 |
| ri-link-unlink | 1 |
| ri-network-line | 1 |
| ri-palette-line | 1 |
| ri-pause-line | 1 |
| ri-percent-line | 1 |
| ri-progress-6-line | 1 |
| ri-qr-code-line | 1 |
| ri-restart-line | 1 |
| ri-router-line ⚠️无CSS | 1 |
| ri-ruler-line | 1 |
| ri-shut-down-line | 1 |
| ri-toggle-fill | 1 |
| ri-tornado-line ⚠️无CSS | 1 |
| ri-usb-line | 1 |
| ri-wifi-line ⚠️无CSS | 1 |
