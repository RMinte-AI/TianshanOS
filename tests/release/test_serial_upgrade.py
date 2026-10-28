"""Offline package / serial command contract. No device or serial driver is exercised."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('serial_upgrade', ROOT / 'tools/serial_upgrade.py')
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


def table(entries):
    rows = b''.join(struct.pack('<HBBII16sI', 0x50AA, e['type'], e['subtype'],
                                e['offset'], e['size'], e['name'].encode(), e['flags']) for e in entries)
    return (rows + b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(rows).digest()).ljust(4096, b'\xff')


class SerialUpgrade(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / 'build'
        self.build.mkdir()
        self.entries = []
        kinds = {'data': 1, 'app': 0}
        subs = {'nvs': 2, 'nvs_keys': 4, 'ota': 0, 'phy': 1,
                'ota_0': 0x10, 'ota_1': 0x11, 'spiffs': 0x82, 'fat': 0x81}
        for line in (ROOT / 'partitions.csv').read_text().splitlines():
            if not line or line.startswith('#'):
                continue
            name, kind, sub, offset, size, _ = [p.strip() for p in line.split(',')]
            self.entries.append(dict(name=name, type=kinds[kind], subtype=subs[sub],
                                     offset=int(offset, 0), size=int(size, 0), flags=0))
        files = {'0x0': 'bootloader/bootloader.bin', '0x8000': 'partition_table/partition-table.bin',
                 '0x16000': 'ota_data_initial.bin', '0x20000': 'TianShanOS.bin', '0x6a0000': 'www.bin'}
        for name in files.values():
            dest = self.build / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(table(self.entries) if name.endswith('partition-table.bin') else b'fixture')
        args = dict(flash_files=files, write_flash_args=['--flash_mode', 'dio', '--flash_size', '16MB'],
                    app=dict(file='TianShanOS.bin'),
                    **{'partition-table': dict(file='partition_table/partition-table.bin', offset='0x8000')})
        (self.build / 'flasher_args.json').write_text(json.dumps(args))
        (self.build / 'project_description.json').write_text('{"project_version":"fixture-local"}')
        self.folder = Path(self.temp.name) / 'package'
        upgrade.package(self.build, self.folder)
        self.calls = []

    def run_flash(self, entries):
        def esptool(port, baud, after, *args):
            self.assertEqual(port, 'fixture-port')
            self.calls.append((after, args))
            if args[0] == 'read_flash':
                Path(args[-1]).write_bytes(table(entries))
        with patch.object(upgrade, 'run_esptool', esptool):
            upgrade.flash(self.folder, 'fixture-port', 460800)

    def test_first_upgrade_initializes_only_new_regions_and_writes_both_slots(self):
        old, _ = upgrade.layouts(self.entries)
        self.run_flash(old)
        self.assertEqual([c[1][0] for c in self.calls], ['read_flash', 'erase_region', 'write_flash'])
        self.assertEqual(self.calls[1], ('no_reset', ('erase_region', '0x9b0000', '0x220000')))
        args = self.calls[-1][1]
        self.assertIn('0x20000', args)
        self.assertIn('0x320000', args)
        self.assertEqual(args[args.index('0x20000') + 1], args[args.index('0x320000') + 1])
        self.assertEqual(self.calls[-1][0], 'hard_reset')

    def test_reinstall_preserves_configuration_regions(self):
        self.run_flash(self.entries)
        self.assertEqual([c[1][0] for c in self.calls], ['read_flash', 'write_flash'])

    def test_unknown_device_layout_stops_before_writing(self):
        changed = [dict(e) for e in self.entries]
        changed[0]['size'] -= 4096
        with self.assertRaises(ValueError):
            self.run_flash(changed)
        self.assertEqual([c[1][0] for c in self.calls], ['read_flash'])

    def test_bad_image_stops_before_connecting(self):
        (self.folder / 'TianShanOS.bin').write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.run_flash(self.entries)
        self.assertFalse(self.calls)

    def test_partition_table_checksum_is_checked(self):
        raw = bytearray(table(self.entries))
        raw[5] ^= 1
        with self.assertRaises(ValueError):
            upgrade.partition_table(raw)


if __name__ == '__main__':
    unittest.main()
