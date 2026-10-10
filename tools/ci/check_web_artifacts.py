"""Check the emitted Web directory and the firmware build's www mapping."""
import argparse
import gzip
import json
from pathlib import Path
import subprocess

ESSENTIAL = ('index.html', 'js/app.js', 'js/api.js', 'js/router.js', 'js/terminal.js',
             'js/lang/en-US.js', 'js/lang/zh-CN.js', 'vendor/xterm/xterm.min.js',
             'vendor/xterm/fit.min.js', 'vendor/xterm/xterm.css')


def check_build(build):
    build = Path(build).resolve()
    www = build / 'www.bin'
    if not www.is_file() or not www.stat().st_size:
        raise ValueError('This build did not generate www.bin')
    files = json.loads((build / 'flasher_args.json').read_text())['flash_files']
    if www not in [(build / path).resolve() for path in files.values()]:
        raise ValueError('This build does not flash its www.bin')
    print('PASS same-build www.bin and flasher mapping')


def check_web(root):
    root = Path(root)
    for name in ESSENTIAL:
        if not (root / name).is_file():
            raise ValueError(f'Missing built resource: {name}')
    resources = sorted(p for p in root.rglob('*') if p.suffix in ('.js', '.css', '.html'))
    for resource in resources:
        packed = Path(str(resource) + '.gz')
        if not packed.is_file():
            raise ValueError(f'Missing required gzip: {packed.relative_to(root)}')
        if gzip.decompress(packed.read_bytes()) != resource.read_bytes():
            raise ValueError(f'Gzip differs from built resource: {resource.relative_to(root)}')
        if resource.suffix == '.js':
            subprocess.run(['node', '--check', str(resource)], check=True, capture_output=True)
    for packed in root.rglob('*.gz'):
        if not Path(str(packed)[:-3]).is_file():
            raise ValueError(f'Gzip has no source: {packed.relative_to(root)}')
    print(f'PASS {len(resources)} built resources: syntax and required gzip identity')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build-dir', type=Path)
    mode.add_argument('--web-root', type=Path)
    args = parser.parse_args()
    if args.build_dir:
        check_build(args.build_dir)
    else:
        check_web(args.web_root)


if __name__ == '__main__':
    main()
