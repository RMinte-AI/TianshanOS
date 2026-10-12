#!/usr/bin/env python3
"""更新仓库 build/ 下的默认刷机镜像和 firmware-manifest.json（每次发版时使用）。

用法（在已做过干净构建的前提下）：
    python3 tools/make_firmware_manifest.py --build-dir <idf 构建目录> --source-commit <被构建的提交>
    python3 tools/make_firmware_manifest.py --verify          # 只核对 build/ 与清单是否一致

流程：
    1. 在“干净检出”的被构建提交上用 ESP-IDF 构建（set-target + build），保证版本号里的 HASH 后面没有 d（未提交修改）。
    2. 运行本脚本：把 6 个文件复制到 build/，并写出 build/firmware-manifest.json。
    3. 提交 build/ 的变化。清单里的 source_base_commit 是第 1 步构建的那个提交，镜像随其后的提交一起进入仓库。

清单字段：
    version / build_version / idf_version   版本；build_version 形如 0.6.2+HASH.月日时分
    source_base_commit                      被构建提交的完整 SHA
    source_inputs_sha256                    参与固件构建的已跟踪源文件的内容哈希：被构建提交中，排除 build/ docs/ tests/
                                            help/ assets/ .github/ 和 README*.md 之外的每个文件，按路径排序，逐个取
                                            "路径\\0文件内容的 sha256\\n"，再整体取 sha256
    files                                   6 个镜像/映射文件的字节数和 sha256
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IMAGES = ['TianShanOS.bin', 'www.bin', 'ota_data_initial.bin', 'flasher_args.json',
          'bootloader/bootloader.bin', 'partition_table/partition-table.bin']
EXCLUDED = ('build/', 'docs/', 'tests/', 'help/', 'assets/', '.github/')


def git(*args, binary=False):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, capture_output=True, text=not binary).stdout


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_inputs_sha256(commit):
    digest = hashlib.sha256()
    entries = git('ls-tree', '-r', '-z', '--full-tree', commit).split('\0')
    for entry in sorted(e for e in entries if e):
        meta, path = entry.split('\t', 1)
        if path.startswith(EXCLUDED) or (path.startswith('README') and path.endswith('.md')):
            continue
        blob = git('cat-file', 'blob', meta.split()[2], binary=True)
        digest.update(path.encode() + b'\0' + hashlib.sha256(blob).hexdigest().encode() + b'\n')
    return digest.hexdigest()


def describe(folder):
    return {name: {'bytes': (Path(folder) / name).stat().st_size, 'sha256': sha256_file(Path(folder) / name)} for name in IMAGES}


def make(build_dir, commit, out):
    build_dir, out = Path(build_dir), Path(out)
    description = json.loads((build_dir / 'project_description.json').read_text())
    build_version = description['project_version']
    version = build_version.split('+', 1)[0]
    expected = (ROOT / 'version.txt').read_text().strip()
    if version != expected:
        raise SystemExit(f'构建版本 {version} 与 version.txt（{expected}）不一致')
    full = git('rev-parse', commit).strip()
    stamp = build_version.split('+', 1)[1].split('.', 1)[0] if '+' in build_version else ''
    if not stamp or not full.startswith(stamp):   # 版本里的 HASH 后面多一个 d 表示构建时有未提交的修改
        raise SystemExit(f'构建版本 {build_version} 里的提交 {stamp} 与 {full[:12]} 对不上（可能带有未提交修改），请在干净检出上重新构建')
    for name in IMAGES:
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(build_dir / name, target)
    manifest = {'version': version, 'build_version': build_version, 'idf_version': description['git_revision'].split('-', 1)[0].lstrip('v'),   # 构建所用 ESP-IDF 的版本，如 v5.5.2
                'source_base_commit': full, 'source_inputs_sha256': source_inputs_sha256(full), 'files': describe(out)}
    (out / 'firmware-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def verify(out):
    out = Path(out)
    manifest = json.loads((out / 'firmware-manifest.json').read_text())
    expected = (ROOT / 'version.txt').read_text().strip()
    problems = []
    if manifest['version'] != expected or manifest['build_version'].split('+', 1)[0] != expected:
        problems.append(f'清单版本 {manifest["version"]} / {manifest["build_version"]} 与 version.txt（{expected}）不一致')
    for name, want in manifest['files'].items():
        path = out / name
        if not path.is_file():
            problems.append(f'{name} 不存在')
        elif path.stat().st_size != want['bytes'] or sha256_file(path) != want['sha256']:
            problems.append(f'{name} 与清单不一致')
    if set(manifest['files']) != set(IMAGES):
        problems.append('清单里的文件集合不对')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build-dir', help='ESP-IDF 构建目录（含 project_description.json）')
    parser.add_argument('--source-commit', help='被构建的提交')
    parser.add_argument('--out', default=str(ROOT / 'build'))
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.verify:
        problems = verify(args.out)
        print('\n'.join(problems) if problems else 'PASS build/ 与 firmware-manifest.json 一致，版本与 version.txt 一致')
        sys.exit(1 if problems else 0)
    if not (args.build_dir and args.source_commit):
        parser.error('需要 --build-dir 和 --source-commit')
    manifest = make(args.build_dir, args.source_commit, args.out)
    print(json.dumps({k: v for k, v in manifest.items() if k != 'files'}, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
