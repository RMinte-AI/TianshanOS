"""In-process download contract; fixtures are never uploaded to a device."""
import struct
import tempfile
import unittest
from pathlib import Path
from ota_server import OTAServer, ESP_APP_DESC_MAGIC, ESP_APP_DESC_OFFSET


class Downloads(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.firmware = bytearray(512)
        struct.pack_into('<I', self.firmware, ESP_APP_DESC_OFFSET, ESP_APP_DESC_MAGIC)
        self.firmware[ESP_APP_DESC_OFFSET + 16:ESP_APP_DESC_OFFSET + 21] = b'1.2.3'
        self.web = bytes(range(256)) * 2
        (root / 'firmware.bin').write_bytes(self.firmware)
        (root / 'www.bin').write_bytes(self.web)
        self.server = OTAServer(str(root / 'firmware.bin'), str(root / 'www.bin'))
        self.client = self.server.app.test_client()

    def test_normal_update_downloads_and_alias(self):
        version = self.client.get('/version')
        self.assertEqual(version.status_code, 200)
        self.assertEqual(version.json['version'], '1.2.3')
        self.assertTrue(version.json['www_available'])
        with self.client.get('/firmware') as response:
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.data, self.firmware)
        for route in ('/www.bin', '/www'):
            with self.subTest(route=route), self.client.get(route) as response:
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.data, self.web)
                self.assertEqual(response.content_length, len(self.web))
            with self.client.head(route) as response:
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.data, b'')
                self.assertEqual(response.content_length, len(self.web))
            with self.client.get(route, headers={'Range': 'bytes=9-19'}) as response:
                self.assertEqual(response.status_code, 206)
                self.assertEqual(response.data, self.web[9:20])
                self.assertEqual(response.headers['Content-Range'], 'bytes 9-19/512')

    def test_missing_web_is_not_firmware(self):
        client = OTAServer(self.server.firmware_path).app.test_client()
        for route in ('/www.bin', '/www'):
            self.assertEqual(client.get(route).status_code, 404)


if __name__ == '__main__':
    unittest.main()
