const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {harness} = require('./harness.cjs');

const deferred = () => {
    let resolve, reject;
    const promise = new Promise((done, fail) => { resolve = done; reject = fail; });
    return {promise, resolve, reject};
};
const answer = (data, status = 200) => ({ok: status === 200, status, json: async () => data});

async function device(language = 'zh-CN', includeWww = true) {
    const h = harness(language);
    await h.ready();
    if (process.env.OTA_WEB_ROOT) {
        for (const name of ['js/lang/zh-CN.js', 'js/lang/en-US.js', 'js/api.js', 'js/terminal.js', 'js/app.js']) {
            h.run(fs.readFileSync(path.join(process.env.OTA_WEB_ROOT, name), 'utf8'));
        }
    } else h.load();
    for (const id of ['ota-progress-section', 'ota-state-text', 'ota-progress-bar',
        'ota-progress-percent', 'ota-progress-size', 'ota-message', 'ota-abort-btn',
        'ota-url-include-www', 'ota-server-input', 'ota-url-input', 'ota-url-skip-verify',
        'ota-file-input', 'ota-file-include-www', 'ota-manual', 'reboot-countdown']) h.el(id);
    h.el('ota-url-include-www').checked = includeWww;
    h.el('ota-file-include-www').checked = includeWww;
    h.el('ota-server-input').value = 'https://ota.fixture.invalid';
    h.el('ota-url-input').value = 'https://ota.fixture.invalid/firmware.bin';
    h.el('ota-file-input').value = '/sdcard/update/TianShanOS.bin';
    const calls = [], toasts = [];
    const state = {
        app: 'idle', www: 'idle', firmwareWritten: false, wwwWritten: false,
        wwwBusy: false, wwwUpload: 'success', wwwStart: 0,
        running: {label: 'ota_0', address: 0x20000, size: 0x340000, version: '0.6.1+old', project: 'TianShanOS', is_bootable: true},
        next: {label: 'ota_1', address: 0x360000, size: 0x340000, version: '0.6.2+target', project: 'TianShanOS', is_bootable: true}
    };
    h.ctx.recordToast = (text, type) => toasts.push({text, type});
    h.run('showToast = recordToast');
    h.ctx.location.reload = () => calls.push({endpoint: 'page.reload'});
    const apiCall = async (endpoint, params) => {
        calls.push({endpoint, params});
        if (endpoint === 'ota.progress') return {code: 0, data: {state: state.app, percent: 100, received_size: 16, total_size: 16}};
        if (endpoint === 'ota.www.progress') return {code: 0, data: {state: state.www, running: state.wwwBusy, percent: 100, received: 16, total: 16}};
        if (endpoint === 'ota.www.start' || endpoint === 'ota.www.start_sdcard') {
            if (state.wwwStart) return {code: state.wwwStart, message: 'WWW start refused'};
            state.wwwBusy = true;
            state.www = 'writing';
            return {code: 0, data: {status: 'started'}};
        }
        if (endpoint === 'ota.upgrade_url' || endpoint === 'ota.upgrade_file') {
            state.app = 'writing';
            return {code: 0, data: {status: 'started'}};
        }
        if (endpoint === 'ota.partitions') {
            if (state.partitionsError) throw state.partitionsError;
            return {code: 0, data: {running: {...state.running}, next: {...state.next}}};
        }
        if (endpoint === 'ota.version') return {code: 0, data: {version: state.running.version}};
        if (endpoint === 'system.reboot') {
            if (state.rebootError) throw state.rebootError;
            return state.rebootResult || {code: 0, data: {status: 'rebooting'}};
        }
        if (endpoint === 'storage.info') return {code: 0, data: {type: 'file'}};
        if (endpoint === 'ota.abort' || endpoint === 'ota.www.abort') return {code: 0, data: {}};
        if (endpoint === 'ota.server.get') return {code: 0, data: {url: 'https://ota.fixture.invalid'}};
        throw new Error(`Unexpected API ${endpoint}`);
    };
    h.ctx.apiCall = apiCall;
    h.run('api.call = apiCall');
    const fetch = async url => {
        calls.push({endpoint: url});
        if (url.startsWith('https://ota.fixture.invalid/')) {
            if (url.endsWith('/www.bin') && state.wwwDownloadGate) {
                state.wwwDownloadReached.resolve();
                await state.wwwDownloadGate.promise;
            }
            let sent = false;
            return {ok: true, headers: {get: () => '16'}, body: {getReader: () => ({read: async () => {
                if (sent) return {done: true};
                sent = true;
                return {done: false, value: new Uint8Array(16)};
            }})}};
        }
        if (url.includes('/ota/firmware?auto_reboot=false')) {
            state.firmwareWritten = true;
            state.app = 'pending_reboot';
            if (state.firmwareReplyGate) {
                state.firmwareUploadReached.resolve();
                await state.firmwareReplyGate.promise;
            }
            return answer({status: 'success'});
        }
        if (url.endsWith('/ota/www')) {
            if (state.wwwUpload === 'offline') throw new TypeError('Failed to fetch');
            if (state.wwwBusy || state.wwwUpload === 'refused') return answer({error: 'OTA already in progress'}, 409);
            state.wwwWritten = true;
            state.www = 'completed';
            return answer({status: 'success'});
        }
        throw new Error(`Unexpected download/upload ${url}`);
    };
    h.ctx.fetch = fetch;
    h.run('refreshInterval = setInterval(refreshOtaProgress, 1000)');
    return {h, state, calls, toasts, apiCall,
        count: endpoint => calls.filter(call => call.endpoint === endpoint).length,
        boot: () => { state.running = {...state.next}; }};
}

