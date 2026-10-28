#!/usr/bin/env python3
"""Build or flash the matching storage-v2 USB/serial package. Never erase keys."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def partition_table(data):
    entries = []
    for offset in range(0, len(data), 32):
        row = data[offset:offset + 32]
        if len(row) != 32:
            break
        magic = struct.unpack_from('<H', row)[0]
        if magic == 0xEBEB:
            if hashlib.md5(data[:offset]).digest() != row[16:]:
                raise ValueError('分区表校验失败 / Partition table checksum failed')
            return entries
        if magic != 0x50AA:
            break
        _, kind, subtype, address, size, label, flags = struct.unpack('<HBBII16sI', row)
        entries.append(dict(name=label.rstrip(b'\0').decode('ascii'), type=kind,
                            subtype=subtype, offset=address, size=size, flags=flags))
    raise ValueError('无法识别分区表 / Unrecognized partition table')


def layouts(entries):
    by_name = {e['name']: e for e in entries}
    for name, kind, subtype, address, size in [
            ('keys_nvs', 1, 2, 0x9000, 0xC000), ('nvs', 1, 2, 0x9B0000, 0x20000),
            ('auto_cfg', 1, 0x82, 0x9D0000, 0x200000)]:
        item = by_name[name]
        if (item['type'], item['subtype'], item['offset'], item['size']) != (kind, subtype, address, size):
            raise ValueError('构建分区不匹配 / Build partition layout does not match storage v2')
    old = [dict(e, name='nvs') if e['name'] == 'keys_nvs' else e
           for e in entries if e['name'] not in ('nvs', 'auto_cfg')]
    return old, entries


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(build, out):
    build, out = build.resolve(), out.resolve()
    args = json.loads((build / 'flasher_args.json').read_text())
    entries = partition_table((build / args['partition-table']['file']).read_bytes())
    layouts(entries)
    slots = sorted((e for e in entries if e['type'] == 0 and e['subtype'] in (0x10, 0x11)),
                   key=lambda e: e['subtype'])
    if len(slots) != 2:
        raise ValueError('需要两个 OTA 应用槽 / Two OTA application slots are required')
    app = args['app']['file']
    if any((build / app).stat().st_size > slot['size'] for slot in slots):
        raise ValueError('固件超过应用分区大小 / Firmware exceeds its application partition')
    images = {int(address, 0): name for address, name in args['flash_files'].items()}
    for slot in slots:
        images[slot['offset']] = app
    protected = [(e['offset'], e['offset'] + e['size']) for e in entries
                 if e['name'] in ('keys_nvs', 'nvs_keys', 'nvs', 'auto_cfg', 'storage', 'fatfs')]
    for address, name in images.items():
        end = address + (build / name).stat().st_size
        if any(address < high and end > low for low, high in protected):
            raise ValueError('刷写镜像覆盖数据区 / Flash image overlaps a preserved data partition')
    out.mkdir(parents=True, exist_ok=False)
    for name in set(images.values()):
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(build / name, target)
    manifest = dict(format=1, build_version=json.loads((build / 'project_description.json').read_text())['project_version'],
                    partition_table=args['partition-table']['file'],
                    partition_table_offset=int(args['partition-table']['offset'], 0),
                    flash_settings=args['write_flash_args'],
                    images=[dict(offset=address, file=name) for address, name in sorted(images.items())],
                    files={name: sha256(out / name) for name in sorted(set(images.values()))})
    (out / 'upgrade.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copyfile(Path(__file__), out / 'upgrade.py')
    (out / 'requirements.txt').write_text('esptool==4.11.0\n')
    (out / 'README.md').write_text(README)
    archive = shutil.make_archive(str(out), 'zip', out)
    print(archive)


def run_esptool(port, baud, after, *args):
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
                    '--baud', str(baud), '--before', 'default_reset', '--after', after,
                    *map(str, args)], check=True)


def flash(folder, port, baud):
    folder = folder.resolve()
    manifest = json.loads((folder / 'upgrade.json').read_text())
    for name, digest in manifest['files'].items():
        if sha256(folder / name) != digest:
            raise ValueError('升级包文件校验失败，请重新下载 / Package checksum failed; download again')
    old, new = layouts(partition_table((folder / manifest['partition_table']).read_bytes()))
    with tempfile.TemporaryDirectory() as temp:
        current = Path(temp) / 'partition-table.bin'
        run_esptool(port, baud, 'no_reset', 'read_flash', hex(manifest['partition_table_offset']), '0x1000', current)
        installed = partition_table(current.read_bytes())
    if installed == old:
        print('首次升级：初始化新增配置区，保留原密钥 / First upgrade: initialize new regions; preserve keys', flush=True)
        system = next(e for e in new if e['name'] == 'nvs')
        auto = next(e for e in new if e['name'] == 'auto_cfg')
        run_esptool(port, baud, 'no_reset', 'erase_region', hex(system['offset']),
                    hex(auto['offset'] + auto['size'] - system['offset']))
    elif installed == new:
        print('重复升级：保留系统设置和应急配置 / Repeat upgrade: preserve system and emergency configuration', flush=True)
    else:
        raise ValueError('设备分区与此升级包不匹配，请联系厂商 / Device layout does not match this package; contact the manufacturer')
    images = [part for image in manifest['images'] for part in (hex(image['offset']), folder / image['file'])]
    run_esptool(port, baud, 'hard_reset', 'write_flash', *manifest['flash_settings'], *images)
    print('刷写完成。请保留原 SD 卡，等待首次配置迁移 / Flash complete. Keep the original SD card inserted for first-start migration.', flush=True)


README = '''# TianShanOS USB／串口升级

这是存储方案 v2 的配套包，包含固件、网页、新分区表和引导文件。
请使用厂商确认可交付的包。开发候选包仍须经厂商确认后，才能用于客户机器。

1. 保留原 SD 卡并插好，使用能传输数据的 USB 线连接设备，关闭占用串口的软件。
2. 安装 Python 3，然后在本目录执行 `python -m pip install -r requirements.txt`。
3. Windows 示例：`python upgrade.py flash --port COM3`。
   macOS／Linux 示例：`python3 upgrade.py flash --port /dev/实际串口名称`。
4. 等待“刷写完成”。首次启动会迁移配置；不要断电。进入网页核对原有设置、规则和主机。

程序会先读取并识别设备分区。首次从旧布局升级时，只清空原来未分配的新增配置区域；
重复刷同一布局时保留系统设置和应急副本。所有升级均保留原密钥区域、NVS 加密材料和 SD 卡。
两个应用槽均写入此包的固件，防止回退到不兼容的旧固件。
请勿另行使用全片擦除、旧分区表或旧版 RootReset 固件。
包内 `upgrade.json` 记录构建版本、镜像地址及校验值。出现错误后程序停止，不会自动重试或降级。
启用 Secure Boot／Flash Encryption 的设备须由厂商提供对应签名／加密刷写方案；此包不绕过设备保护。

# USB / serial upgrade

Use a manufacturer-approved package. Development candidates require manufacturer approval before customer use.
Keep the original SD card inserted, connect a USB data cable, and close other serial applications.
Install Python 3 and run `python -m pip install -r requirements.txt` in this folder.
Run `python upgrade.py flash --port COM3` on Windows, or replace COM3 with your serial device path.
Wait for completion and first-start configuration migration; keep power connected.
Check the existing system settings, rules and SSH hosts in the Web UI.

The tool identifies the installed partition table before writing. On the first upgrade, it initializes
only the newly allocated configuration regions. Reinstalling the same layout preserves those regions.
Keys, NVS encryption material and the SD card are preserved. Both application slots receive compatible firmware.
Do not erase the whole flash, install an old partition table, or use an old RootReset image.
The manifest records the build version, addresses and file hashes. Errors stop the operation without retries.
Secure Boot / Flash Encryption devices require a manufacturer-provided signed / encrypted flashing procedure;
this package does not bypass device protection.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    make = sub.add_parser('package')
    make.add_argument('--build-dir', type=Path, required=True)
    make.add_argument('--output', type=Path, required=True)
    install = sub.add_parser('flash')
    install.add_argument('--port', required=True)
    install.add_argument('--baud', type=int, default=460800)
    args = parser.parse_args()
    try:
        if args.command == 'package':
            package(args.build_dir, args.output)
        else:
            flash(Path(__file__).resolve().parent, args.port, args.baud)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
