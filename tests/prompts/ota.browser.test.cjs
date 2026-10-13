const {test} = require('node:test');
const assert = require('node:assert/strict');
const kit = require('./pagekit.cjs');

kit.install();
const deferred = () => {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return {promise, resolve};
};

async function otaPage(language, mode, run) {
    const target = {label: 'ota_1', address: 0x360000, size: 0x340000, version: '0.6.2+target', project: 'TianShanOS', is_bootable: true};
    const state = {app: 'idle', www: 'idle', wwwStart: 0, firmwareWritten: false, wwwWritten: false, wwwBusy: false,
        running: {...target, label: 'ota_0', address: 0x20000, version: '0.6.1+old'}};
    const respond = call => {
        switch (call.endpoint) {
            case 'ota/server/get': return {code: 0, data: {url: 'https://ota.fixture.invalid'}};
            case 'ota/version': return {code: 0, data: {version: state.running.version, project: 'TianShanOS', compile_date: 'Oct 1 2026', compile_time: '00:00:00'}};
            case 'ota/partitions': return {code: 0, data: {running: state.running, next: target}};
            case 'ota/test_connection': return {code: 0, data: {dns_ok: true, tcp_ok: true, http_ok: true}};
            case 'ota/progress': return {code: 0, data: {state: state.app, percent: 100}};
            case 'ota/www/start':
            case 'ota/www/start_sdcard':
                if (state.wwwStart) return {code: state.wwwStart, message: 'WWW start refused'};
                state.wwwBusy = true;
                state.www = 'writing';
                return {code: 0, data: {status: 'started'}};
            case 'ota/www/progress': return {code: 0, data: {state: state.www, running: state.wwwBusy, percent: 20}};
            case 'ota/upgrade_url':
            case 'ota/upgrade_file': state.app = 'writing'; return {code: 0, data: {status: 'started'}};
            case 'storage/info': return {code: 0, data: {type: 'file'}};
            case 'ota/abort': state.app = 'idle'; return {code: 0};
            case 'ota/firmware':
                state.firmwareWritten = true;
                state.app = 'pending_reboot';
                return {status: 'success'};
            case 'ota/www':
                if (mode === 'upload-drop') return kit.DROP;
                if (state.wwwBusy) return {status: 'error', error: 'OTA already in progress'};
                state.wwwWritten = true;
                return {status: 'success'};
            case 'system/reboot':
                if (mode !== 'old-firmware') state.running = {...target};
                if (mode === 'wrong-build') state.running.version = '0.6.2+other';
                return {code: 0, data: {status: 'rebooting'}};
        }
    };
    await kit.withPage(language, respond, async d => {
        const gate = deferred(), reached = deferred();
        await d.page.route('https://ota.fixture.invalid/**', async route => {
            const url = route.request().url();
            if (url.endsWith('/version')) {
                await route.fulfill({json: {version: target.version, compile_date: 'Oct 13 2026', compile_time: '00:00:00', size: 32}});
                return;
            }
            if (mode === 'download-paused' && url.endsWith('/www.bin')) {
                reached.resolve();
                await gate.promise;
            }
            await route.fulfill({body: Buffer.alloc(32), contentType: 'application/octet-stream', headers: {'Content-Length': '32'}});
        });
        await d.page.evaluate(() => {
            api.token = 'fixture'; api.username = 'root'; api.level = 2;
            closeLoginModal();
            router.navigate('/ota');
        });
        await d.page.waitForSelector('#ota-server-input');
        await d.page.locator('#ota-server-input').fill('https://ota.fixture.invalid');
        if (!mode.startsWith('manual-')) {
            await d.page.locator('[onclick="checkForUpdates()"]').click();
            await d.page.locator('[onclick="upgradeFromServer()"]').click();
        }
        try { await run({...d, state, gate, reached}); }
        finally { gate.resolve(); }
    });
}

async function manualInput(page, source, includeWww) {
    await page.waitForFunction(() => document.getElementById('ota-current-version')?.textContent.includes('0.6.'));
    await page.locator('#ota-manual').evaluate(section => { section.open = true; });
    const prefix = source === 'URL' ? 'ota-url' : 'ota-file';
    await page.locator(`#${prefix}-input`).fill(source === 'URL'
        ? 'https://ota.fixture.invalid/firmware.bin' : '/sdcard/update/TianShanOS.bin');
    await page.locator(`#${prefix}-include-www`).setChecked(includeWww);
}

async function leaveOtaPage(page) {
    await page.evaluate(() => {
        router.register('/ota-test-away', async () => {
            document.getElementById('page-content').innerHTML = '<div id="ota-test-away">Another page</div>';
        });
        router.navigate('/ota-test-away');
    });
    await page.locator('#ota-test-away').waitFor();
}