test('proxy owns both images while normal progress polling remains active', async () => {
    const d = await device();
    d.state.wwwDownloadGate = deferred();
    d.state.wwwDownloadReached = deferred();
    const upgrade = d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.state.wwwDownloadReached.promise;
    await d.h.advance(3000);
    assert.equal(d.count('ota.www.start'), 0, 'the device must not also download WWW');
    assert.equal(d.count('system.reboot'), 0, 'WWW is still downloading in the browser');
    d.state.wwwDownloadGate.resolve();
    await upgrade;
    assert.equal(d.state.wwwWritten, true);
    assert.equal(d.count('system.reboot'), 1);
});

test('a progress response requested before proxy OTA cannot advance the new upgrade', async () => {
    const d = await device();
    const reply = deferred();
    d.h.ctx.apiCall = async (endpoint, params) => endpoint === 'ota.progress' ? reply.promise : d.apiCall(endpoint, params);
    d.h.run('api.call = apiCall');
    const oldPoll = d.h.run('refreshOtaProgress()');
    d.state.wwwDownloadGate = deferred();
    d.state.wwwDownloadReached = deferred();
    const upgrade = d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.state.wwwDownloadReached.promise;
    reply.resolve({code: 0, data: {state: 'pending_reboot'}});
    await oldPoll;
    assert.equal(d.count('ota.www.start'), 0);
    assert.equal(d.count('system.reboot'), 0);
    d.state.wwwDownloadGate.resolve();
    await upgrade;
});

test('firmware-only proxy waits for the firmware write acknowledgement before rebooting', async () => {
    const d = await device('zh-CN', false);
    d.state.firmwareReplyGate = deferred();
    d.state.firmwareUploadReached = deferred();
    const upgrade = d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.state.firmwareUploadReached.promise;
    await d.h.advance(2000);
    assert.equal(d.count('system.reboot'), 0);
    d.state.firmwareReplyGate.resolve();
    await upgrade;
    assert.equal(d.count('system.reboot'), 1);
    assert.equal(d.count('/api/v1/ota/www'), 0);
});

