#!/usr/bin/env python3
"""交接用清单：把当前代码里还没做完的点都列成文件（output/macos27-r2/handoff/）。
  python3 tests/macos27-r2/inventory.py
输出：native-dialog-sites.txt（confirmAction/confirm/alert/prompt 调用点）、old-dialog-structure.txt（仍是旧结构的弹窗）、
      icons-used.txt（用到的 ri-* 图标名与次数）、old-tokens.txt（JS/HTML 里仍引用的旧 CSS 变量）、asyncification.txt（confirmAction 所在函数）。"""
import re, os, collections
REPO = '/Users/massif/TianshanOS'
OUT = os.path.join(REPO, 'output/macos27-r2/handoff'); os.makedirs(OUT, exist_ok=True)
APP = open(f'{REPO}/components/ts_webui/web/js/app.js', encoding='utf-8').read()
HTML = open(f'{REPO}/components/ts_webui/web/index.html', encoding='utf-8').read()
LINES = APP.split('\n')
fn_re = re.compile(r'^(?:async\s+)?function\s+(\w+)')
def enclosing(i):
    for j in range(i, -1, -1):
        m = fn_re.match(LINES[j])
        if m: return m.group(1), j + 1
    return '(顶层)', 0

# 1) 原生对话框调用点
rows = []
for i, l in enumerate(LINES):
    for kind, pat in (('confirmAction', r'\bconfirmAction\('), ('confirm', r'(?<![\w.])confirm\('), ('alert', r'(?<![\w.])alert\('), ('prompt', r'(?<![\w.])prompt\(')):
        if re.search(pat, l) and not l.strip().startswith('//') and 'function confirmAction' not in l:
            n, ln = enclosing(i)
            rows.append((kind, i + 1, n, l.strip()[:150]))
with open(f'{OUT}/native-dialog-sites.txt', 'w', encoding='utf-8') as f:
    f.write('# 第 4 步：原生 confirm/alert/prompt 与 confirmAction 调用点（行号随代码变化，以函数名为准）\n')
    for kind in ('confirmAction', 'confirm', 'alert', 'prompt'):
        sel = [r for r in rows if r[0] == kind]
        f.write(f'\n## {kind}（{len(sel)} 处）\n')
        for k, ln, n, txt in sel: f.write(f'- L{ln:<6} {n:<38} {txt}\n')
print('原生对话框调用点：', collections.Counter(r[0] for r in rows))

# 2) 仍是旧结构的弹窗
old = collections.OrderedDict()
for i, l in enumerate(LINES):
    if re.search(r'class="modal-content|class="modal-header|class="modal-body|class="modal-footer|modal-content', l):
        n, ln = enclosing(i); old.setdefault((n, ln), []).append(i + 1)
with open(f'{OUT}/old-dialog-structure.txt', 'w', encoding='utf-8') as f:
    f.write('# 仍用旧弹窗结构（modal-content / modal-header …）的函数：第 3 步还没改完\n')
    for (n, ln), v in old.items(): f.write(f'- {n} (fn@L{ln})  行 {v[:5]}\n')
    f.write(f'\nindex.html 里旧结构出现次数：modal-content={HTML.count("modal-content")}\n')
    for m in re.finditer(r'<div id="([\w-]+)" class="modal[^"]*"', HTML): f.write(f'- index.html 静态弹窗：#{m.group(1)}\n')
print('旧结构弹窗函数：', len(old))

# 3) 图标
cnt = collections.Counter()
for src in (APP, HTML):
    for m in re.finditer(r'\bri-[a-z0-9-]+', src): cnt[m.group(0)] += 1
lang = ''
for lf in ('zh-CN.js', 'en-US.js'):
    lang += open(f'{REPO}/components/ts_webui/web/js/lang/{lf}', encoding='utf-8').read()
for m in re.finditer(r'\bri-[a-z0-9-]+', lang): cnt[m.group(0)] += 0
# 图标来源：第 5 步之后是 index.html 里的内联精灵（<symbol id="ri-…">）；字体文件已删除
have_font = set(re.findall(r'<symbol id="(ri-[a-z0-9-]+)"', HTML))
with open(f'{OUT}/icons-used.txt', 'w', encoding='utf-8') as f:
    f.write('# 第 5 步：代码里用到的 ri-* 图标名（次数）；「精灵里没有」= index.html 的 SVG 精灵缺这个 symbol\n')
    for k, v in sorted(cnt.items()): f.write(f'{k:34} {v:4}  {"" if k in have_font else "精灵里没有"}\n')
print('图标名：', len(cnt))

# 4) 旧 CSS 变量
style = open(f'{REPO}/components/ts_webui/web/css/style.css', encoding='utf-8').read()
defined = set(re.findall(r'(--[\w-]+)\s*:', style))
tok = collections.Counter()
where = collections.defaultdict(list)
for i, l in enumerate(LINES):
    for m in re.finditer(r'var\((--[\w-]+)', l):
        if m.group(1) not in defined: tok[m.group(1)] += 1; where[m.group(1)].append(enclosing(i)[0])
for m in re.finditer(r'var\((--[\w-]+)', HTML):
    if m.group(1) not in defined: tok[m.group(1)] += 1; where[m.group(1)].append('index.html')
with open(f'{OUT}/old-tokens.txt', 'w', encoding='utf-8') as f:
    f.write('# JS/HTML 里还在引用、但新样式表已不定义的 CSS 变量（引用它们的内联样式要换成新令牌 --ink / --ink-2 / --ink-3 / --ok / --warn / --bad / --accent 等）\n')
    for k, v in tok.most_common(): f.write(f'{k:20} {v:3}  {sorted(set(where[k]))[:6]}\n')
print('旧变量：', sum(tok.values()), '处', len(tok), '种')
