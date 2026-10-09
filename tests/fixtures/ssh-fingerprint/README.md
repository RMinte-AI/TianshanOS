# SSH host-key confirmation: bytes on the wire

`wire-*.json` are the HTTP response bodies the firmware sends for `ssh.exec` when the target host is unknown
(code 1002) or its fingerprint changed (code 1001), plus a successful run and a plain failure. They are produced by
`tests/runtime/test_ssh_fingerprint_wire.py`, which runs the production `ssh.exec` handler and the production HTTP
response chain of `ts_webui_api.c` against synthetic SSH/known-hosts back ends, and fails if the committed files differ
from what the code now sends (`UPDATE_FIXTURES=1` regenerates them after an intentional change).

`tests/prompts/ssh-fingerprint.browser.test.cjs` replays the same bytes into the real Web UI, so the firmware's wire
format and the page's expectations are checked against one shared contract. Fingerprints and hosts are synthetic.