async function returnToOtaPage(page) {
    await page.evaluate(() => { router.navigate('/ota'); });
    await page.waitForFunction(() => document.getElementById('ota-current-version')?.textContent.includes('0.6.'));
}

async function expectIndependentProgress(page, abortVisible) {
    assert.equal(await page.locator('#ota-progress-section').isVisible(), true);
    assert.equal(await page.locator('#ota-abort-btn').isVisible(), abortVisible);
    assert.deepEqual(await page.locator('#ota-progress-section').evaluate(section => ({
        card: section.classList.contains('card'),
        parent: section.parentElement.className,
        previous: section.previousElementSibling.tagName,
        folded: !!section.closest('details'),
        title: section.querySelector('h2').textContent === t('ota.upgradeProgress')
    })), {card: true, parent: 'ota-col', previous: 'H1', folded: false, title: true});
    for (const width of [1440, 390]) {
        await page.setViewportSize({width, height: 1000});
        const card = await page.locator('#ota-progress-section').boundingBox();
        const bar = await page.locator('#ota-progress-section .bar').boundingBox();
        const fill = await page.locator('#ota-progress-bar').boundingBox();
        const percent = await page.locator('#ota-progress-percent').boundingBox();
        assert(card.x >= 0 && card.x + card.width <= width);
        assert(bar.width > 0 && bar.x >= card.x && bar.x + bar.width <= card.x + card.width);
        assert(fill.width >= 0 && fill.width <= bar.width);
        assert(percent.x >= card.x && percent.x + percent.width <= card.x + card.width);
    }
}

