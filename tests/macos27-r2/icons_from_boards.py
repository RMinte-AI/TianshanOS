#!/usr/bin/env python3
"""【已被 icons_from_phosphor.py 取代，仅作历史记录】把 index.html 内联精灵里「稿里画过」的图标换成稿的原始图形。

稿（docs/macos27/design-system/boards/*.html）里的图标是内联 <svg class="i" viewBox="0 0 16 16">：16×16、描边 1.35、圆角端点，
样式由 .i（fill:none;stroke:currentColor;stroke-width:1.35;linecap/linejoin:round）提供。共 41 种图形。
这里把产品里语义相同的图标名（ri-*，id 不变）的 <symbol> 换成稿的图形；稿里没画的 35 个（上/下箭头、搜索、问号、感叹号圆，规则/组件/配置模块/LED 卡用的 28 个装饰图标，以及横向条形图、带行的文档 2 个）按同一风格补画。
82 个图标至此全部是稿的风格，没有 Remix 残留。

  python3 tests/macos27-r2/icons_from_boards.py           # 改写 components/ts_webui/web/index.html
  python3 tests/macos27-r2/icons_from_boards.py --check   # 只检查，不改文件（已是目标状态则退出码 0）

幂等：重复运行结果相同。稿的图形取自稿 HTML 原文（不手抄），用「名字 → 图形里一段独有的路径文字」定位，找不到或不唯一直接报错。
"""
import glob, math, os, re, sys

# 2026-10-03 起图标换成 Phosphor Regular，生成器是 icons_from_phosphor.py。本脚本只作第一轮（稿的原图形）的历史记录；
# 运行它会把精灵改回旧图形，所以默认拒绝执行，确需运行请加 --legacy。
if '--legacy' not in sys.argv:
    sys.exit('icons_from_boards.py 已被 icons_from_phosphor.py 取代（图标现为 Phosphor Regular）；运行它会把精灵改回旧图形。确需运行请加 --legacy。')
sys.argv = [a for a in sys.argv if a != '--legacy']

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BOARDS = os.path.join(REPO, 'docs/macos27/design-system/boards')
INDEX = os.path.join(REPO, 'components/ts_webui/web/index.html')

# 稿里的图形（语义名 → 图形里一段独有的文字；整段取自稿）
KEY = {
    'refresh': 'M13 8a5 5 0 1 1-1.6-3.7', 'close': 'M4 4l8 8M12 4l-8 8', 'trash': 'M3.5 4.5h9M6.5 4.5V3h3v1.5',
    'download': 'M8 2.5v8M5 7.5l3 3 3-3M3 12.5h10', 'play': 'M5.5 3.5v9l7-4.5-7-4.5Z', 'upload': 'M8 10.5V2.5M5 5.5l3-3 3 3',
    'plus': 'M8 3v10M3 8h10', 'power': 'M8 2.5v5M4.8 4.6a5 5 0 1 0 6.4 0', 'info': 'M8 7.2V11M8 5v.1',
    'eye': 'M1.5 8S4 3.5 8 3.5', 'edit': 'M9.5 3.5l3 3M3 13l.7-3', 'stop': '<rect x="4" y="4" width="8" height="8" rx="1.5"/>',
    'file': 'M4 2.5h5l3 3v8H4v-11Z', 'sun': 'M8 2v1.5M8 12.5V14', 'bulb': 'M6 11.5h4M6.5 13.5h3',
    'save': 'M3.5 3.5h7l2 2v7h-9v-9Z', 'globe': 'M2.5 8h11M8 2.5c-2 2.2-2 8.8 0 11', 'key': 'cx="5.5" cy="10.5" r="2.5"',
    'clock': 'M8 4.8V8l2.2 1.4', 'apps': '<rect x="3" y="3" width="4" height="4" rx="1"/>', 'bars': 'M2.5 13.5h11M4.5 11V8',
    'copy': '<rect x="5.5" y="5.5" width="8" height="8" rx="1.5"/>', 'warn': 'M8 2.5 14 13H2L8 2.5Z',
    'home': 'M2.5 8 8 3l5.5 5M4 7v6h8V7', 'folder': '<path d="M2.5 12.5v-8h4l1.5 1.5h5.5v6.5h-11Z"/>',
    'contrast': 'M8 2.5v11', 'qr': '<rect x="2.5" y="2.5" width="4" height="4" rx="1"/>', 'text': 'M3.5 4h9M8 4v8.5',
    'filter': '<circle cx="6" cy="6" r="3.5"/>', 'shield': 'M8 2.5 13 4v4c0 3-2.2 4.8-5 5.5', 'pause': 'M6 3.5v9M10 3.5v9',
    'server': '<rect x="2.5" y="3" width="11" height="4" rx="1.5"/>', 'folderplus': 'M2.5 12.5v-8h4l1.5 1.5h5.5v6.5h-11ZM8 8v3M6.5 9.5h3',
    'eject': 'M3.5 9.5 8 4l4.5 5.5h-9Z', 'network': '<rect x="6" y="2.5" width="4" height="3" rx=".8"/>',
    'wifi': 'M2 6.5a8.5 8.5 0 0 1 12 0', 'broadcast': '<circle cx="8" cy="7" r="1.2"/>', 'box': 'M8 2.5 13.5 5v6L8 13.5 2.5 11V5L8 2.5Z',
    'lock': '<rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/>', 'check': 'M3.5 8.5l3 3 6-7',
}

