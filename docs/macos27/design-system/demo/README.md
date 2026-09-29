# 天山OS 界面演示包

纯静态网页，用来在官网里展示天山OS 的界面。共 7 个页面，顶部菜单可以互相跳转。没有后端，页面里的数据全是示例数据，除菜单跳转外的按钮都不会有动作。

## 文件

- `index.html`：入口，自动跳到 `system.html`。
- `system.html` 系统、`network.html` 网络、`files.html` 文件、`terminal.html` 终端、`automation.html` 自动化、`commands.html` 指令、`security.html` 安全。
- `assets/spec.css`、`assets/quantify.css`：样式和字体，所有页面共用。

所有引用都是相对路径，整个文件夹放到网站任意子路径都能用，例如 `https://www.rminte.com/tianshanos-demo/`。页面不请求任何外部资源。

## 嵌入方式

推荐用 iframe，官网点“天山OS”后打开一个独立的演示页，或直接跳到 `/tianshanos-demo/`：

```html
<iframe id="ts-demo" src="/tianshanos-demo/index.html"
        style="width:100%;border:0;height:900px" title="天山OS 演示"></iframe>
<script>
  // 页面高度随宽度变化，演示页会把自己的高度发给父页面，用来自动撑开 iframe
  addEventListener('message', function (e) {
    if (e.data && e.data.type === 'tianshanos-demo-height')
      document.getElementById('ts-demo').style.height = e.data.height + 'px';
  });
</script>
```

- 页面按 1440px 宽的设计整体缩放到容器宽度，文字和图形是矢量，缩放后仍然清晰。
- 桌面和平板宽度（约 900px 以上）显示正常；手机上会被缩得很小，建议手机端引导用户用电脑查看，或只放一张截图。
- 如果官网设置了 CSP 或 `X-Frame-Options`，需要允许同源 iframe。
- 想改页面内容，只需要改对应的 `.html` 文件，样式在 `assets/spec.css`。

## 内容说明

页面里的 IP、密钥、规则名称等都是虚构的示例值（例如 192.0.2.x），不对应真实设备。
