#!/usr/bin/env python3
"""把 index.html 内联精灵里的 82 个图标换成 Phosphor Regular（MIT，@phosphor-icons/core 2.1.1）。

对应表：tests/macos27-r2/phosphor-map.json（id 不变，只换图形；phosphor 为 null 的保留现有自绘：路由器、实心圆点）。
源 SVG：output/macos27-r2/assets/phosphor-2.1.1/regular/<名字>.svg；缺的从 jsDelivr 固定版本下载（需要联网，下载前先征得使用者同意）。
Phosphor 的 SVG 是 256×256 的轮廓路径（fill），所以 symbol 里包一层 <g fill="currentColor" stroke="none">，
盖过 .i 的 fill:none / stroke:currentColor；Regular 档描边在 256 栅格里是 16 单位，16px 下约 1.0px，24px 下约 1.5px，随尺寸等比。

  python3 tests/macos27-r2/icons_from_phosphor.py           # 改写 components/ts_webui/web/index.html
  python3 tests/macos27-r2/icons_from_phosphor.py --check   # 只检查，不改文件（已是目标状态则退出码 0）

幂等：symbol 内容只由「对应表 + 源 SVG」决定，重复运行结果相同。取代旧的 icons_from_boards.py（稿的原图形）。
"""
import json, os, re, sys, urllib.request

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
INDEX = os.path.join(REPO, 'components/ts_webui/web/index.html')
MAP = os.path.join(os.path.dirname(__file__), 'phosphor-map.json')
M = json.load(open(MAP, encoding='utf-8'))
CACHE = os.path.join(REPO, 'output/macos27-r2/assets', f"phosphor-{M['version']}", M['weight'])
URL = f"https://cdn.jsdelivr.net/npm/{M['package']}@{M['version']}/assets/{M['weight']}/%s.svg"


def source(name):
    path = os.path.join(CACHE, name + '.svg')
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        print('下载', name, URL % name)
        req = urllib.request.Request(URL % name, headers={'User-Agent': 'tianshanos-icons/1'})
        data = urllib.request.urlopen(req, timeout=30).read()
        open(path, 'wb').write(data)
    return open(path, encoding='utf-8').read()


def inner(svg, name):
    m = re.search(r'<svg\b([^>]*)>(.*)</svg>', svg, re.S)
    assert m and 'viewBox="0 0 256 256"' in m.group(1), f'{name}: 不是 256 栅格的 SVG'
    body = re.sub(r'\s*/>', '/>', re.sub(r'\s+', ' ', m.group(2)).strip())
    assert not re.search(r'<(style|defs|g|use)\b|\bid=', body), f'{name}: 含不支持的结构'
    return body


def main():
    check = '--check' in sys.argv
    html = open(INDEX, encoding='utf-8').read()
    pat = re.compile(r'<symbol id="(ri-[^"]+)" viewBox="([^"]*)">(.*?)</symbol>', re.S)
    found = pat.findall(html)
    ids = [i for i, _, _ in found]
    icons = M['icons']
    missing, extra = sorted(set(icons) - set(ids)), sorted(set(ids) - set(icons))
    if missing or extra:
        sys.exit(f'对应表与精灵不一致：表里有精灵里没有 {missing}；精灵里有表里没有 {extra}')
    changed = [0]

    def repl(m):
        i, vb, content = m.group(1), m.group(2), m.group(3)
        name = icons[i]['phosphor']
        if not name:
            return m.group(0)   # 保留自绘
        new = f'<symbol id="{i}" viewBox="0 0 256 256"><g fill="currentColor" stroke="none">{inner(source(name), name)}</g></symbol>'
        if new != m.group(0): changed[0] += 1
        return new

    out = pat.sub(repl, html)
    kept = [i for i in ids if not icons[i]['phosphor']]
    sprite = ''.join(m.group(0) for m in pat.finditer(out))
    print(f"精灵 {len(ids)} 个：Phosphor {len(ids) - len(kept)} 个，保留自绘 {len(kept)} 个（{', '.join(kept)}）；有变化 {changed[0]} 个；精灵 {len(sprite.encode())} B；index.html {len(html.encode())} → {len(out.encode())} B")
    if check:
        sys.exit(0 if out == html else 1)
    if out != html:
        open(INDEX, 'w', encoding='utf-8').write(out)
        print('已写入', INDEX)


main()
