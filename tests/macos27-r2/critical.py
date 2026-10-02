#!/usr/bin/env python3
"""把 css/style.css 里 /*CRITICAL*/ … /*END*/ 之间的规则压缩后写进 index.html 的 <style id="critical">。
  python3 tests/macos27-r2/critical.py          # 生成并写回
  python3 tests/macos27-r2/critical.py --check  # 只检查是否已同步（不同步则退出码 1）
压缩用的就是 tools/minify_web.py 的 minify_css，和构建产物里 style.css 走同一条路径。"""
import os, re, sys, importlib.util

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WEB = os.path.join(REPO, 'components/ts_webui/web')
spec = importlib.util.spec_from_file_location('minify_web', os.path.join(REPO, 'tools/minify_web.py'))
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)

css = open(os.path.join(WEB, 'css/style.css'), encoding='utf-8').read()
m = re.search(r'/\*CRITICAL\*/(.*?)/\*END\*/', css, re.S)
if not m:
    sys.exit('style.css 里没有 CRITICAL 区')
crit = mw.minify_css(m.group(1))
html_path = os.path.join(WEB, 'index.html')
html = open(html_path, encoding='utf-8').read()
new = re.sub(r'(<style id="critical">).*?(</style>)', lambda mm: mm.group(1) + crit + mm.group(2), html, count=1, flags=re.S)
if new == html:
    print('index.html 内联关键 CSS 已同步（%d 字节）' % len(crit.encode()))
    sys.exit(0)
if '--check' in sys.argv:
    print('index.html 内联关键 CSS 与 style.css 的 CRITICAL 区不同步，请运行 tests/macos27-r2/critical.py')
    sys.exit(1)
open(html_path, 'w', encoding='utf-8').write(new)
print('已写入 index.html 内联关键 CSS（%d 字节）' % len(crit.encode()))
