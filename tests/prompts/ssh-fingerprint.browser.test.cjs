// "Test connection" on the SSH hosts page, against the exact bytes the firmware puts on the wire.
//
// The device answers ssh.exec for an unknown / changed host with HTTP 200 and {code:1002|1001, error, data:{fingerprint…}}.
// tests/runtime/test_ssh_fingerprint_wire.py produces those bytes with the production handlers and the production HTTP
// response chain and keeps them in tests/fixtures/ssh-fingerprint/. Here they are replayed verbatim into the real page:
// real api.js request pipeline, real button, real confirmation sheet. Nothing replaces api.call.
const {test, before, after} = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http'), fs = require('node:fs'), path = require('node:path');
const {chromium} = require('playwright');
const {root} = require('./harness.cjs');

const fixtureDir = path.resolve(__dirname, '../fixtures/ssh-fingerprint');
const device = name => fs.readFileSync(path.join(fixtureDir, name), 'utf8');
const NEW_HOST = JSON.parse(device('wire-new-host.json'));
const CHANGED_HOST = JSON.parse(device('wire-changed-host.json'));
const host = {id: 'test@192.0.2.8', host: '192.0.2.8', port: 22, username: 'test', keyid: 'synthetic-key'};
const TEST_BUTTON = '#ssh-hosts-table-body button[onclick="testSshHostByIndex(0)"]';

let browser;
before(async () => { browser = await chromium.launch({channel: 'chrome', headless: true}); });
after(async () => { await browser?.close(); });