for (const source of ['URL', 'SD card']) for (const includeWww of [true, false]) {
    test(`${source} upgrade preserves ${includeWww ? 'both images' : 'firmware-only'} sequencing`, async () => {
        const d = await device('zh-CN', includeWww);
        await d.h.run(source === 'URL' ? 'otaFromUrl()' : 'otaFromFile()');
        const start = d.calls.find(call => call.endpoint === (source === 'URL' ? 'ota.upgrade_url' : 'ota.upgrade_file'));
        assert.equal(start.params.no_reboot, true);
        assert.equal(d.count('system.reboot'), 0);
        d.state.firmwareWritten = true;
        d.state.app = 'pending_reboot';
        await d.h.run('refreshOtaProgress()');
        if (includeWww) {
            const wwwEndpoint = source === 'URL' ? 'ota.www.start' : 'ota.www.start_sdcard';
            assert.equal(d.count(wwwEndpoint), 1);
            const www = d.calls.find(call => call.endpoint === wwwEndpoint);
            assert.equal(www.params[source === 'URL' ? 'url' : 'file'], source === 'URL'
                ? 'https://ota.fixture.invalid/www.bin' : '/sdcard/update/www.bin');
            assert.equal(d.count('system.reboot'), 0);
            d.state.www = 'completed';
            d.state.wwwWritten = true;
            d.state.wwwBusy = false;
            await d.h.run('refreshOtaProgress()');
        } else assert.equal(d.count('ota.www.start') + d.count('ota.www.start_sdcard'), 0);
        assert.equal(d.count('system.reboot'), 1);
        d.boot();
        await d.h.advance(2000);
        assert(d.toasts.some(toast => toast.type === 'success' && toast.text.includes('0.6.2+target')));
    });
}

for (const language of ['zh-CN', 'en-US']) for (const failure of ['offline', 'refused']) {
    test(`${language}: ${failure} WWW upload retains the written-firmware outcome without rebooting`, async () => {
        const d = await device(language);
        d.state.wwwUpload = failure;
        await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
        assert.equal(d.state.firmwareWritten, true);
        assert.equal(d.count('system.reboot'), 0);
        const title = d.h.el('ota-state-text').textContent;
        assert(title.includes(language === 'zh-CN' ? '固件' : 'firmware'));
        if (failure === 'offline') assert(title.includes(language === 'zh-CN' ? '未确认' : 'unconfirmed'));
        d.state.wwwUpload = 'success';
        await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
        assert.equal(d.state.wwwWritten, true, 'the failed attempt must not leave the next attempt blocked');
        assert.equal(d.count('system.reboot'), 1);
    });
}

test('reboot confirmation accepts a fast reboot to the written target without observing a disconnect', async () => {
    const d = await device();
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    d.boot();
    await d.h.advance(2000);
    assert(d.toasts.some(toast => toast.type === 'success' && toast.text.includes('0.6.2+target')));
    assert.equal(d.h.run('rebootCheckInterval'), null);
});

