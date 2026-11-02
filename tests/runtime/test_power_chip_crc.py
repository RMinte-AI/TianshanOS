#!/usr/bin/env python3
"""Compile the production UART decoder; verify captured CRC frames and rejection."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'components/ts_drivers/src/ts_power_monitor.c').read_text()
decoder = re.search(r'^static esp_err_t read_power_chip_packet\([^;]+?\)\s*\{.*?^}',
                    source, re.M | re.S)
assert decoder
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ts_power_monitor.h"
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
static struct { ts_power_monitor_config_t config; ts_power_monitor_stats_t stats; } s_pm;
static uint8_t received[4];
static esp_err_t uart_get_buffered_data_len(int port, size_t *length) {
    assert(port == 1); *length = sizeof(received); return ESP_OK;
}
static int uart_read_bytes(int port, void *buffer, size_t length, unsigned wait) {
    assert(port == 1 && length >= sizeof(received) && wait == 100);
    memcpy(buffer, received, sizeof(received)); return sizeof(received);
}
static uint32_t esp_log_timestamp(void) { return 12345; }
'''
code += decoder.group() + r'''
static void check(const uint8_t frame[4], bool valid, float voltage, float current) {
    memcpy(received, frame, 4);
    ts_power_chip_data_t data = {0};
    uint32_t errors = s_pm.stats.crc_errors;
    assert(read_power_chip_packet(&data) == ESP_OK);
    assert(data.valid == valid && data.crc_valid == valid);
    assert(s_pm.stats.crc_errors == errors + !valid);
    assert(!memcmp(data.raw_data, frame, 4) && data.timestamp == 12345);
    if (valid) {
        assert(data.voltage == voltage && data.current == current);
        assert(data.power == voltage * current);
    }
}
int main(void) {
    s_pm.config.power_chip_config.uart_num = 1;
    const uint8_t field[] = {0xFF, 0x1C, 0x32, 0x71}; // Captured device: 28 V, 5 A.
    const uint8_t reference[] = {0xFF, 0x0C, 0x14, 0x63}; // XSP16 protocol example: 12 V, 2 A.
    const uint8_t zero[] = {0xFF, 0x1C, 0x00, 0x73};
    check(field, true, 28, 5);
    check(reference, true, 12, 2);
    check(zero, true, 28, 0);
    // A sum-valid packet must not bypass the actual CRC rule.
    const uint8_t sum_only[] = {0xFF, 0x1C, 0x32, 0x4D};
    check(sum_only, false, 0, 0);
    // Every single-bit change in the payload or checksum must invalidate this frame.
    for (unsigned bit = 0; bit < 24; bit++) {
        uint8_t corrupt[4]; memcpy(corrupt, field, 4);
        corrupt[1 + bit / 8] ^= 1u << (bit % 8);
        check(corrupt, false, 0, 0);
    }
    check(field, true, 28, 5); // A bad packet does not prevent a subsequent valid reading.
    puts("PASS production UART decoder: captured/reference/zero CRC frames, sum-only rejection, 24 single-bit errors, recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='ts-power-crc-') as directory:
    build = Path(directory)
    (build / 'test.c').write_text(code)
    env = {**os.environ, 'DEVELOPER_DIR': os.environ.get('DEVELOPER_DIR', '/Library/Developer/CommandLineTools')}
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(root / 'tests/runtime/storage_stubs'),
                    '-I' + str(root / 'components/ts_drivers/include'), str(build / 'test.c'),
                    '-o', str(build / 'test')], check=True, env=env)
    subprocess.run([str(build / 'test')], check=True)