# 产品图标名 → 稿图形。前 3 组来自「同位置对上」的实测（iconmap：稿与页面/弹窗里相同位置的一对），其余按语义相同归并。
MAP = {
    'refresh': ['ri-refresh-line', 'ri-restart-line', 'ri-scan-line'], 'close': ['ri-close-line'], 'trash': ['ri-delete-bin-line'],
    'download': ['ri-download-line'], 'play': ['ri-play-line'], 'upload': ['ri-upload-line'], 'plus': ['ri-add-line'],
    'power': ['ri-shut-down-line'], 'info': ['ri-information-line'], 'eye': ['ri-eye-line'], 'edit': ['ri-edit-line'],
    'stop': ['ri-stop-line', 'ri-stop-circle-line', 'ri-stop-fill'], 'file': ['ri-file-text-line'],
    'sun': ['ri-sun-line'], 'bulb': ['ri-lightbulb-line'], 'save': ['ri-save-line'], 'globe': ['ri-global-line'], 'key': ['ri-key-line'],
    'clock': ['ri-time-line'], 'apps': ['ri-apps-line'], 'bars': ['ri-line-chart-line'], 'copy': ['ri-file-copy-line'],
    'warn': ['ri-alert-line'], 'home': ['ri-home-line'], 'folder': ['ri-folder-line', 'ri-folder-open-line'], 'contrast': ['ri-contrast-line'],
    'qr': ['ri-qr-code-line'], 'text': ['ri-text'], 'filter': ['ri-color-filter-line'], 'shield': ['ri-shield-line', 'ri-shield-keyhole-line'],
    'pause': ['ri-pause-line'], 'server': ['ri-server-line'], 'folderplus': ['ri-folder-add-line'], 'eject': ['ri-eject-line'],
    'network': ['ri-network-line'], 'wifi': ['ri-signal-wifi-3-line', 'ri-wifi-line'], 'broadcast': ['ri-broadcast-line'],
    'box': ['ri-box-3-line'], 'lock': ['ri-lock-line'], 'check': ['ri-check-line'],
}

# 稿里没有、按同一风格补画的（16×16 网格、1.35 描边、圆角端点；由 .i 提供 fill:none/stroke）。共 35 个：
#   5 个「界面里常见」的（箭头、搜索、问号、感叹号圆）+ 28 个原先仍是 Remix 的（规则/组件/配置模块/LED 卡用的装饰图标）。
def _gear():
    pts = []
    for k in range(8):
        for da, r in ((-15, 4.6), (-8, 6.1), (8, 6.1), (15, 4.6)):
            th = math.radians(k * 45 + da - 90)
            pts.append((8 + r * math.cos(th), 8 + r * math.sin(th)))
    return '<path d="M' + 'L'.join('%.1f %.1f' % p for p in pts) + 'Z"/><circle cx="8" cy="8" r="1.9"/>'


def _fan():
    blade = '<path d="M8 7.2C7.6 5.2 8.3 3.2 10 2.5c1.3-.5 2.3.5 2 1.7-.4 1.6-2.2 2.7-4 3Z"%s/>'
    return blade % '' + blade % ' transform="rotate(120 8 8)"' + blade % ' transform="rotate(240 8 8)"' + '<circle cx="8" cy="8" r=".9"/>'


