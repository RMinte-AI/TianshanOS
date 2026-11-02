#!/usr/bin/env python3
"""Compile the production USB display helper against its actual target enum."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'components/ts_drivers/include/ts_usb_mux.h').read_text()
source = (root / 'components/ts_console/commands/ts_cmd_device.c').read_text()
enum = re.search(r'typedef enum\s*\{[^}]+\}\s*ts_usb_mux_target_t;', header).group()
helper = re.search(r'static const char \*usb_target_display_name\([\s\S]+?\n\}', source).group()
program = '#include <assert.h>\n#include <string.h>\n' + enum + '\n' + helper + r'''
int main(void) {
    assert(!strcmp(usb_target_display_name(TS_USB_MUX_ESP32), "ESP32"));
    assert(!strcmp(usb_target_display_name(TS_USB_MUX_AGX), "Inference Computer"));
    assert(!strcmp(usb_target_display_name(TS_USB_MUX_LPMU), "Application Computer"));
    assert(!strcmp(usb_target_display_name(TS_USB_MUX_DISCONNECT), "Disconnected"));
    assert(!strcmp(usb_target_display_name((ts_usb_mux_target_t)99), "Unknown"));
}
'''
with tempfile.TemporaryDirectory(prefix='ts-device-names-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS production USB target display: configured targets, disconnected and unknown')