for (const language of ['zh-CN', 'en-US']) {
    test(`${language}: idle and check-update states keep the independent progress card hidden`, async () => {
        await otaPage(language, 'manual-idle', async ({page, calls}) => {
            assert.equal(await page.locator('.ota-col > .card').count(), 4);
            assert.equal(await page.locator('#ota-progress-section').isVisible(), false);
            await page.locator('[onclick="checkForUpdates()"]').click();
            await page.locator('[onclick="upgradeFromServer()"]').waitFor();
            assert.equal(await page.locator('#ota-progress-section').isVisible(), false);
            assert.equal(await page.locator('#ota-manual').evaluate(section => section.open), false);
            assert.equal(calls.filter(call => /ota\/(firmware|upgrade_url|upgrade_file)/.test(call.endpoint)).length, 0);
        });
    });

    for (const source of ['URL', 'SD card']) for (const includeWww of [true, false]) {
        test(`${language}: ${source} ${includeWww ? 'both images' : 'firmware-only'} progress stays visible with manual controls folded`, async () => {
            await otaPage(language, 'manual-progress-card', async ({page, calls, state}) => {
                await manualInput(page, source, includeWww);
                const start = source === 'URL' ? 'otaFromUrl()' : 'otaFromFile()';
                await page.locator(`[onclick="${start}"]`).click();
                await page.waitForFunction(() => otaUpgrade?.phase === 'app');
                await page.locator('#ota-manual > summary').click();
                await page.evaluate(() => refreshOtaProgress());
                assert.equal(await page.locator('#ota-manual').evaluate(section => section.open), false);
                await expectIndependentProgress(page, true);
                state.app = 'pending_reboot';
                await page.evaluate(() => refreshOtaProgress());
                if (includeWww) {
                    await expectIndependentProgress(page, true);
                    state.www = 'completed';
                    state.wwwBusy = false;
                    await page.evaluate(() => refreshOtaProgress());
                }
                await expectIndependentProgress(page, false);
                const wwwStarts = calls.filter(call => call.endpoint === 'ota/www/start' || call.endpoint === 'ota/www/start_sdcard');
                assert.equal(wwwStarts.length, includeWww ? 1 : 0);
                assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 1);
                assert.equal(await page.locator('#ota-manual').evaluate(section => section.open), false);
            });
        });
    }

    test(`${language}: repeated SD WWW retries preserve the source until the successful write`, async () => {
        await otaPage(language, 'manual-retry', async ({page, calls, state}) => {
            await manualInput(page, 'SD card', true);
            await page.evaluate(() => otaFromFile());
            state.app = 'pending_reboot';
            state.wwwStart = 7;
            await page.evaluate(() => refreshOtaProgress());
            const secondReply = page.waitForResponse(response => response.url().endsWith('/api/v1/ota/www/start_sdcard'));
            await page.locator('[onclick="startWwwOta()"]').click();
            await secondReply;
            await page.waitForFunction(() => otaUpgrade.phase === 'done');
            state.wwwStart = 0;
            const thirdReply = page.waitForResponse(response => /\/api\/v1\/ota\/www\/start(?:_sdcard)?$/.test(response.url()));
            await page.locator('[onclick="startWwwOta()"]').click();
            await thirdReply;
            await page.waitForFunction(() => otaUpgrade.phase === 'www');
            const starts = calls.filter(call => call.endpoint === 'ota/www/start_sdcard');
            assert.equal(starts.length, 3);
            assert(starts.every(call => call.params.file === '/sdcard/update/www.bin'));
            assert.equal(calls.filter(call => call.endpoint === 'ota/www/start').length, 0);
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
            state.www = 'completed';
            state.wwwBusy = false;
            await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('ui.allUpgradeComplete'));
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 1);
        });
    });

    test(`${language}: leaving during SD path lookup does not block the next upgrade`, async () => {
        await otaPage(language, 'manual-path-navigation', async ({page, calls, gate, reached}) => {
            await manualInput(page, 'SD card', true);
            await page.route('**/api/v1/storage/info', async route => {
                reached.resolve();
                await gate.promise;
                await route.fulfill({json: {code: 0, data: {type: 'file'}}});
            });
            await page.evaluate(() => { window.otaTestStart = otaFromFile(); });
            await reached.promise;
            await leaveOtaPage(page);
            gate.resolve();
            await page.evaluate(() => window.otaTestStart);
            assert.equal(calls.filter(call => call.endpoint === 'ota/upgrade_file').length, 0);
            assert.equal(await page.evaluate(() => otaUpgrade), null);
            await returnToOtaPage(page);
            await manualInput(page, 'URL', false);
            await page.evaluate(() => otaFromUrl());
            assert.equal(calls.filter(call => call.endpoint === 'ota/upgrade_url').length, 1);
            assert.equal(await page.evaluate(() => otaUpgrade.phase), 'app');
        });
    });

    for (const source of ['URL', 'SD card']) for (const includeWww of [true, false]) {
        test(`${language}: ${source} acknowledgement after navigation resumes ${includeWww ? 'both images' : 'firmware-only'}`, async () => {
            await otaPage(language, 'manual-ack-navigation', async ({page, calls, state, gate, reached}) => {
                await manualInput(page, source, includeWww);
                const startEndpoint = source === 'URL' ? 'ota/upgrade_url' : 'ota/upgrade_file';
                await page.route(`**/api/v1/${startEndpoint}`, async route => {
                    calls.push({endpoint: startEndpoint, params: route.request().postDataJSON()});
                    state.app = 'writing';
                    reached.resolve();
                    await gate.promise;
                    await route.fulfill({json: {code: 0, data: {status: 'started'}}});
                });
                await page.evaluate(source => {
                    window.otaTestStart = source === 'URL' ? otaFromUrl() : otaFromFile();
                }, source);
                await reached.promise;
                await leaveOtaPage(page);
                gate.resolve();
                await page.evaluate(() => window.otaTestStart);
                assert.equal(await page.evaluate(() => otaUpgrade.phase), 'app');
                assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
                state.app = 'pending_reboot';
                const wwwEndpoint = source === 'URL' ? 'ota/www/start' : 'ota/www/start_sdcard';
                const wwwReply = includeWww ? page.waitForResponse(response => response.url().endsWith(`/api/v1/${wwwEndpoint}`)) : null;
                await returnToOtaPage(page);
                if (includeWww) {
                    await wwwReply;
                    await page.waitForFunction(() => otaUpgrade.phase === 'www');
                    const starts = calls.filter(call => call.endpoint === wwwEndpoint);
                    assert.equal(starts.length, 1);
                    assert.equal(starts[0].params[source === 'URL' ? 'url' : 'file'], source === 'URL'
                        ? 'https://ota.fixture.invalid/www.bin' : '/sdcard/update/www.bin');
                    assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
                    state.www = 'completed';
                    state.wwwBusy = false;
                } else assert.equal(calls.filter(call => call.endpoint === 'ota/www/start' || call.endpoint === 'ota/www/start_sdcard').length, 0);
                await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('ui.allUpgradeComplete'));
                assert.equal(calls.filter(call => call.endpoint === startEndpoint).length, 1);
                assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 1);
            });
        });
    }

    test(`${language}: acknowledged manual abort hides progress and permits a new upgrade`, async () => {
        await otaPage(language, 'manual-abort', async ({page, calls}) => {
            await page.locator('#ota-manual').evaluate(section => { section.open = true; });
            await page.locator('#ota-url-input').fill('https://ota.fixture.invalid/firmware.bin');
            await page.locator('#ota-url-include-www').uncheck();
            await page.locator('[onclick="otaFromUrl()"]').click();
            await page.locator('#ota-abort-btn').click();
            await kit.confirmSheet(page);
            await page.locator('#ota-progress-section').waitFor({state: 'hidden'});
            assert.equal(calls.filter(call => call.endpoint === 'ota/abort').length, 1);
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
            await page.locator('[onclick="otaFromUrl()"]').click();
            await page.waitForFunction(() => document.getElementById('ota-abort-btn').style.display !== 'none');
            assert.equal(calls.filter(call => call.endpoint === 'ota/upgrade_url').length, 2);
        });
    });

    for (const includeWww of [true, false]) {
        test(`${language}: delayed abort refresh leaves the next SD ${includeWww ? 'both-image' : 'firmware-only'} polling active`, async () => {
            await otaPage(language, 'manual-delayed-abort', async ({page, calls, state, gate, reached}) => {
                await manualInput(page, 'URL', false);
                await page.locator('[onclick="otaFromUrl()"]').click();
                // Hold the real metadata request made after acknowledging abort.
                await page.route('**/api/v1/ota/server/get', async route => {
                    reached.resolve();
                    await gate.promise;
                    await route.fulfill({json: {code: 0, data: {url: 'https://ota.fixture.invalid'}}});
                });
                await page.evaluate(() => {
                    const originalAbort = abortOta;
                    abortOta = () => (window.otaTestAbort = originalAbort());
                });
                await page.locator('#ota-abort-btn').click();
                await kit.confirmSheet(page);
                await reached.promise;
                await page.waitForFunction(() => otaUpgrade.phase === 'done');
                await manualInput(page, 'SD card', includeWww);
                await page.locator('[onclick="otaFromFile()"]').click();
                await page.waitForFunction(() => otaUpgrade.phase === 'app' && refreshInterval !== null);
                const nextTimer = await page.evaluate(() => refreshInterval);
                gate.resolve();
                await page.evaluate(() => window.otaTestAbort);
                assert.equal(await page.evaluate(() => refreshInterval), nextTimer);
                const wwwReply = includeWww ? page.waitForResponse(response => response.url().endsWith('/api/v1/ota/www/start_sdcard')) : null;
                state.app = 'pending_reboot';
                if (includeWww) {
                    await wwwReply;
                    await page.waitForFunction(() => otaUpgrade.phase === 'www');
                    assert.equal(calls.filter(call => call.endpoint === 'ota/www/start_sdcard').length, 1);
                    assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
                    state.www = 'completed';
                    state.wwwBusy = false;
                }
                await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('ui.allUpgradeComplete'));
                assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 1);
                assert.equal(calls.filter(call => call.endpoint === 'ota/www/start_sdcard').length, includeWww ? 1 : 0);
            });
        });
    }

    test(`${language}: real OTA page keeps one owner while WWW download is slow`, async () => {
        await otaPage(language, 'download-paused', async ({page, calls, reached, gate, state}) => {
            await reached.promise;
            assert.equal(await page.locator('#ota-manual').evaluate(section => section.open), false);
            await expectIndependentProgress(page, false);
            await page.waitForTimeout(1500);
            assert.equal(calls.filter(call => call.endpoint === 'ota/www/start').length, 0);
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
            gate.resolve();
            await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('ui.allUpgradeComplete'));
            assert.equal(state.firmwareWritten, true);
            assert.equal(state.wwwWritten, true);
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 1);
            const request = calls.find(call => call.endpoint === 'system/reboot');
            assert.equal(request.params.delay, 1000);
        });
    });

    test(`${language}: a lost WWW upload answer shows partial, unconfirmed success without reboot`, async () => {
        await otaPage(language, 'upload-drop', async ({page, calls, state}) => {
            await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('failure.ops.ota.upgradewww.unknown'));
            assert.equal(state.firmwareWritten, true);
            assert.equal(state.wwwWritten, false);
            assert.equal(calls.filter(call => call.endpoint === 'system/reboot').length, 0);
            await kit.toastIs(page, 'warning');
        });
    });

    test(`${language}: a different build in the target partition is never announced as successful`, async () => {
        await otaPage(language, 'wrong-build', async ({page}) => {
            await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('promptRepair.otaRebootMismatch'));
            assert.match(await page.locator('#ota-message').textContent(), /0\.6\.2\+other/);
            assert.equal(await page.locator('#toast').getAttribute('class'), 'toast toast-error show rich');
        });
    });

    test(`${language}: readable old firmware times out with an actionable unconfirmed result`, async () => {
        await otaPage(language, 'old-firmware', async ({page}) => {
            await page.waitForSelector('#reboot-countdown');
            await page.evaluate(() => { rebootStartTime -= 121000; });
            await page.waitForFunction(() => document.getElementById('ota-state-text').textContent === t('promptRepair.otaRebootUnconfirmed'));
            assert.match(await page.locator('#ota-message').textContent(), /0\.6\.2\+target/);
            await page.locator('#ota-message [onclick="window.location.reload()"]', {strict: true}).waitFor();
            await kit.toastIs(page, 'warning');
        });
    });
}