DRAWN = {
    # —— 常见的 5 个 ——
    'ri-arrow-up-line': '<path d="M8 13V3M4.5 6.5 8 3l3.5 3.5"/>',
    'ri-arrow-down-line': '<path d="M8 3v10M4.5 9.5 8 13l3.5-3.5"/>',
    'ri-search-line': '<circle cx="7" cy="7" r="4.2"/><path d="M10.2 10.2 13.5 13.5"/>',
    'ri-question-line': '<circle cx="8" cy="8" r="5.5"/><path d="M6.3 6.4a1.8 1.8 0 1 1 2.5 1.7c-.5.3-.8.7-.8 1.3M8 11.3v.1"/>',
    'ri-error-warning-line': '<circle cx="8" cy="8" r="5.5"/><path d="M8 5v3.8M8 11v.1"/>',
    # —— 原先仍是 Remix 的 28 个 ——
    # 「进度条」与「双数值」、「文本」与「日志流」在添加组件面板里并排，需要不同的图：横向条形图、带两行字的文档
    'ri-bar-chart-line': '<path d="M2.5 4.5h7M2.5 8h11M2.5 11.5h5"/>',
    'ri-file-list-line': '<path d="M4 2.5h5l3 3v8H4v-11ZM9 2.5v3h3M6.3 8.4h3.4M6.3 10.8h3.4"/>',
    'ri-brain-line': '<rect x="1.5" y="4.5" width="13" height="6" rx="1.2"/><path d="M5 6.8v1.4M8 6.8v1.4M11 6.8v1.4M4 10.5V13M6.7 10.5V13M9.3 10.5V13M12 10.5V13"/>',   # 内存条
    'ri-checkbox-blank-circle-fill': '<circle cx="8" cy="8" r="3.5" fill="currentColor" stroke="none"/>',
    'ri-computer-line': '<rect x="2" y="2.8" width="12" height="8.2" rx="1.5"/><path d="M8 11v2.6M5.5 13.6h5"/>',
    'ri-cpu-line': '<rect x="4" y="4" width="8" height="8" rx="1.6"/><rect x="6.4" y="6.4" width="3.2" height="3.2" rx=".5"/><path d="M6.3 2v2M9.7 2v2M6.3 12v2M9.7 12v2M2 6.3h2M2 9.7h2M12 6.3h2M12 9.7h2"/>',
    'ri-dashboard-3-line': '<path d="M2.5 11.5a5.5 5.5 0 1 1 11 0M8 11.5l2.4-3.4M8 11.5v.1"/>',
    'ri-emotion-line': '<circle cx="8" cy="8" r="5.5"/><path d="M5.6 9.6a3 3 0 0 0 4.8 0M5.9 6.4v.1M10.1 6.4v.1"/>',
    'ri-focus-line': '<circle cx="8" cy="8" r="4"/><path d="M8 1.8v2.2M8 12v2.2M1.8 8H4M12 8h2.2M8 8v.1"/>',
    'ri-gamepad-line': '<rect x="1.5" y="4.5" width="13" height="7.5" rx="3.2"/><path d="M5 6.7v2.6M3.7 8h2.6M10.3 7.1v.1M11.9 8.9v.1"/>',
    'ri-hard-drive-line': '<rect x="2" y="3.5" width="12" height="9" rx="1.6"/><path d="M2 9h12M11.5 11v.1"/>',
    'ri-image-line': '<rect x="2" y="3" width="12" height="10" rx="1.6"/><circle cx="5.8" cy="6.6" r=".9"/><path d="M2.3 11.2 6 8.2l2.6 2.3 1.8-1.6 3.3 2.7"/>',
    'ri-lightbulb-flash-line': '<path d="M6 11.5h4M6.5 13.5h3M8 2.5a3.6 3.6 0 0 0-2 6.6c.4.4.5.8.5 1.4h3c0-.6.1-1 .5-1.4A3.6 3.6 0 0 0 8 2.5Z"/><path d="M8.6 4.6 7.2 7h1.6L7.4 9.2"/>',
    'ri-movie-line': '<rect x="2" y="3.2" width="12" height="9.6" rx="1.6"/><path d="M6.8 6.1v3.8L10.2 8 6.8 6.1Z"/>',
    'ri-music-line': '<path d="M6 11.6V3.8l6.5-1.3v7.7M6 6.2l6.5-1.3"/><circle cx="4.5" cy="11.8" r="1.6"/><circle cx="11" cy="10.4" r="1.6"/>',
    'ri-notification-line': '<path d="M3.5 11.2h9l-1.2-1.6V7a3.3 3.3 0 0 0-6.6 0v2.6L3.5 11.2ZM6.6 13.4h2.8M8 2.5v1.2"/>',
    'ri-numbers-line': '<path d="M6.2 2.5 5 13.5M11 2.5 9.8 13.5M2.8 6h10.4M2.6 10h10.4"/>',
    'ri-percent-line': '<circle cx="4.8" cy="4.8" r="1.7"/><circle cx="11.2" cy="11.2" r="1.7"/><path d="M12 3.6 4 12.4"/>',
    'ri-plug-line': '<path d="M6 2.3v3M10 2.3v3M4.5 5.5h7v2.4a3.5 3.5 0 0 1-7 0V5.5ZM8 11.4v2.4"/>',
    'ri-progress-6-line': '<path d="M13.5 8A5.5 5.5 0 1 1 8 2.5V8h5.5"/>',
    'ri-record-circle-fill': '<circle cx="8" cy="8" r="5.5"/><circle cx="8" cy="8" r="2.2" fill="currentColor" stroke="none"/>',
    'ri-rocket-line': '<path d="M8 1.8c2.2 1.4 3.2 3.7 2.9 6.4L8 11 5.1 8.2C4.8 5.5 5.8 3.2 8 1.8ZM5.1 8.4 3.4 10.2l.6 2 2.1-.6M10.9 8.4l1.7 1.8-.6 2-2.1-.6M7 12.6 8 14.4l1-1.8"/><circle cx="8" cy="5.9" r="1.05"/>',
    'ri-router-line': '<rect x="2" y="8.5" width="12" height="4.5" rx="1.4"/><path d="M5 10.7v.1M7.4 10.7v.1M6.6 7.1a2 2 0 0 1 2.8 0M5.2 5.7a4 4 0 0 1 5.6 0"/>',
    'ri-settings-line': _gear(),
    'ri-smartphone-line': '<rect x="4.5" y="1.8" width="7" height="12.4" rx="1.8"/><path d="M7 12h2"/>',
    'ri-temp-hot-line': '<path d="M6.4 9.3V3.7a1.6 1.6 0 0 1 3.2 0v5.6a3.1 3.1 0 1 1-3.2 0ZM8 6.2v5.4"/>',
    'ri-thunderstorms-line': '<path d="M9.2 1.8 4.4 9h3.4l-.8 5.2 4.8-7.2H8.6l.6-5.2Z"/>',
    'ri-timer-line': '<circle cx="8" cy="9" r="4.8"/><path d="M8 9V6.6M6.5 1.9h3M12 4.5l1 1"/>',
    'ri-tools-line': '<path d="M13.2 4.9A3 3 0 1 1 11.1 2.8M7.1 7.7 2.8 12a.9.9 0 0 0 1.3 1.3l4.3-4.3"/>',
    'ri-tornado-line': _fan(),   # 配置模块「风扇」用它，画成风扇叶片
}


