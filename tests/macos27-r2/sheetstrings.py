#!/usr/bin/env python3
"""列出稿里某个弹窗的全部可见文案（文本节点 / placeholder / value / aria-label），并给出语言包里的候选 key。
用法：python3 sheetstrings.py <稿名> <序号|标题片段>"""
import json, os, re, subprocess, sys
from html.parser import HTMLParser
REPO = '/Users/massif/TianshanOS'
sys.path.insert(0, os.path.dirname(__file__))
board, sel = sys.argv[1], sys.argv[2]
src = open(f'{REPO}/docs/macos27/design-system/boards/{board}.html', encoding='utf-8').read()
# 找出各 .sheet 起止（按 div 嵌套深度）
starts = [m.start() for m in re.finditer(r'<div class="[^"]*\bsheet\b[^"]*"', src)]
def extract(i):
    a = starts[i]; depth = 0
    for m in re.finditer(r'<div\b|</div>', src[a:]):
        depth += 1 if m.group(0) == '<div' else -1
        if depth == 0: return src[a:a + m.end()]
if sel.isdigit(): idx = int(sel)
else:
    idx = next(i for i, a in enumerate(starts) if sel in re.sub(r'<[^>]+>', '', extract(i)[:400]))
frag = extract(idx)
out = []
class P(HTMLParser):
    def handle_starttag(s, tag, attrs):
        d = dict(attrs)
        for k in ('placeholder', 'value', 'aria-label', 'title'):
            if d.get(k): out.append((k, d[k]))
    def handle_data(s, data):
        t = ' '.join(data.split())
        if t: out.append(('text', t))
P().feed(frag)
JS = r"""const vm=require('vm'),fs=require('fs');let cap;vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),{i18n:{registerLanguage:(c,o)=>{cap=o}}});
function flat(o,p,out){for(const[k,v]of Object.entries(o)){const key=p?p+'.'+k:k;if(v&&typeof v==='object')flat(v,key,out);else out[key]=v}return out}
console.log(JSON.stringify(flat(cap,'',{})))"""
zh = json.loads(subprocess.check_output(['node', '-e', JS, f'{REPO}/components/ts_webui/web/js/lang/zh-CN.js']))
inv = {}
for k, v in zh.items():
    if isinstance(v, str): inv.setdefault(v.strip(), []).append(k)
seen = set()
print(f'# {board} #{idx}')
for kind, s in out:
    if (kind, s) in seen: continue
    seen.add((kind, s))
    ks = inv.get(s, [])
    pref = [k for k in ks if k.split('.')[0] in ('securityPage', 'common')] or ks
    print(f'{kind:12} {s[:60]:62} {" | ".join(pref[:3]) if pref else "—— 无现成 key"}')