test('same-version reflash must actually switch to the written OTA partition', async () => {
    const d = await device();
    d.state.next.version = d.state.running.version;
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.h.advance(2000);
    assert(!d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
    d.boot();
    await d.h.advance(2000);
    assert(d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
});

test('an old or rolled-back firmware never passes reboot confirmation and eventually stops polling', async () => {
    const d = await device();
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.h.advance(122000);
    assert(!d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
    assert.equal(d.h.run('rebootCheckInterval'), null);
    assert(d.h.el('ota-state-text').textContent.includes('未确认'));
});

test('the correct partition running the wrong build is rejected', async () => {
    const d = await device();
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    d.boot();
    d.state.running.version = '0.6.2+other-build';
    await d.h.advance(2000);
    assert(!d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
    assert(d.h.el('ota-state-text').textContent.includes('不符'));
});

test('unreadable boot target preserves the written-files outcome and does not issue a reboot', async () => {
    const d = await device();
    d.state.partitionsError = new Error('status request interrupted');
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    assert.equal(d.state.firmwareWritten, true);
    assert.equal(d.state.wwwWritten, true);
    assert.equal(d.count('system.reboot'), 0);
    assert(d.h.el('ota-state-text').textContent.includes('尚未请求重启'));
});

test('repeated SD-card WWW start failures keep the sibling path through the successful retry', async () => {
    const d = await device();
    await d.h.run('otaFromFile()');
    d.state.app = 'pending_reboot';
    d.state.wwwStart = 7;
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 0);
    await d.h.run('startWwwOta()');
    d.state.wwwStart = 0;
    await d.h.run('startWwwOta()');
    const starts = d.calls.filter(call => call.endpoint === 'ota.www.start_sdcard');
    assert.equal(starts.length, 3);
    assert.equal(d.count('ota.www.start'), 0);
    assert(starts.every(call => call.params.file === '/sdcard/update/www.bin'));
    assert.equal(d.count('system.reboot'), 0);
    d.state.www = 'completed';
    d.state.wwwBusy = false;
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 1);
});

for (const pathExists of [true, false]) {
    test(`leaving the OTA page during ${pathExists ? 'successful' : 'failed'} SD path lookup cannot acquire an upgrade lock`, async () => {
        const d = await device();
        const reply = deferred(), reached = deferred();
        d.h.ctx.router = {navigation: {isCurrent: () => true}};
        d.h.ctx.apiCall = async (endpoint, params) => {
            if (endpoint === 'storage.info') { reached.resolve(); return reply.promise; }
            return d.apiCall(endpoint, params);
        };
        d.h.run('api.call = apiCall');
        const start = d.h.run('otaFromFile()');
        await reached.promise;
        d.h.ctx.router.navigation = {isCurrent: () => true};
        d.h.el('ota-progress-section').remove();
        reply.resolve(pathExists ? {code: 0, data: {type: 'file'}} : {code: 2});
        await start;
        assert.equal(d.count('ota.upgrade_file'), 0);
        assert.equal(d.h.run('otaUpgrade'), null);
        assert.equal(d.h.doc.getElementById('toast')?.classList.contains('show') ?? false, false,
            'a disposed page must not report its path lookup');
        d.h.el('ota-progress-section');
        await d.h.run('otaFromUrl()');
        assert.equal(d.count('ota.upgrade_url'), 1, 'the next attempt must remain available');
    });
}

for (const source of ['URL', 'SD card']) for (const includeWww of [true, false]) {
    test(`${source} acknowledgement off the OTA page resumes ${includeWww ? 'both images' : 'firmware-only'} on return`, async () => {
        const d = await device('zh-CN', includeWww);
        const reply = deferred(), reached = deferred();
        const startEndpoint = source === 'URL' ? 'ota.upgrade_url' : 'ota.upgrade_file';
        d.h.ctx.router = {navigation: {isCurrent: () => true}};
        d.h.ctx.apiCall = async (endpoint, params) => {
            const result = await d.apiCall(endpoint, params);
            if (endpoint === startEndpoint) { reached.resolve(); await reply.promise; }
            return result;
        };
        d.h.run('api.call = apiCall');
        const start = d.h.run(source === 'URL' ? 'otaFromUrl()' : 'otaFromFile()');
        await reached.promise;
        d.h.ctx.router.navigation = {isCurrent: () => true};
        d.h.el('ota-state-text').remove();
        d.h.el('ota-progress-section').remove();
        reply.resolve();
        await start;
        assert.equal(d.h.run('otaUpgrade.phase'), 'app', 'a successful device start must stay active');
        assert.equal(d.count('system.reboot'), 0);
        assert(!d.toasts.some(toast => toast.type === 'error' || toast.type === 'warning'));
        d.h.ctx.router.navigation = {isCurrent: () => true};
        d.h.el('ota-state-text');
        d.h.el('ota-progress-section');
        d.state.app = 'pending_reboot';
        await d.h.run('refreshOtaProgress()');
        if (includeWww) {
            assert.equal(d.count(source === 'URL' ? 'ota.www.start' : 'ota.www.start_sdcard'), 1);
            assert.equal(d.count('system.reboot'), 0, 'WWW is still being written');
            d.state.www = 'completed';
            d.state.wwwBusy = false;
            await d.h.run('refreshOtaProgress()');
        } else assert.equal(d.count('ota.www.start') + d.count('ota.www.start_sdcard'), 0);
        assert.equal(d.count('system.reboot'), 1);
        d.boot();
        await d.h.advance(2000);
        assert(d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
    });
}

test('double clicks and another upgrade source cannot start a second active upgrade', async () => {
    const d = await device();
    d.state.wwwDownloadGate = deferred();
    d.state.wwwDownloadReached = deferred();
    const first = d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.state.wwwDownloadReached.promise;
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    await d.h.run('otaFromUrl()');
    assert.equal(d.count('https://ota.fixture.invalid/firmware'), 1);
    assert.equal(d.count('ota.upgrade_url'), 0);
    d.state.wwwDownloadGate.resolve();
    await first;
    assert.equal(d.count('system.reboot'), 1);
});

test('URL upgrade after a failed SD-card attempt does not reuse the SD-card WWW path', async () => {
    const d = await device();
    await d.h.run('otaFromFile()');
    d.state.app = 'pending_reboot';
    d.state.wwwStart = 7;
    await d.h.run('refreshOtaProgress()');
    d.state.wwwStart = 0;
    await d.h.run('otaFromUrl()');
    d.state.app = 'pending_reboot';
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('ota.www.start'), 1);
    assert.equal(d.count('ota.www.start_sdcard'), 1);
});

test('a lost reboot acknowledgement waits for the actual target and then succeeds', async () => {
    const d = await device();
    d.state.rebootError = d.h.run("new ApiOperationError({code: 10}, 'system.reboot', {kind: 'network', uncertain: true})");
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    assert.equal(d.count('system.reboot'), 1);
    assert(d.toasts.some(toast => toast.type === 'warning'));
    assert(!d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
    d.boot();
    await d.h.advance(2000);
    assert(d.toasts.some(toast => toast.text.includes('OTA 升级成功')));
});

test('an explicit reboot rejection reports the written files and stops verification', async () => {
    const d = await device();
    d.state.rebootResult = {code: 3, message: 'Permission denied'};
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    assert(d.h.el('ota-state-text').textContent.includes('拒绝'));
    assert.equal(d.h.run('rebootCheckInterval'), null);
    assert.equal(d.state.wwwWritten, true);
});

test('manual abort releases the upgrade and allows the next attempt', async () => {
    const d = await device();
    d.h.run('confirmAction = async () => true; refreshOtaInfo = async () => {}');
    await d.h.run('otaFromUrl()');
    await d.h.run('abortOta()');
    assert.equal(d.count('ota.abort'), 1);
    assert.equal(d.h.el('ota-progress-section').style.display, 'none');
    assert.equal(d.count('system.reboot'), 0);
    await d.h.run("upgradeViaProxy('https://ota.fixture.invalid')");
    assert.equal(d.count('system.reboot'), 1);
});

for (const includeWww of [true, false]) {
    test(`a delayed abort refresh cannot stop the next ${includeWww ? 'two-image' : 'firmware-only'} upgrade`, async () => {
        const d = await device('zh-CN', includeWww);
        const refresh = deferred(), reached = deferred();
        d.h.ctx.abortRefresh = () => { reached.resolve(); return refresh.promise; };
        d.h.run('confirmAction = async () => true; refreshOtaInfo = abortRefresh');
        await d.h.run('otaFromUrl()');
        const oldTimer = d.h.run('refreshInterval');
        const abort = d.h.run('abortOta()');
        await reached.promise;
        assert.equal(d.h.timers.has(oldTimer), false);
        await d.h.run('otaFromFile()');
        const nextTimer = d.h.run('refreshInterval');
        assert(d.h.timers.has(nextTimer));
        refresh.resolve();
        await abort;
        assert.equal(d.h.run('refreshInterval'), nextTimer);
        assert(d.h.timers.has(nextTimer));
        d.state.app = 'pending_reboot';
        await d.h.advance(1000);
        if (includeWww) {
            assert.equal(d.count('ota.www.start_sdcard'), 1);
            assert.equal(d.count('system.reboot'), 0);
            d.state.www = 'completed';
            d.state.wwwBusy = false;
            await d.h.advance(1000);
        } else assert.equal(d.count('ota.www.start_sdcard'), 0);
        assert.equal(d.count('system.reboot'), 1);
    });
}

test('abort without a local attempt still stops the existing page polling', async () => {
    const d = await device();
    d.h.run('confirmAction = async () => true; refreshOtaInfo = async () => {}; refreshInterval = setInterval(refreshOtaProgress, 1000)');
    const timer = d.h.run('refreshInterval');
    await d.h.run('abortOta()');
    assert.equal(d.count('ota.abort'), 1);
    assert.equal(d.h.timers.has(timer), false);
    assert.equal(d.h.run('refreshInterval'), null);
});

test('WWW completion cannot reboot while the WWW writer is still running', async () => {
    const d = await device();
    await d.h.run('otaFromUrl()');
    d.state.app = 'pending_reboot';
    await d.h.run('refreshOtaProgress()');
    d.state.www = 'completed';
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 0);
    d.state.wwwBusy = false;
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 1);
});

test('pending manual abort prevents polling from requesting a reboot', async () => {
    const d = await device('zh-CN', false);
    d.h.run('confirmAction = async () => true; refreshOtaInfo = async () => {}');
    await d.h.run('otaFromUrl()');
    const reply = deferred(), reached = deferred();
    d.h.ctx.apiCall = async (endpoint, params) => {
        if (endpoint === 'ota.abort') { reached.resolve(); return reply.promise; }
        return d.apiCall(endpoint, params);
    };
    d.h.run('api.call = apiCall');
    const abort = d.h.run('abortOta()');
    await reached.promise;
    d.state.app = 'pending_reboot';
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 0);
    reply.resolve({code: 0});
    await abort;
});

