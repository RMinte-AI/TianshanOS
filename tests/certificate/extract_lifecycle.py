"""Compile production lifecycle without unrelated request handlers and SDK transport internals."""
from pathlib import Path
import sys
s=Path('components/ts_https/src/ts_https.c').read_text()
end=s.index('esp_err_t ts_https_register_endpoint(')
Path(sys.argv[1]).write_text(s[:end])
s=Path('main/ts_services.c').read_text()
start=s.index('/* TLS lifecycle has exactly one owner.')
end=s.index('/* ============================================================================\n * Console', start)
Path(sys.argv[1]).with_name('coordinator.inc').write_text(s[start:end])
s=Path('components/ts_api/src/ts_api_cert.c').read_text()
# Extract helper functions and exactly three unchanged handlers; no alternate implementations.
pieces=[s[s.index('static const char *status_to_display'):s.index('/*===========================================================================*/\n/*                          API Handlers')]]
for name in ['api_cert_status','api_cert_install','api_cert_install_ca']:
    start=s.index('static esp_err_t '+name+'(')
    end=s.index('\n}\n',start)+3
    pieces.append(s[start:end])
Path(sys.argv[1]).with_name('api.inc').write_text('\n'.join(pieces))
def function(source, declaration):
    start=source.index(declaration)
    end=source.index('\n}\n',start)+3
    return source[start:end]
s=Path('components/ts_net/src/ts_time_sync.c').read_text()
pieces=[function(s,d) for d in ['static void notify_clock_updated(', 'static void time_sync_notification_cb(', 'esp_err_t ts_time_sync_set_time(', 'bool ts_time_sync_needs_sync(']]
s=Path('components/ts_pki_client/src/ts_pki_client.c').read_text()
pieces.append(function(s,'void ts_pki_client_stop_auto_enroll('))
Path(sys.argv[1]).with_name('time_cancel.inc').write_text('\n'.join(pieces))
