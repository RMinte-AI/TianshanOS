#!/usr/bin/env python3
"""与固件同流程的构建 + 体积度量（在副本上做，不改源码目录）。

  python3 tests/macos27-r2/build.py --label step1                 # 构建当前工作区 web/
  python3 tests/macos27-r2/build.py --from-head --label head      # 构建 HEAD 的 web/，并写基线 JSON
  可选：--web DIR   --baseline JSON   --no-spiffs

流程（同 docs/macos27/TESTING.md「真实构建」）：
  复制 web/ -> tools/minify_web.py <副本> --gzip -> spiffsgen 0x300000（页 256、名长 32、meta 4、magic）
度量：非 0xFF 页数 = 已用页（含每块的 magic 页），与 HEAD 基线比较。
"""
import argparse, gzip, hashlib, json, os, re, shutil, subprocess, sys, tempfile, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(REPO, 'output', 'macos27-r2')
IDF_PY = '/Users/massif/.espressif/python_env/idf5.5_py3.12_env/bin/python'
SPIFFSGEN = '/Users/massif/esp/v5.5.2/esp-idf/components/spiffs/spiffsgen.py'
PAGE = 256
IMG_SIZE = 0x300000


def rel_files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = os.path.getsize(p)
    return out


def used_pages(img):
    data = open(img, 'rb').read()
    n = len(data) // PAGE
    erased = b'\xff' * PAGE
    used = sum(1 for i in range(n) if data[i * PAGE:(i + 1) * PAGE] != erased)
    return n, used


def integrity(built):
    res = {}
    css = os.path.join(built, 'css', 'style.css')
    if os.path.exists(css):
        s = open(css, encoding='utf-8').read()
        res['css_not_space_paren'] = s.count(':not (')
        res['css_calc_bare_plus'] = len(re.findall(r'calc\([^)]*[^\s(]\+[^\s)][^)]*\)', s))
    bad = []
    n = 0
    for rel, _ in rel_files(built).items():
        if rel.endswith('.gz'):
            n += 1
            plain = os.path.join(built, rel[:-3])
            try:
                if gzip.open(os.path.join(built, rel), 'rb').read() != open(plain, 'rb').read():
                    bad.append(rel)
            except Exception as e:  # noqa
                bad.append(rel + ' (' + type(e).__name__ + ')')
    res['gz_files'] = n
    res['gz_roundtrip_mismatch'] = bad
    return res


def css_parity(src_css, built_css):
    r = subprocess.run(['node', os.path.join(REPO, 'tests/macos27-r2/csscheck.cjs'), src_css, built_css], capture_output=True, text=True)
    first = (r.stdout.strip().splitlines() or [''])[0]
    try:
        d = json.loads(first)
    except Exception:  # noqa
        d = {'error': (r.stdout + r.stderr)[:300]}
    d['ok'] = r.returncode == 0
    d['detail'] = r.stdout.strip().splitlines()[1:9]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--web', default=os.path.join(REPO, 'components/ts_webui/web'))
    ap.add_argument('--label', default='cur')
    ap.add_argument('--from-head', action='store_true')
    ap.add_argument('--baseline', default=os.path.join(OUT, 'baseline', 'head-baseline.json'))
    ap.add_argument('--no-spiffs', action='store_true')
    a = ap.parse_args()

    bdir = os.path.join(OUT, 'build', a.label)
    if os.path.isdir(bdir):
        shutil.rmtree(bdir)  # 仅清理本脚本自己的产物目录
    os.makedirs(bdir)
    src = a.web
    tmp = None
    if a.from_head:
        tmp = tempfile.mkdtemp(prefix='ts-r2-head-')
        subprocess.run('git -C %s archive HEAD components/ts_webui/web | tar -x -C %s' % (REPO, tmp), shell=True, check=True)
        src = os.path.join(tmp, 'components/ts_webui/web')
    built = os.path.join(bdir, 'web_min')
    shutil.copytree(src, built)
    t0 = time.time()
    r = subprocess.run([sys.executable, os.path.join(REPO, 'tools/minify_web.py'), built, '--gzip'], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit('minify failed')
    files = rel_files(built)
    total = sum(files.values())
    gz_total = sum(v for k, v in files.items() if k.endswith('.gz'))
    res = {
        'label': a.label, 'time': time.strftime('%F %T'), 'src': src,
        'built_dir_bytes': total, 'gz_bytes': gz_total, 'files': files,
        'integrity': integrity(built),
    }
    res['integrity']['css_parity'] = css_parity(os.path.join(src, 'css', 'style.css'), os.path.join(built, 'css', 'style.css'))
    if not a.no_spiffs:
        img = os.path.join(bdir, 'www.bin')
        r = subprocess.run([IDF_PY, SPIFFSGEN, hex(IMG_SIZE), built, img, '--page-size=256', '--obj-name-len=32',
                            '--meta-len=4', '--use-magic', '--use-magic-len'], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, r.stderr)
            sys.exit('spiffsgen failed (image over 3 MiB?)')
        n, used = used_pages(img)
        res.update({'pages_total': n, 'used_pages': used, 'used_bytes': used * PAGE, 'free_bytes': (n - used) * PAGE,
                    'img_sha256': hashlib.sha256(open(img, 'rb').read()).hexdigest()})
    if a.from_head:
        os.makedirs(os.path.dirname(a.baseline), exist_ok=True)
        json.dump(res, open(a.baseline, 'w'), indent=1)
        shutil.rmtree(tmp, ignore_errors=True)
    base = json.load(open(a.baseline)) if os.path.exists(a.baseline) else None
    json.dump(res, open(os.path.join(bdir, 'report.json'), 'w'), indent=1)

    md = ['# 构建体积：%s' % a.label, '']
    if 'used_pages' in res:
        md.append('- SPIFFS 已用 **%d 页 = %d B**，空闲 %d B' % (res['used_pages'], res['used_bytes'], res['free_bytes']))
        if base and 'used_pages' in base:
            d = res['used_bytes'] - base['used_bytes']
            md.append('- 相对 HEAD 基线（%d 页 = %d B）：**%+d B（%+d 页）**%s' % (
                base['used_pages'], base['used_bytes'], d, res['used_pages'] - base['used_pages'],
                '  ✅ 不高于基线' if d <= 0 else ''))
    md.append('- 构建目录 %d B（其中 .gz %d B）' % (total, gz_total))
    md.append('- 完整性：%s' % json.dumps(res['integrity'], ensure_ascii=False))
    if base:
        md += ['', '## 逐文件（相对基线，仅列有变化的）', '| 文件 | 基线 | 现在 | Δ |', '|---|---:|---:|---:|']
        keys = sorted(set(files) | set(base['files']))
        for k in keys:
            b0, c0 = base['files'].get(k, 0), files.get(k, 0)
            if b0 != c0:
                md.append('| %s | %d | %d | %+d |' % (k, b0, c0, c0 - b0))
    open(os.path.join(bdir, 'report.md'), 'w').write('\n'.join(md) + '\n')
    print('\n'.join(md[:8]))
    print('输出：', bdir)


if __name__ == '__main__':
    main()