test('a rejected manual abort resumes the original upgrade', async () => {
    const d = await device('zh-CN', false);
    d.h.run('confirmAction = async () => true');
    await d.h.run('otaFromUrl()');
    d.h.ctx.apiCall = async (endpoint, params) => endpoint === 'ota.abort' ? {code: 7, message: 'Abort refused'} : d.apiCall(endpoint, params);
    d.h.run('api.call = apiCall');
    await d.h.run('abortOta()');
    d.state.app = 'pending_reboot';
    await d.h.run('refreshOtaProgress()');
    assert.equal(d.count('system.reboot'), 1);
});

test('a late WWW start error cannot overwrite an acknowledged manual abort', async () => {
    const d = await device();
    d.h.run('confirmAction = async () => true; refreshOtaInfo = async () => {}');
    await d.h.run('otaFromUrl()');
    const reply = deferred(), reached = deferred();
    d.h.ctx.apiCall = async (endpoint, params) => {
        if (endpoint === 'ota.www.start') { reached.resolve(); return reply.promise; }
        return d.apiCall(endpoint, params);
    };
    d.h.run('api.call = apiCall');
    d.state.app = 'pending_reboot';
    const poll = d.h.run('refreshOtaProgress()');
    await reached.promise;
    await d.h.run('abortOta()');
    const title = d.h.el('ota-state-text').textContent;
    reply.reject(d.h.run("new ApiOperationError({code: 10}, 'ota.www.start', {kind: 'network', uncertain: true})"));
    await poll;
    assert.equal(d.h.el('ota-state-text').textContent, title);
    assert.equal(d.count('system.reboot'), 0);
});