// respond(params, n) returns the raw response body for the n-th ssh.exec request.
async function withPage(language, respond, run) {
    const execRequests = [], errors = [];
    const server = http.createServer((req, res) => {
        const url = new URL(req.url, 'http://local');
        const raw = body => { res.statusCode = 200; res.setHeader('Content-Type', 'application/json'); res.end(body); };
        if (url.pathname.startsWith('/api/')) {
            if (url.pathname === '/api/v1/ssh/exec') {
                let body = '';
                req.setEncoding('utf8');
                req.on('data', part => body += part);
                req.on('end', () => { const params = JSON.parse(body); execRequests.push(params); raw(respond(params, execRequests.length)); });
                return;
            }
            if (url.pathname === '/api/v1/ssh/hosts/list') return raw(JSON.stringify({code: 0, data: {hosts: [host]}}));
            if (url.pathname === '/api/v1/hosts/list') return raw(JSON.stringify({code: 0, data: {hosts: []}}));
            if (url.pathname === '/api/v1/key/list') return raw(JSON.stringify({code: 0, data: {keys: []}}));
            return raw(JSON.stringify({code: 0, data: {}}));
        }
        const file = path.resolve(root, '.' + (url.pathname === '/' ? '/index.html' : url.pathname));
        if (!file.startsWith(root + '/')) { res.writeHead(403).end(); return; }
        fs.readFile(file, (error, data) => {
            if (error) { res.writeHead(404).end(); return; }
            res.setHeader('Content-Type', ({'.js': 'application/javascript', '.html': 'text/html', '.css': 'text/css', '.woff2': 'font/woff2', '.svg': 'image/svg+xml'})[path.extname(file)] || 'application/octet-stream');
            res.end(data);
        });
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const base = `http://127.0.0.1:${server.address().port}`;
    const context = await browser.newContext({locale: language});
    await context.addInitScript(language => {
        localStorage.setItem('ts_language', language);
        class Socket { static OPEN = 1; static CONNECTING = 0; constructor() { this.readyState = 0; } send() {} close() { this.readyState = 3; } }
        window.WebSocket = Socket;
    }, language);
    const page = await context.newPage();
    page.on('pageerror', error => errors.push(error.message));
    try {
        await page.goto(base);
        await page.waitForFunction(() => i18n.isReady());
        await page.evaluate(async () => { closeLoginModal(); await loadSecurityPage(); });
        await page.locator(TEST_BUTTON).waitFor();
        await run({page, execRequests});
        assert.deepEqual(errors, []);
    } finally {
        await context.close();
        await new Promise(resolve => server.close(resolve));
    }
}

const sheet = page => page.locator('.confirm-sheet');
const toastMessage = page => page.evaluate(() => { const toast = document.getElementById('toast'); return toast?.querySelector('.toast-msg')?.textContent ?? toast?.textContent ?? ''; });
const toastKind = (page, kind) => page.waitForFunction(kind => document.getElementById('toast')?.classList.contains('toast-' + kind), kind);
const settle = page => page.waitForTimeout(400);   // a background retry would show up as an extra request

for (const language of ['zh-CN', 'en-US']) {
    for (const [name, wire, shownKey, code, flags, dialog] of [
        ['new host', NEW_HOST, 'fingerprint', 1002, {trust_new: true, accept_changed: false},
            {title: 'ui.fingerprintTitle', primary: 'ui.trustConnect', focus: '1'}],
        ['changed fingerprint', CHANGED_HOST, 'current_fingerprint', 1001, {trust_new: false, accept_changed: true},
            {title: 'securityPage.mismatchTitle', primary: 'securityPage.updateHostKey', focus: '0'}],
    ]) {
        const fingerprint = wire.data?.[shownKey];
        const needData = () => assert.equal(typeof fingerprint, 'string',
            `fixture has no data.${shownKey}: the device must send the fingerprint together with code ${code}`);

        test(`${language}: ${name}: the confirmation sheet shows the fingerprint the device sent`, async () => {
            assert.equal(wire.code, code);
            needData();
            await withPage(language, () => JSON.stringify(wire), async ({page, execRequests}) => {
                await page.locator(TEST_BUTTON).click();
                await sheet(page).waitFor({timeout: 5000});
                const shown = await sheet(page).locator('.mono').textContent();
                assert(shown.includes(`${host.host}:${host.port}`), shown);
                assert(shown.includes(fingerprint), shown);
                assert.equal(execRequests.length, 1);
                assert.equal(execRequests[0].trust_new, false);          // nothing is trusted before the user decides
                assert.equal('confirmed_fingerprint' in execRequests[0], false);
                // The sheet says what is happening and what the buttons do.
                const text = await sheet(page).textContent();
                const label = key => page.evaluate(key => t(key), key);
                assert(text.includes(await label(dialog.title)), 'title');
                assert(text.includes(await label(dialog.primary)), 'confirm button');
                assert.equal(await page.evaluate(() => document.activeElement?.dataset?.r), dialog.focus);
                if (code === 1001) {
                    // A changed key is a security warning: both fingerprints are shown, the default button is Cancel,
                    // and Enter (which activates the focused button) therefore cancels instead of accepting.
                    assert(shown.includes(wire.data.stored_fingerprint), 'stored fingerprint');
                    assert(text.includes(await label('securityPage.hostKeyChangedWarning')), 'warning');
                    await page.keyboard.press('Enter');
                    await sheet(page).waitFor({state: 'detached', timeout: 5000});
                    await settle(page);
                    assert.equal(execRequests.length, 1);
                } else {
                    await sheet(page).locator('button[data-r="0"]').click();
                }
            });
        });

        test(`${language}: ${name}: cancelling trusts nothing and does not retry`, async () => {
            needData();
            await withPage(language, () => JSON.stringify(wire), async ({page, execRequests}) => {
                await page.locator(TEST_BUTTON).click();
                await sheet(page).waitFor({timeout: 5000});
                await sheet(page).locator('button[data-r="0"]').click();
                await toastKind(page, 'warning');
                await settle(page);
                assert.equal(await sheet(page).count(), 0);
                assert.equal(execRequests.length, 1);
                // "cancelled, not trusted, not connected" - not an error, and no plain English leak
                assert.equal(await page.locator('#toast').textContent(), await page.evaluate(h => t('failure.ops.ssh.connect.cancelled', {host: h}), host.host));
                assert.equal(await page.evaluate(() => document.getElementById('toast').classList.contains('toast-success')), false);
            });
        });

        test(`${language}: ${name}: confirming sends exactly the fingerprint the user was shown, once`, async () => {
            needData();
            await withPage(language, (_, n) => n === 1 ? JSON.stringify(wire) : device('wire-exec-ok.json'), async ({page, execRequests}) => {
                await page.locator(TEST_BUTTON).click();
                await sheet(page).waitFor({timeout: 5000});
                await sheet(page).locator('button[data-r="1"]').click();
                await toastKind(page, 'success');
                await settle(page);
                assert.equal(execRequests.length, 2);
                const [first, second] = execRequests;
                assert.equal(second.confirmed_fingerprint, fingerprint);
                assert.equal(second.trust_new, flags.trust_new);
                assert.equal(second.accept_changed, flags.accept_changed);
                for (const key of ['host', 'port', 'user', 'keyid', 'command']) assert.equal(second[key], first[key], key);
                const expected = await page.evaluate(id => t('toast.connectionSuccess', {host: id}), host.id);
                assert.equal(await page.locator('#toast').textContent(), expected);
            });
        });

        test(`${language}: ${name}: if the device presents a different key at confirmation, nothing is trusted and there is no loop`, async () => {
            needData();
            const other = JSON.parse(JSON.stringify(wire));
            other.data[shownKey] = 'SHA256:SYNTHETICanotherKEYpresentedAtConfirmation0000000';
            await withPage(language, (_, n) => JSON.stringify(n === 1 ? wire : other), async ({page, execRequests}) => {
                await page.locator(TEST_BUTTON).click();
                await sheet(page).waitFor({timeout: 5000});
                await sheet(page).locator('button[data-r="1"]').click();
                await toastKind(page, 'error');
                await settle(page);
                assert.equal(execRequests.length, 2);
                assert.equal(execRequests[1].confirmed_fingerprint, fingerprint);   // the old text, not the new key
                assert.equal(await sheet(page).count(), 0);
                assert.equal(await toastMessage(page), await page.evaluate(h => describeFailure({op: 'ssh.connect', params: {host: h}, token: 'ssh_fingerprint_mismatch'}).message, host.host));
                assert.equal(await page.evaluate(() => document.getElementById('toast').classList.contains('toast-success')), false);
            });
        });
    }
}

test(`the fixtures are real firmware output: failures carry data, plain failures do not`, () => {
    assert.equal(NEW_HOST.code, 1002);
    assert.equal(NEW_HOST.error, 'ssh_host_key_unknown');
    assert.equal(typeof NEW_HOST.data.fingerprint, 'string');
    assert.equal(CHANGED_HOST.code, 1001);
    assert.equal(typeof CHANGED_HOST.data.current_fingerprint, 'string');
    assert.equal(typeof CHANGED_HOST.data.stored_fingerprint, 'string');
    assert.notEqual(CHANGED_HOST.data.current_fingerprint, CHANGED_HOST.data.stored_fingerprint);
    assert.equal(JSON.parse(device('wire-exec-ok.json')).code, 0);
    assert.deepEqual(JSON.parse(device('wire-generic-failure.json')), {code: 1, error: "Missing 'command' parameter"});
});

// ---------------------------------------------------------------------------------------------------------------------
// "Test connection" when the connection itself fails: the sentence says where it stopped. Bytes: wire-ssh-*.json.
for (const language of ['zh-CN', 'en-US']) {
    const zh = language === 'zh-CN';
    for (const [file, expectedZh, expectedEn, stage] of [
        ['wire-ssh-auth-failed.json', '连接 192.0.2.8 失败：主机拒绝了用户“test”的登录。请检查用户名，以及密码或密钥是否正确。',
            'Failed to connect to 192.0.2.8: the host rejected the login for user “test”. Check the username and that the password or key is correct.', 'auth'],
        ['wire-ssh-port-refused.json', '连接 192.0.2.8 失败：对方拒绝了连接（端口 22）。请确认目标主机上的 SSH 服务已启动、端口号正确。',
            'Failed to connect to 192.0.2.8: the connection was refused (port 22). Make sure SSH is running on the host and the port number is right.', 'refused'],
        ['wire-ssh-dns-failed.json', '连接 192.0.2.8 失败：无法解析这个主机名。请检查主机地址的拼写，以及设备的 DNS 设置。',
            'Failed to connect to 192.0.2.8: the host name cannot be resolved. Check the spelling of the host address and the device\'s DNS settings.', 'dns'],
        ['wire-ssh-unreachable.json', '连接 192.0.2.8 失败：无法到达这台主机。请检查主机是否开机、网络是否连通。',
            'Failed to connect to 192.0.2.8: the host cannot be reached. Check that the host is on and the network is connected.', 'unreachable'],
        ['wire-ssh-timeout.json', '连接 192.0.2.8 失败：连接超时。请确认主机已开机、网络可达，且没有防火墙拦截。',
            'Failed to connect to 192.0.2.8: the connection timed out. Make sure the host is on, reachable, and not blocked by a firewall.', 'timeout'],
        ['wire-ssh-handshake-failed.json', '连接 192.0.2.8 失败：无法建立 SSH 加密连接。请确认这个端口运行的是 SSH 服务。',
            'Failed to connect to 192.0.2.8: an SSH session could not be set up. Make sure this port runs an SSH service.', 'handshake'],
        ['wire-ssh-connect-failed.json', '连接 192.0.2.8 失败：无法建立 SSH 连接。可在「终端」页打开「系统日志」查看这次操作的详细记录。',
            'Failed to connect to 192.0.2.8: the SSH connection could not be made. Open Terminal → System Logs to see the details of this operation.', 'connect'],
    ]) {
        test(`${language}: connection failure ${file.replace('wire-ssh-', '').replace('.json', '')}: the message says where it stopped, the details keep the device text`, async () => {
            await withPage(language, () => device(file), async ({page, execRequests}) => {
                await page.locator(TEST_BUTTON).click();
                await toastKind(page, 'error');
                await page.waitForFunction(text => document.querySelector('#toast .toast-msg')?.textContent === text, zh ? expectedZh : expectedEn);
                assert.equal(await sheet(page).count(), 0);          // a failed connection is not a fingerprint question
                assert.equal(execRequests.length, 1);
                await page.locator('#toast .toast-more').click();
                const details = await page.locator('#toast .toast-detail').textContent();
                assert(details.includes('ssh.exec') && details.includes(stage) && details.includes('SSH connection failed (code='), details);
                // the machine word and the English device text are only in the folded part
                const shown = await toastMessage(page);
                assert(!/ssh_[a-z_]+|SSH connection failed/.test(shown), shown);
            });
        });
    }

    test(`${language}: a device that sends no fingerprint (older firmware) is told apart from a failed connection`, async () => {
        await withPage(language, () => '{"code":1002,"error":"ssh_host_key_unknown"}', async ({page, execRequests}) => {
            await page.locator(TEST_BUTTON).click();
            await toastKind(page, 'error');
            await page.waitForFunction(text => document.querySelector('#toast .toast-msg')?.textContent === text,
                zh ? '连接 192.0.2.8 失败：设备没有提供它的指纹，无法在这里确认。请把固件和 WebUI 一起升级到同一版本。'
                   : 'Failed to connect to 192.0.2.8: the device did not provide its fingerprint, so it cannot be confirmed here. Upgrade the firmware and the WebUI to the same version.');
            assert.equal(await sheet(page).count(), 0);
            assert.equal(execRequests.length, 1);
        });
    });

    test(`${language}: confirming a different key later reports it in plain words`, async () => {
        const first = JSON.parse(device('wire-new-host.json'));
        const other = JSON.parse(device('wire-new-host.json'));
        other.data.fingerprint = 'SHA256:SYNTHETICanotherKEY000000000000000000000000000000';
        await withPage(language, (_, n) => JSON.stringify(n === 1 ? first : other), async ({page}) => {
            await page.locator(TEST_BUTTON).click();
            await sheet(page).waitFor({timeout: 5000});
            await sheet(page).locator('button[data-r="1"]').click();
            await toastKind(page, 'error');
            await page.waitForFunction(text => document.querySelector('#toast .toast-msg')?.textContent === text,
                zh ? '连接 192.0.2.8 失败：它出示的指纹和你刚才确认的不一样。为安全起见没有信任它；请核实后重新测试。'
                   : 'Failed to connect to 192.0.2.8: it presented a different fingerprint from the one you just confirmed. For safety it was not trusted; check it and test again.');
        });
    });
}
