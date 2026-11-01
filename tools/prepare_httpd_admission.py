#!/usr/bin/env python3
"""Build-local ESP-IDF HTTPD policy: never LRU-evict an upgraded WebSocket.

The SDK installation is read-only. Refuse source drift rather than silently
building firmware without the admission policy. Tested with ESP-IDF v5.5.2.
"""
import argparse
from pathlib import Path


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Unsupported ESP-IDF HTTPD source: admission anchor changed')
    return source.replace(old, new, 1)


def prepare(idf, output):
    src = Path(idf) / 'components/esp_http_server/src'
    sess = (src / 'httpd_sess.c').read_text()
    sess = replace_once(sess,
        'if (session->for_async_req == false) {',
        '''if (session->for_async_req == false && !session->lru_socket
#ifdef CONFIG_HTTPD_WS_SUPPORT
            && !session->ws_handshake_done
#endif
        ) {''')
    sess = replace_once(sess,
        'if (!context.session) {\n        return ESP_OK;\n    }\n    ESP_LOGD(TAG, LOG_FMT("Closing session with fd %d"), context.session->fd);',
        'if (!context.session) {\n        return ESP_ERR_NOT_FOUND;\n    }\n    ESP_LOGD(TAG, LOG_FMT("Closing session with fd %d"), context.session->fd);')
    main = (src / 'httpd_main.c').read_text()
    main = replace_once(main,
        'return httpd_sess_close_lru(hd);',
        '''esp_err_t ret = httpd_sess_close_lru(hd);
            if (ret == ESP_ERR_NOT_FOUND) {
                /* All sessions are protected or closing. Consume the pending
                 * connection and reject it; never spin on a readable listener
                 * or displace an established session. No plaintext is sent on
                 * a potentially TLS listener. Normal WS admission sends 1013
                 * while reserved HTTP capacity is available. */
                int rejected_fd = accept(listen_fd, NULL, NULL);
                if (rejected_fd >= 0) close(rejected_fd);
                return ESP_OK;
            }
            return ret;''')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for name, content in [('httpd_sess.c', sess), ('httpd_main.c', main)]:
        dest = output / name
        if not dest.exists() or dest.read_text() != content:
            dest.write_text(content)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--idf-dir', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    prepare(args.idf_dir, args.output)