def board_glyphs():
    raw = {}
    for f in sorted(glob.glob(os.path.join(BOARDS, '*.html'))):
        s = open(f, encoding='utf-8').read()
        for m in re.finditer(r'<svg class="i"[^>]*>([\s\S]*?)</svg>', s):
            raw.setdefault(re.sub(r'\s+', ' ', m.group(1)).strip(), 0)
    out = {}
    for name, key in KEY.items():
        hit = [r for r in raw if key in r]
        if len(hit) != 1:
            sys.exit(f'稿里找不到唯一的图形：{name}（{key}）→ {len(hit)} 个')
        out[name] = hit[0]
    return out


def main():
    check = '--check' in sys.argv
    glyphs = board_glyphs()
    html = open(INDEX, encoding='utf-8').read()
    new = {}
    for g, names in MAP.items():
        for n in names:
            new[n] = glyphs[g]
    new.update(DRAWN)
    out, changed, missing = html, 0, []
    for name, inner in new.items():
        pat = re.compile(r'<symbol id="%s"[^>]*>[\s\S]*?</symbol>' % re.escape(name))
        if not pat.search(out):
            missing.append(name)
            continue
        repl = '<symbol id="%s" viewBox="0 0 16 16">%s</symbol>' % (name, inner)
        if pat.search(out).group(0) != repl:
            changed += 1
        out = pat.sub(lambda m: repl, out, count=1)
    if missing:
        sys.exit('精灵里没有这些 symbol：' + ', '.join(missing))
    print(f'图形 {len(glyphs)} 种；换掉 {len(new)} 个 symbol（其中 {changed} 个有变化）；index.html {len(html.encode())} → {len(out.encode())} 字节')
    if check:
        sys.exit(1 if changed else 0)
    if out != html:
        open(INDEX, 'w', encoding='utf-8').write(out)
        print('已写入', INDEX)


main()
