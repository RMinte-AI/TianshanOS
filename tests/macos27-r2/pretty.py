#!/usr/bin/env python3
"""把稿 HTML 排成缩进格式并缩写 svg 路径，便于阅读。python pretty.py <Board> [--full-svg] -> 打印"""
import re, sys
from html.parser import HTMLParser
VOID={'input','br','img','hr','meta','link','path','circle','rect','line','polyline','polygon','ellipse','use','stop'}
class P(HTMLParser):
    def __init__(s): super().__init__(convert_charrefs=False); s.out=[]; s.d=0; s.pending=''
    def flush(s):
        t=s.pending.strip()
        if t: s.out.append('  '*s.d+t)
        s.pending=''
    def attrs(s,a):
        r=''
        for k,v in a:
            if v is None: r+=' '+k
            else:
                if k=='d' and len(v)>44 and not FULL: v=v[:40]+'…'
                r+=f' {k}="{v}"'
        return r
    def handle_starttag(s,t,a):
        s.flush(); s.out.append('  '*s.d+f'<{t}{s.attrs(a)}>')
        if t not in VOID: s.d+=1
    def handle_startendtag(s,t,a): s.flush(); s.out.append('  '*s.d+f'<{t}{s.attrs(a)}/>')
    def handle_endtag(s,t):
        s.flush()
        if t not in VOID: s.d-=1; s.out.append('  '*s.d+f'</{t}>')
    def handle_data(s,d): s.pending+=d
    def handle_entityref(s,n): s.pending+=f'&{n};'
    def handle_charref(s,n): s.pending+=f'&#{n};'
FULL='--full-svg' in sys.argv
name=sys.argv[1]
src=open(f'/Users/massif/TianshanOS/docs/macos27/design-system/boards/{name}.html',encoding='utf-8').read()
src=re.sub(r'<svg viewBox="0 -760[^>]*>.*?</svg>','<svg WORDMARK/>',src,flags=re.S)
p=P(); p.feed(src); p.flush()
lines=p.out; out=[]; i=0
while i<len(lines):
    l=lines[i]
    m=re.match(r'^(\s*)<(\w+)([^>]*)>$',l)
    if m and i+2<len(lines) and lines[i+2].strip()=='</'+m.group(2)+'>' and not lines[i+1].strip().startswith('<'):
        out.append(l+lines[i+1].strip()+'</'+m.group(2)+'>'); i+=3; continue
    if m and i+1<len(lines) and lines[i+1].strip()=='</'+m.group(2)+'>':
        out.append(l+'</'+m.group(2)+'>'); i+=2; continue
    out.append(l); i+=1
print('\n'.join(out))
