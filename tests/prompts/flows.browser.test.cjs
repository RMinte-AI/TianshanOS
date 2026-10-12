// What the user reads when a button fails, flow by flow, in the real page and in both languages.
//
// Every scenario drives one real handler of the page against a fake device whose answers are either the exact bytes the
// production firmware sends (tests/fixtures/failure-wire/*, written by tests/runtime/test_failure_wire.py) or a hand-made
// answer for a situation the firmware does not produce (the device never answered, an old firmware text ...).
// What the page shows - the toast or the inline note, its colour, and the folded details - is compared with the "golden"
// sentences in tests/fixtures/failure-texts/<file>.json. Those files are the list of every message of these flows: review
// them like a document. After an intentional wording change:  UPDATE_FIXTURES=1 node --test tests/prompts/flows.browser.test.cjs
const {test, after} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const kit = require('./pagekit.cjs');
const {DROP} = kit;

kit.install();

const UPDATE = process.env.UPDATE_FIXTURES === '1';
const wire = name => fs.readFileSync(path.resolve(__dirname, '../fixtures/failure-wire', name), 'utf8');
const ok = (data = {}) => ({code: 0, data});
const CJK = /[㐀-鿿＀-￯　-〿]/;

// ---------------------------------------------------------------------------------------------------------------------
// What a message must never contain, whatever the flow.
function checkSentence(language, shown, where) {
    assert(shown && shown.trim(), `${where}: nothing was shown`);
    const text = shown.replace(/“[^”]*”/g, '“”');   // what is inside the quotes is the user's own name for something
    assert(!/\{\w+\}|undefined|null|NaN|\[object|说明暂时无法显示|is unavailable\./i.test(text), `${where}: unfilled or unavailable text: ${text}`);
    assert(!/\b[a-z]+_[a-z0-9_]+\b/.test(text), `${where}: a machine code reached the user: ${text}`);
    assert(!/code=|errno|ESP_/.test(text), `${where}: an internal word reached the user: ${text}`);
    if (language === 'zh-CN') {
        assert(CJK.test(text), `${where}: no Chinese: ${text}`);
        const latin = (text.replace(/[“”‘’]/g, '').replace(/\/[\w.\-/]+/g, '').match(/[A-Za-z]{2,}/g) || []).filter(word => !['SSH', 'SD', 'DNS', 'IP', 'WebUI', 'automation', 'json', 'ID', 'LED', 'REST', 'Webhook', 'QR', 'URL', 'WebSocket', 'Socket', 'IO', 'HTTP', 'API', 'OS', 'tscfg',
            'WiFi', 'NAT', 'OTA', 'AGX', 'LPMU', 'DHCP', 'USB', 'CA', 'PKI', 'CSR', 'JSON', 'UTF', 'AP', 'STA', 'www', 'bin'].includes(word));
        assert.deepEqual(latin, [], `${where}: English in the Chinese text: ${text}`);
    } else {
        assert(!CJK.test(text.replace(/[“”’]/g, '')), `${where}: Chinese in the English text: ${text}`);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
async function observeToast(page) {
    await page.waitForFunction(() => document.getElementById('toast')?.classList.contains('show') && /\S/.test(document.getElementById('toast').textContent));
    return page.evaluate(() => {
        const toast = document.getElementById('toast');
        const type = [...toast.classList].find(c => c.startsWith('toast-'))?.slice(6);
        const text = toast.querySelector('.toast-msg')?.textContent ?? toast.textContent;
        const details = toast.querySelector('.toast-detail')?.textContent ?? null;
        return {type, text, details: details && details.replace(/\d{1,2}:\d{2}:\d{2}/, '<time>')};
    });
}
async function observeInline(page, selector) {
    const element = page.locator(selector);
    await element.waitFor();
    return element.evaluate(node => ({type: 'inline', text: (node.querySelector('.note-msg') ?? node).textContent.trim(), details: node.querySelector('.note-detail')?.textContent.replace(/\d{1,2}:\d{2}:\d{2}/, '<time>') ?? null}));
}
// a text pane that keeps appending (command output): the last line is the latest message
async function observeLastLine(page, selector) {
    const element = page.locator(selector);
    await element.waitFor();
    return element.evaluate(node => ({type: 'line', text: node.textContent.split('\n').map(line => line.trim()).filter(Boolean).at(-1) ?? '', details: null}));
}
// A blocked delete explains itself in a dialog: read it, then close it the way a user would (the secondary button comes first).
async function observeSheet(page) {
    const sheet = page.locator('.confirm-sheet');
    await sheet.waitFor();
    const seen = await sheet.evaluate(node => {
        const body = node.querySelector('.t-body').cloneNode(true);
        const items = [...body.querySelectorAll('.grp .row')].map(row => [...row.children].map(cell => cell.textContent.trim()).join(' · '));   // the user's own rule names are listed under the sentence
        body.querySelectorAll('.grp').forEach(list => list.remove());
        return {type: 'sheet', title: node.querySelector('.st')?.textContent.trim() ?? '', text: body.textContent.trim().replace(/\s*\n\s*/g, ' / '), items,
            buttons: [...node.querySelectorAll('button[data-r]')].map(button => button.textContent.trim())};
    });
    await sheet.locator('button[data-r="0"], button[data-r="1"]').first().click();
    return seen;
}
async function observeField(page, id) {
    const message = page.locator(`#${id}`).locator('xpath=ancestor::*[contains(@class,"row") or contains(@class,"fl")][1]').locator('.fe-msg');
    await message.waitFor();
    return {type: 'field', text: (await message.textContent()).trim(), focused: await page.evaluate(id => document.activeElement?.id === id, id)};
}

const golden = new Map();   // file -> {scenario -> {language -> observation}}
const produced = new Map();
function goldenFile(file) { return path.resolve(__dirname, '../fixtures/failure-texts', file + '.json'); }
function expectedFor(file) {
    if (!golden.has(file)) golden.set(file, fs.existsSync(goldenFile(file)) ? JSON.parse(fs.readFileSync(goldenFile(file), 'utf8')) : {});
    return golden.get(file);
}
after(() => {
    if (!UPDATE) return;
    for (const [file, scenarios] of produced) {
        fs.mkdirSync(path.dirname(goldenFile(file)), {recursive: true});
        const sorted = Object.fromEntries(Object.entries(scenarios).sort(([a], [b]) => a.localeCompare(b)));
        fs.writeFileSync(goldenFile(file), JSON.stringify(sorted, null, 2) + '\n');
    }
});

// A scenario: id, setup(page, names) -> DOM the handler needs, act(page, names) -> runs the handler, device {endpoint: answer},
// show: 'toast' | {inline: selector} | {field: id}. `names` are the user's own names for the objects, in the language of the run.
const NAMES = {rule: ['夜间关灯', 'Night off'], action: ['重启 AGX', 'Restart AGX'], source: ['温度', 'Temperature']};
function scenarioTests(file, scenarios) {
    for (const language of ['zh-CN', 'en-US']) {
        for (const scenario of scenarios) {
            test(`${language}: ${file}: ${scenario.id}`, async () => {
                const respond = call => {
                    const answer = scenario.device?.[call.endpoint];
                    return typeof answer === 'function' ? answer(call) : answer;
                };
                const names = {rule: NAMES.rule[language === 'zh-CN' ? 0 : 1], action: NAMES.action[language === 'zh-CN' ? 0 : 1], source: NAMES.source[language === 'zh-CN' ? 0 : 1]};
                await kit.withPage(language, respond, async ({page, calls}) => {
                    await scenario.setup?.(page, names);
                    const show = scenario.show ?? 'toast';
                    const acting = Promise.resolve(scenario.act(page, names));
                    if (!show.sheet) await acting;   // a dialog keeps the handler waiting until the user answers: read it first
                    const seen = show === 'toast' ? await observeToast(page) : show.sheet ? await observeSheet(page) : show.inline ? await observeInline(page, show.inline) : show.last ? await observeLastLine(page, show.last) : await observeField(page, show.field);
                    if (show.sheet) await acting;
                    checkSentence(language, seen.text, scenario.id);
                    if (seen.title) checkSentence(language, seen.title, scenario.id + ' (title)');
                    // the folded details are for whoever looks into it: they carry the machine words the sentence must not
                    if (seen.details) assert(/(原因代号|Reason code)[:：]/.test(seen.details), `${scenario.id}: details without the reason code`);
                    if (scenario.verify) await scenario.verify({page, calls, seen, language});
                    if (UPDATE) {
                        const entry = produced.get(file) ?? produced.set(file, {}).get(file);
                        (entry[scenario.id] ??= {})[language] = seen;
                    } else {
                        const want = expectedFor(file)[scenario.id]?.[language];
                        assert(want, `${scenario.id} (${language}) has no golden text yet: UPDATE_FIXTURES=1 node --test tests/prompts/flows.browser.test.cjs`);
                        assert.deepEqual(seen, want, `${scenario.id} (${language})`);
                    }
                });
            });
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
const page = {
    rules: p => p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="rules-list"></div><div id="sources-list"></div><div id="actions-list"></div><div id="automation-status"></div>'); }),
    sureToDelete: p => p.evaluate(() => { confirmAction = async () => true; }),
};
const rulesList = (name = 'night-off') => ok({loaded: true, rules: [{id: name, name: 'Night off', enabled: true, manual_trigger: true, conditions_count: 1, actions_count: 1, trigger_count: 0}]});

// ---------------------------------------------------------------------------------------------------------------------
// The pages that were converted in bulk: sign-in, device, files, network, LED, fan, OTA, certificates, config packs.
// One scenario per kind of failure (a sentence from the device's own legacy text, one the page worked out by itself, a lost
// answer to a write, a batch, a dialog result box) so that the golden sentences below can be read like a document.
const dom = html => p => p.evaluate(html => document.body.insertAdjacentHTML('beforeend', html), html);
const sure = p => p.evaluate(() => { confirmAction = async () => true; });
scenarioTests('system', [
    // ---- sign in (the reason stays in the form)
    ...[['wrong-password', '{"code":3,"error":"Invalid username or password"}'], ['locked', '{"code":3,"error":"Account locked due to too many failed attempts"}'], ['no-answer', DROP]].map(([name, answer]) => ({
        id: 'login.' + name, show: {inline: '#login-error'}, device: {'auth/login': answer},
        act: p => p.evaluate(() => { document.getElementById('login-modal').classList.remove('hidden'); document.getElementById('username').value = 'root'; document.getElementById('password').value = 'wrong'; document.getElementById('login-form').requestSubmit(); })})),
    // ---- device
    {id: 'device.reboot.no-answer', setup: sure, device: {'system/reboot': DROP},
     act: async p => { await p.evaluate(() => confirmReboot()); await p.waitForFunction(() => document.getElementById('toast').classList.contains('toast-warning')); }},
    {id: 'service.restart.busy', act: p => p.evaluate(() => serviceAction('automation', 'restart')), device: {'service/restart': '{"code":4,"error":"busy"}'}},
    {id: 'service.stop.no-answer', act: p => p.evaluate(() => serviceAction('automation', 'stop')), device: {'service/stop': DROP}},
    {id: 'fan.setspeed.rejected', setup: p => p.evaluate(() => { refreshFans = async () => {}; }), act: p => p.evaluate(() => setFanSpeed(2, 60)), device: {'fan/set': '{"code":1,"error":"Invalid fan id"}'}},
    {id: 'led.brightness.device-not-found', act: p => p.evaluate(() => setBrightness('matrix', 50)), device: {'led/brightness': '{"code":2,"error":"Device not found"}'}},
    // ---- files
    {id: 'file.mount.failed', setup: p => p.evaluate(() => { refreshFilesPage = async () => {}; }), act: p => p.evaluate(() => mountSdCard()), device: {'storage/mount': '{"code":7,"error":"Failed to mount SD card"}'}},
    {id: 'file.delete.not-found', setup: async p => { await sure(p); await p.evaluate(() => { refreshFilesPage = async () => {}; }); }, act: p => p.evaluate(() => deleteFile('/sdcard/a.txt')), device: {'storage/delete': '{"code":2,"error":"File not found"}'}},
    {id: 'file.delete.batch-mixed', show: {inline: '#operation-summary'},
     setup: async p => { await sure(p); await p.evaluate(() => { refreshFilesPage = async () => {}; selectedFiles.clear(); selectedFiles.add('/sdcard/a.txt'); selectedFiles.add('/sdcard/b.txt'); selectedFiles.add('/sdcard'); }); },
     act: p => p.evaluate(() => batchDelete()),
     device: {'storage/delete': call => call.params.path === '/sdcard/a.txt' ? ok() : call.params.path === '/sdcard' ? '{"code":1,"error":"Cannot delete root directory"}' : DROP}},
    {id: 'file.createdir.no-answer', setup: async p => { await dom('<input id="new-folder-name" value="logs"><div id="newfolder-modal" class="hidden"></div>')(p); await p.evaluate(() => { currentFilePath = '/sdcard'; }); },
     act: p => p.evaluate(() => createNewFolder()), device: {'storage/mkdir': DROP}},
    // ---- network
    {id: 'wifi.scan.wrong-mode', setup: dom('<div id="wifi-scan-section" class="hidden"><div id="wifi-scan-results"></div></div>'), show: {inline: '#wifi-scan-results'},
     act: p => p.evaluate(() => showWifiScan()), device: {'network/wifi/scan': '{"code":8,"error":"WiFi scan requires STA or APSTA mode"}'}},
    {id: 'network.nat.no-upstream', setup: p => p.evaluate(() => { refreshNetworkPage = async () => {}; }), act: p => p.evaluate(() => toggleNat()),
     device: {'nat/status': ok({enabled: false}), 'nat/enable': '{"code":7,"error":"WiFi STA not connected"}'}},
    {id: 'system.hostname.rejected', setup: dom('<input id="hostname-input" value="bad name">'), act: p => p.evaluate(() => setHostname()), device: {'network/hostname': '{"code":1,"error":"Invalid hostname"}'}},
    // ---- OTA
    {id: 'ota.path.firmware-missing', setup: dom('<input id="ota-file-input" value="/sdcard/ota/TianShanOS.bin"><input type="checkbox" id="ota-file-include-www">'),
     act: p => p.evaluate(() => otaFromFile()), device: {'storage/info': '{"code":2,"error":"File not found"}'}},
    {id: 'ota.path.nothing-entered', setup: dom('<input id="ota-file-input" value="/sdcard/ota"><input type="checkbox" id="ota-file-include-www">'),
     act: p => p.evaluate(() => otaFromFile()), device: {'storage/list': ok({entries: [{name: 'notes.txt', type: 'file'}]})}},
    {id: 'ota.check.server-unreachable', setup: dom('<input id="ota-server-input" value="http://127.0.0.1:9"><div id="ota-update-status"></div>'), show: {inline: '#ota-update-status'},
     act: p => p.evaluate(() => checkForUpdates()), device: {'ota/test_connection': '{"code":1,"error":"x"}'}},
    {id: 'ota.upload.device-refused', act: p => p.evaluate(async () => { try { await uploadFirmwareToDevice(new ArrayBuffer(3), false); } catch (error) { reportFailure({ op: 'ota.upgrade', error, endpoint: 'ota.proxy', write: true }); } }),
     device: {'ota/firmware': '{"status":"error","message":"Not enough space"}'}},
    {id: 'ota.upload.no-answer', act: p => p.evaluate(async () => { try { await uploadFirmwareToDevice(new ArrayBuffer(3), false); } catch (error) { reportFailure({ op: 'ota.upgrade', error, endpoint: 'ota.proxy', write: true }); } }),
     device: {'ota/firmware': DROP}},
    // ---- certificates and config packs (the result boxes of their dialogs)
    {id: 'cert.install.no-keypair', show: {inline: '#cert-install-result'},
     setup: dom('<textarea id="cert-pem-input">-----BEGIN CERTIFICATE-----</textarea><div id="cert-install-result" class="hidden"></div><button id="cert-install-submit"></button>'),
     act: p => p.evaluate(() => installCertMaterial(false)), device: {'cert/install': '{"code":1,"error":"No private key exists. Generate key pair first."}'}},
    {id: 'cert.install.no-answer', show: {inline: '#cert-install-result'},
     setup: dom('<textarea id="cert-pem-input">-----BEGIN CERTIFICATE-----</textarea><div id="cert-install-result" class="hidden"></div><button id="cert-install-submit"></button>'),
     act: p => p.evaluate(() => installCertMaterial(false)), device: {'cert/install': DROP}},
    {id: 'pack.verify.signature-invalid', show: {inline: '#pack-import-result'},
     setup: dom('<textarea id="pack-import-content">{"x":1}</textarea><div id="pack-import-result" class="hidden"></div><div id="pack-import-preview" class="hidden"></div>'),
     act: p => p.evaluate(() => verifyConfigPack()), device: {'config/pack/verify': ok({valid: false, reason: 'pack_signature_invalid', result_message: 'Signature verification failed'})}},
]);

scenarioTests('automation', [
    // ---- the engine's four buttons
    {id: 'engine.start.ok', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': wire('wire-engine-start-ok.json')}},
    {id: 'engine.start.paused', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': wire('wire-engine-start-paused.json')}},
    {id: 'engine.start.unavailable', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': wire('wire-engine-start-unavailable.json')}},
    {id: 'engine.start.no-memory', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': wire('wire-engine-start-nomem.json')}},
    {id: 'engine.start.old-firmware-text', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': '{"code":7,"message":"Failed to start automation engine"}'}},
    {id: 'engine.start.no-answer', act: p => p.evaluate(() => automationControl('start')), device: {'automation/start': DROP}},
    {id: 'engine.stop.timeout', act: p => p.evaluate(() => automationControl('stop')), device: {'automation/stop': wire('wire-engine-stop-timeout.json')}},
    {id: 'engine.stop.no-answer', act: p => p.evaluate(() => automationControl('stop')), device: {'automation/stop': DROP}},
    {id: 'engine.pause.not-running', act: p => p.evaluate(() => automationControl('pause')), device: {'automation/pause': wire('wire-engine-pause-not-running.json')}},
    {id: 'engine.pause.already-paused', act: p => p.evaluate(() => automationControl('pause')), device: {'automation/pause': wire('wire-engine-pause-paused.json')}},
    {id: 'engine.reload.config-invalid', act: p => p.evaluate(() => automationControl('reload')), device: {'automation/reload': wire('wire-engine-reload-config-invalid.json')}},
    {id: 'engine.reload.unavailable', act: p => p.evaluate(() => automationControl('reload')), device: {'automation/reload': wire('wire-engine-reload-unavailable.json')}},
    {id: 'engine.status.unreadable', setup: page.rules, act: p => p.evaluate(() => refreshAutomationStatus()), device: {'automation/status': '{"code":7,"message":"Failed to get automation status"}'},
     show: {inline: '#automation-status'}},

    // ---- rules
    {id: 'rule.trigger.ok', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => triggerRule('night-off')), device: {'automation/rules/trigger': wire('wire-trigger-ok.json')}},
    {id: 'rule.trigger.engine-not-accepting', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => triggerRule('night-off')), device: {'automation/rules/trigger': wire('wire-trigger-engine-not-accepting.json')}},
    {id: 'rule.trigger.pending-delete', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => triggerRule('night-off')), device: {'automation/rules/trigger': wire('wire-trigger-rule-pending-delete.json')}},
    {id: 'rule.trigger.not-allowed', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => triggerRule('night-off')), device: {'automation/rules/trigger': wire('wire-trigger-manual-not-allowed.json')}},
    {id: 'rule.trigger.no-answer', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => triggerRule('night-off')), device: {'automation/rules/trigger': DROP}},
    {id: 'rule.delete.ok', setup: async (p, c) => { await page.rules(p); await page.sureToDelete(p); await p.evaluate(n => ruleNames.set('night-off', n), c.rule); },
     act: p => p.evaluate(() => deleteRule('night-off')),
     device: {'automation/rules/get': ok({revision: 7}), 'automation/rules/delete': wire('wire-rule-ok.json'), 'automation/rules/list': rulesList()}},
    {id: 'rule.delete.running', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => ruleNames.set('night-off', n), c.rule); },
     act: p => p.evaluate(() => deleteRule('night-off')),
     device: {'automation/rules/get': ok({revision: 7}), 'automation/rules/delete': wire('wire-rule-execution-busy.json')}},
    {id: 'rule.delete.no-answer', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => ruleNames.set('night-off', n), c.rule); },
     act: p => p.evaluate(() => deleteRule('night-off')),
     device: {'automation/rules/get': ok({revision: 7}), 'automation/rules/delete': DROP}},
    {id: 'rule.load.not-found', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => editRule('night-off')),
     device: {'automation/rules/get': '{"code":2,"message":"Rule not found"}'}},
    {id: 'rule.load.no-answer', setup: (p, c) => p.evaluate(n => ruleNames.set('night-off', n), c.rule), act: p => p.evaluate(() => editRule('night-off')), device: {'automation/rules/get': DROP}},
    {id: 'rule.list.no-answer', setup: page.rules, act: p => p.evaluate(() => refreshRules()), device: {'automation/rules/list': DROP}, show: {inline: '#rules-list'}},
    {id: 'rule.list.refused', setup: page.rules, act: p => p.evaluate(() => refreshRules()), device: {'automation/rules/list': '{"code":7,"message":"Failed to list rules"}'}, show: {inline: '#rules-list'}},
    {id: 'rule.add-action.no-templates', setup: async p => { await p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="actions-container"></div>'); }); },
     act: p => p.evaluate(() => addActionTemplateRow()), device: {'automation/actions/list': ok({templates: []})}},
    {id: 'rule.add-action.list-unreadable', setup: async p => { await p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="actions-container"></div>'); }); },
     act: p => p.evaluate(() => addActionTemplateRow()), device: {'automation/actions/list': DROP}},

    // ---- action templates
    {id: 'action.save.ok', setup: async (p, c) => { await p.evaluate(n => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-name').value = n; document.getElementById('action-cli-command').value = 'device --reset agx0'; }, c.action); },
     act: p => p.evaluate(() => submitAction()), device: {'automation/actions/add': '{"code":0,"message":"Action template created"}'}},
    {id: 'action.save.exists', setup: async (p, c) => { await p.evaluate(n => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-name').value = n; document.getElementById('action-cli-command').value = 'device --reset agx0'; }, c.action); },
     act: p => p.evaluate(() => submitAction()), device: {'automation/actions/add': wire('wire-action-save-exists.json')}},
    {id: 'action.save.capacity', setup: async p => { await p.evaluate(() => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-cli-command').value = 'device --reset agx0'; }); },
     act: p => p.evaluate(() => submitAction()), device: {'automation/actions/add': wire('wire-action-save-capacity.json')}},
    {id: 'action.save.protected', setup: async (p, c) => { await p.evaluate(n => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-name').value = n; document.getElementById('action-cli-command').value = 'device --reset agx0'; }, c.action); },
     act: p => p.evaluate(() => submitAction('restart-agx')), device: {'automation/actions/update': wire('wire-action-save-protected.json')}},
    {id: 'action.save.generic', setup: async (p, c) => { await p.evaluate(n => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-name').value = n; document.getElementById('action-cli-command').value = 'device --reset agx0'; }, c.action); },
     act: p => p.evaluate(() => submitAction()), device: {'automation/actions/add': wire('wire-action-save-generic.json')}},
    {id: 'action.save.no-answer', setup: async (p, c) => { await p.evaluate(n => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; document.getElementById('action-name').value = n; document.getElementById('action-cli-command').value = 'device --reset agx0'; }, c.action); },
     act: p => p.evaluate(() => submitAction()), device: {'automation/actions/add': DROP}},
    {id: 'action.save.missing-id', setup: p => p.evaluate(() => { showAddActionModal(); }), act: p => p.evaluate(() => submitAction()), show: {field: 'action-id'}},
    {id: 'action.save.missing-command', setup: p => p.evaluate(() => { showAddActionModal(); document.getElementById('action-id').value = 'restart-agx'; }), act: p => p.evaluate(() => submitAction()), show: {field: 'action-cli-command'}},
    {id: 'action.test.ok', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': '{"code":0,"message":"Action executed successfully","data":{"async":false,"duration_ms":12,"output":"0"}}'}},
    {id: 'action.test.failed', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': wire('wire-action-run-failed.json')}},
    {id: 'action.test.failed-without-output', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': wire('wire-action-run-failed-no-output.json')}},
    {id: 'action.test.timeout', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': wire('wire-action-run-timeout.json')}},
    {id: 'action.test.not-found', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': wire('wire-action-run-not-found.json')}},
    {id: 'action.test.no-answer', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => testAction('restart-agx')),
     device: {'automation/actions/execute': DROP}},
    {id: 'action.load.not-found', setup: (p, c) => p.evaluate(n => actionNames.set('restart-agx', n), c.action), act: p => p.evaluate(() => editAction('restart-agx')),
     device: {'automation/actions/get': '{"code":2,"message":"Template not found"}'}},
    {id: 'action.delete.ok', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => actionNames.set('restart-agx', n), c.action); },
     act: p => p.evaluate(() => deleteAction('restart-agx')),
     device: {'automation/actions/get': ok({id: 'restart-agx', type: 'cli'}), 'automation/actions/delete': '{"code":0,"message":"Action template deleted"}'}},
    {id: 'action.delete.protected', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => actionNames.set('restart-agx', n), c.action); },
     act: p => p.evaluate(() => deleteAction('restart-agx')),
     device: {'automation/actions/get': ok({id: 'restart-agx', type: 'cli'}), 'automation/actions/delete': '{"code":4,"message":"service_delete_protected"}'}, show: {sheet: true}},
    // the explanation dialog of a blocked delete (the device names the rules that still use the template, or cannot check)
    ...[['in-use', '{"code":4,"error":"action_in_use","data":{"details_available":true,"rules":[{"id":"night-off","name":"Night off"},{"id":"morning","name":"Morning"}]}}'],
        ['in-use-no-details', '{"code":4,"error":"action_in_use","data":{"details_available":false}}'],
        ['check-busy', '{"code":4,"error":"action_reference_check_unavailable","data":{"check_reason":"busy"}}'],
        ['check-loading', '{"code":4,"error":"action_reference_check_unavailable","data":{"check_reason":"loading"}}'],
        ['check-recovery', '{"code":4,"error":"action_reference_check_unavailable","data":{"check_reason":"recovery"}}'],
        ['check-failed', '{"code":4,"error":"action_reference_check_unavailable","data":{"check_reason":"other"}}']].map(([name, answer]) => ({
        id: 'action.delete.' + name, setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => actionNames.set('restart-agx', n), c.action); },
        act: p => p.evaluate(() => deleteAction('restart-agx')), show: {sheet: true},
        device: {'automation/actions/get': ok({id: 'restart-agx', type: 'cli'}), 'automation/actions/delete': answer}})),
    {id: 'action.delete.no-answer', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => actionNames.set('restart-agx', n), c.action); },
     act: p => p.evaluate(() => deleteAction('restart-agx')),
     device: {'automation/actions/get': ok({id: 'restart-agx', type: 'cli'}), 'automation/actions/delete': DROP}},
    {id: 'action.delete.template-unreadable', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => actionNames.set('restart-agx', n), c.action); },
     act: p => p.evaluate(() => deleteAction('restart-agx')),
     device: {'automation/actions/get': '{"code":2,"message":"Template not found"}'}},
    {id: 'action.list.no-answer', setup: page.rules, act: p => p.evaluate(() => refreshActions()), device: {'automation/actions/list': DROP}, show: {inline: '#actions-list'}},
    {id: 'action.list.refused', setup: page.rules, act: p => p.evaluate(() => refreshActions()), device: {'automation/actions/list': '{"code":7,"message":"Failed to list templates"}'}, show: {inline: '#actions-list'}},

    // ---- data sources
    {id: 'source.enable.ok', setup: (p, c) => p.evaluate(n => sourceNames.set('temp', n), c.source), act: p => p.evaluate(() => toggleSource('temp', true)),
     device: {'automation/sources/enable': '{"code":0,"message":"Source enabled"}'}},
    {id: 'source.disable.not-found', setup: (p, c) => p.evaluate(n => sourceNames.set('temp', n), c.source), act: p => p.evaluate(() => toggleSource('temp', false)),
     device: {'automation/sources/disable': '{"code":2,"message":"Source not found"}'}},
    {id: 'source.enable.generic', setup: (p, c) => p.evaluate(n => sourceNames.set('temp', n), c.source), act: p => p.evaluate(() => toggleSource('temp', true)),
     device: {'automation/sources/enable': '{"code":7,"message":"Failed to enable source"}'}},
    {id: 'source.enable.no-answer', setup: (p, c) => p.evaluate(n => sourceNames.set('temp', n), c.source), act: p => p.evaluate(() => toggleSource('temp', true)),
     device: {'automation/sources/enable': DROP}},
    {id: 'source.delete.ok', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => sourceNames.set('temp', n), c.source); }, act: p => p.evaluate(() => deleteSource('temp')),
     device: {'automation/sources/delete': '{"code":0,"message":"Source deleted"}'}},
    {id: 'source.delete.not-found', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => sourceNames.set('temp', n), c.source); }, act: p => p.evaluate(() => deleteSource('temp')),
     device: {'automation/sources/delete': '{"code":2,"message":"Source not found"}'}},
    {id: 'source.delete.no-answer', setup: async (p, c) => { await page.sureToDelete(p); await p.evaluate(n => sourceNames.set('temp', n), c.source); }, act: p => p.evaluate(() => deleteSource('temp')),
     device: {'automation/sources/delete': DROP}},
    {id: 'source.add.ok', setup: (p, c) => p.evaluate(n => { showAddSourceModal(); document.getElementById('source-id').value = 'temp'; document.getElementById('source-label').value = n; document.getElementById('source-rest-url').value = 'http://192.0.2.1/api/status'; }, c.source),
     act: p => p.evaluate(() => submitAddSource()), device: {'automation/sources/add': wire('wire-source-add-ok.json')}},
    {id: 'source.add.capacity', setup: (p, c) => p.evaluate(n => { showAddSourceModal(); document.getElementById('source-id').value = 'temp'; document.getElementById('source-label').value = n; document.getElementById('source-rest-url').value = 'http://192.0.2.1/api/status'; }, c.source),
     act: p => p.evaluate(() => submitAddSource()), device: {'automation/sources/add': wire('wire-source-add-capacity.json')}},
    {id: 'source.add.unavailable', setup: (p, c) => p.evaluate(n => { showAddSourceModal(); document.getElementById('source-id').value = 'temp'; document.getElementById('source-label').value = n; document.getElementById('source-rest-url').value = 'http://192.0.2.1/api/status'; }, c.source),
     act: p => p.evaluate(() => submitAddSource()), device: {'automation/sources/add': wire('wire-source-add-unavailable.json')}},
    {id: 'source.add.no-answer', setup: (p, c) => p.evaluate(n => { showAddSourceModal(); document.getElementById('source-id').value = 'temp'; document.getElementById('source-label').value = n; document.getElementById('source-rest-url').value = 'http://192.0.2.1/api/status'; }, c.source),
     act: p => p.evaluate(() => submitAddSource()), device: {'automation/sources/add': DROP}},
    {id: 'source.list.no-answer', setup: page.rules, act: p => p.evaluate(() => refreshSources()), device: {'automation/sources/list': DROP}, show: {inline: '#sources-list'}},

    // ---- config packs: export, verify (the preview after choosing a file) and import
    {id: 'pack.export-source.not-allowed', setup: (p, c) => p.evaluate(n => { sourceNames.set('temp', n); showExportSourceModal('temp'); }, c.source), act: p => p.evaluate(() => doExportSource('temp')),
     device: {'automation/sources/export': wire('wire-pack-not-allowed.json')}, show: {inline: '#export-source-result'}},
    {id: 'pack.export-source.no-memory', setup: (p, c) => p.evaluate(n => { sourceNames.set('temp', n); showExportSourceModal('temp'); }, c.source), act: p => p.evaluate(() => doExportSource('temp')),
     device: {'automation/sources/export': '{"code":6,"message":"no_memory"}'}, show: {inline: '#export-source-result'}},
    {id: 'pack.export-source.invalid-answer', setup: (p, c) => p.evaluate(n => { sourceNames.set('temp', n); showExportSourceModal('temp'); }, c.source), act: p => p.evaluate(() => doExportSource('temp')),
     device: {'automation/sources/export': '{"code":0,"data":{}}'}, show: {inline: '#export-source-result'}},
    {id: 'pack.export-source.no-answer', setup: (p, c) => p.evaluate(n => { sourceNames.set('temp', n); showExportSourceModal('temp'); }, c.source), act: p => p.evaluate(() => doExportSource('temp')),
     device: {'automation/sources/export': DROP}, show: {inline: '#export-source-result'}},
    {id: 'pack.export-rule.cert-unavailable', setup: (p, c) => p.evaluate(n => { ruleNames.set('night-off', n); showExportRuleModal('night-off'); }, c.rule), act: p => p.evaluate(() => doExportRule('night-off')),
     device: {'automation/rules/export': '{"code":7,"message":"device_cert_unavailable","data":{"detail":"ESP_FAIL"}}'}, show: {inline: '#export-rule-result'}},
    {id: 'pack.export-action.not-found', setup: (p, c) => p.evaluate(n => { actionNames.set('restart-agx', n); showExportActionModal('restart-agx'); }, c.action), act: p => p.evaluate(() => doExportAction('restart-agx')),
     device: {'automation/actions/export': '{"code":2,"message":"Action template not found"}'}, show: {inline: '#export-action-result'}},
    {id: 'pack.verify-source.wrong-device', setup: p => p.evaluate(() => { showImportSourceModal(); const dt = new DataTransfer(); dt.items.add(new File(['{}'], 'source_temp.tscfg')); document.getElementById('import-source-file').files = dt.files; }),
     act: p => p.evaluate(() => previewSourceImport()), device: {'automation/sources/import': wire('wire-pack-wrong-device.json')}, show: {inline: '#import-source-result'}},
    {id: 'pack.verify-rule.signature', setup: p => p.evaluate(() => { showImportRuleModal(); const dt = new DataTransfer(); dt.items.add(new File(['{}'], 'rule_night.tscfg')); document.getElementById('import-rule-file').files = dt.files; }),
     act: p => p.evaluate(() => previewRuleImport()), device: {'automation/rules/import': wire('wire-pack-signature.json')}, show: {inline: '#import-rule-result'}},
    {id: 'pack.verify-action.not-a-pack', setup: p => p.evaluate(() => { showImportActionModal(); const dt = new DataTransfer(); dt.items.add(new File(['hello'], 'notes.txt')); document.getElementById('import-action-file').files = dt.files; }),
     act: p => p.evaluate(() => previewActionImport()), device: {'automation/actions/import': wire('wire-pack-parse.json')}, show: {inline: '#import-action-result'}},
    {id: 'pack.verify-source.no-answer', setup: p => p.evaluate(() => { showImportSourceModal(); const dt = new DataTransfer(); dt.items.add(new File(['{}'], 'source_temp.tscfg')); document.getElementById('import-source-file').files = dt.files; }),
     act: p => p.evaluate(() => previewSourceImport()), device: {'automation/sources/import': DROP}, show: {inline: '#import-source-result'}},
    {id: 'pack.import-source.ok', setup: p => p.evaluate(() => { showImportSourceModal(); window._importSourceTscfg = '{}'; window._importSourceFilename = 'source_temp.tscfg'; }),
     act: p => p.evaluate(() => confirmSourceImport()), device: {'automation/sources/import': '{"code":0,"data":{"id":"temp","imported":true}}'}},
    {id: 'pack.import-source.storage-failed', setup: p => p.evaluate(() => { showImportSourceModal(); window._importSourceTscfg = '{}'; window._importSourceFilename = 'source_temp.tscfg'; }),
     act: p => p.evaluate(() => confirmSourceImport()), device: {'automation/sources/import': '{"code":7,"message":"storage_failed","data":{"detail":"No such file or directory"}}'}, show: {inline: '#import-source-result'}},
    {id: 'pack.import-source.no-answer', setup: p => p.evaluate(() => { showImportSourceModal(); window._importSourceTscfg = '{}'; window._importSourceFilename = 'source_temp.tscfg'; }),
     act: p => p.evaluate(() => confirmSourceImport()), device: {'automation/sources/import': DROP}, show: {inline: '#import-source-result'}},
    // the rule pack: the preview decides what the import may do (revision, generation, credentials and digest come from it)
    {id: 'pack.import-rule.preview-required', setup: p => p.evaluate(() => { showImportRuleModal(); window._importRuleTscfg = '{}'; window._importRuleFilename = 'rule_night.tscfg'; }),
     act: p => p.evaluate(() => confirmRuleImport()), show: {inline: '#import-rule-result'}},
    ...[['revision-conflict', '{"code":4,"error":"revision_conflict"}'], ['overwrite-required', '{"code":1,"error":"overwrite_required"}'], ['restart-pending', '{"code":4,"error":"restart_pending"}'],
        ['dependency-missing', '{"code":1,"error":"dependency_missing","data":{"error_code":"dependency_missing"}}'], ['no-memory', '{"code":6,"error":"no_memory"}'],
        ['commit-unknown', '{"code":4,"error":"commit_unknown"}'], ['no-answer', DROP]].map(([name, answer]) => ({
        id: 'pack.import-rule.' + name, show: {inline: '#import-rule-result'}, device: {'automation/rules/import': answer},
        setup: p => p.evaluate(() => { showImportRuleModal(); window._importRuleTscfg = '{}'; window._importRuleFilename = 'rule_night.tscfg';
            rulePackPreview = {id: 'night-off', expected_revision: 4, expected_generation: 7, credential_generation: 4, package_digest: 'a'.repeat(64)}; }),
        act: p => p.evaluate(() => confirmRuleImport())})),
    {id: 'pack.verify-rule.untrusted', setup: p => p.evaluate(() => { showImportRuleModal(); const dt = new DataTransfer(); dt.items.add(new File(['{}'], 'rule_night.tscfg')); document.getElementById('import-rule-file').files = dt.files; }),
     act: p => p.evaluate(() => previewRuleImport()), device: {'automation/rules/import': ok({valid: true, trusted: false, target_matches: true})}, show: {inline: '#import-rule-result'}},
    {id: 'pack.verify-rule.wrong-device', setup: p => p.evaluate(() => { showImportRuleModal(); const dt = new DataTransfer(); dt.items.add(new File(['{}'], 'rule_night.tscfg')); document.getElementById('import-rule-file').files = dt.files; }),
     act: p => p.evaluate(() => previewRuleImport()), device: {'automation/rules/import': '{"code":1,"error":"wrong_device"}'}, show: {inline: '#import-rule-result'}},
    {id: 'pack.import-action.no-file', setup: p => p.evaluate(() => { showImportActionModal(); window._importActionTscfg = null; }), act: p => p.evaluate(() => confirmActionImport())},

    // ---- lists the rule / data source forms read
    {id: 'variable.list.refused', setup: p => p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="variable-select-loading"></div><div id="variable-select-empty"></div><div id="variable-select-list"></div>'); }),
     act: p => p.evaluate(() => loadVarSelectList()), device: {'automation/variables/list': '{"code":7,"message":"Failed to list variables"}'}, show: {inline: '#variable-select-loading'}},
    {id: 'variable.list.no-answer', setup: p => p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="variable-select-loading"></div><div id="variable-select-empty"></div><div id="variable-select-list"></div>'); }),
     act: p => p.evaluate(() => loadVarSelectList()), device: {'automation/variables/list': DROP}, show: {inline: '#variable-select-loading'}},
    {id: 'image.list.no-answer', act: p => p.evaluate(() => showImageSelectModal('x', () => {})), device: {'storage/list': DROP}, show: {inline: '#image-select-loading'}},
    {id: 'image.list.sd-missing', act: p => p.evaluate(() => showImageSelectModal('x', () => {})), device: {'storage/list': '{"code":2,"message":"SD card not mounted"}'}, show: {inline: '#image-select-loading'}},
    {id: 'host.list.refused', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('variable'); }), act: p => p.evaluate(() => loadSshHostsForSource()),
     device: {'ssh/hosts/list': '{"code":7,"message":"Failed to list hosts"}'}, show: {inline: '#source-ssh-host'}},

    // ---- testing a data source's address (the answer is shown next to the Test button)
    {id: 'source.test-rest.connect-failed', setup: p => p.evaluate(() => { showAddSourceModal(); document.getElementById('source-rest-url').value = 'http://192.0.2.9/api/status'; }), act: p => p.evaluate(() => testRestConnection()),
     device: {'automation/proxy/fetch': wire('wire-proxy-connect-failed.json')}, show: {inline: '#rest-test-result .test-status'}},
    {id: 'source.test-rest.http-error', setup: p => p.evaluate(() => { showAddSourceModal(); document.getElementById('source-rest-url').value = 'http://192.0.2.9/api/status'; }), act: p => p.evaluate(() => testRestConnection()),
     device: {'automation/proxy/fetch': wire('wire-proxy-http-error.json')}, show: {inline: '#rest-test-result .test-status'}},
    {id: 'source.test-rest.init-failed', setup: p => p.evaluate(() => { showAddSourceModal(); document.getElementById('source-rest-url').value = 'not an address'; }), act: p => p.evaluate(() => testRestConnection()),
     device: {'automation/proxy/fetch': '{"code":7,"message":"proxy_init_failed"}'}, show: {inline: '#rest-test-result .test-status'}},
    {id: 'source.test-rest.no-answer', setup: p => p.evaluate(() => { showAddSourceModal(); document.getElementById('source-rest-url').value = 'http://192.0.2.9/api/status'; }), act: p => p.evaluate(() => testRestConnection()),
     device: {'automation/proxy/fetch': DROP}, show: {inline: '#rest-test-result .test-status'}},
    {id: 'source.test-rest.missing-address', setup: p => p.evaluate(() => { showAddSourceModal(); }), act: p => p.evaluate(() => testRestConnection()), show: {field: 'source-rest-url'}},
    {id: 'source.test-ws.start-failed', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('websocket'); document.getElementById('source-ws-uri').value = 'ws://192.0.2.9:81/feed'; }), act: p => p.evaluate(() => testWsConnection()),
     device: {'automation/proxy/websocket_test': '{"code":10,"message":"ws_start_failed","data":{"detail":"ESP_ERR_INVALID_ARG"}}'}, show: {inline: '#ws-test-result .test-status'}},
    {id: 'source.test-ws.connect-failed', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('websocket'); document.getElementById('source-ws-uri').value = 'ws://192.0.2.9:81/feed'; }), act: p => p.evaluate(() => testWsConnection()),
     device: {'automation/proxy/websocket_test': '{"code":7,"message":"ws_connect_failed","data":{"connected":false,"error":"ws_connect_failed"}}'}, show: {inline: '#ws-test-result .test-status'}},
    {id: 'source.test-ws.timeout', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('websocket'); document.getElementById('source-ws-uri').value = 'ws://192.0.2.9:81/feed'; }), act: p => p.evaluate(() => testWsConnection()),
     device: {'automation/proxy/websocket_test': '{"code":7,"message":"ws_connect_timeout","data":{"connected":false,"error":"ws_connect_timeout"}}'}, show: {inline: '#ws-test-result .test-status'}},
    {id: 'source.test-ws.no-data', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('websocket'); document.getElementById('source-ws-uri').value = 'ws://192.0.2.9:81/feed'; }), act: p => p.evaluate(() => testWsConnection()),
     device: {'automation/proxy/websocket_test': '{"code":7,"message":"ws_no_data","data":{"connected":true,"error":"No data received within timeout"}}'}, show: {inline: '#ws-test-result .test-status'}},
    {id: 'source.test-sio.no-session', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('socketio'); document.getElementById('source-sio-url').value = 'http://192.0.2.9:3000'; }), act: p => p.evaluate(() => testSioConnection()),
     device: {'automation/proxy/socketio_test': '{"code":7,"message":"sio_no_session","data":{"connected":false,"error":"No session ID received"}}'}, show: {inline: '#sio-test-result .test-status'}},
    {id: 'source.test-sio.event-timeout', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('socketio'); document.getElementById('source-sio-url').value = 'http://192.0.2.9:3000'; document.getElementById('source-sio-event').value = 'stats'; }), act: p => p.evaluate(() => testSioConnection()),
     device: {'automation/proxy/socketio_test': '{"code":5,"message":"sio_event_timeout","data":{"connected":true,"sid":"abcdef123456"}}'}, show: {inline: '#sio-test-result .test-status'}},
    {id: 'source.test-sio.connect-timeout', setup: p => p.evaluate(() => { showAddSourceModal(); switchSourceType('socketio'); document.getElementById('source-sio-url').value = 'http://192.0.2.9:3000'; }), act: p => p.evaluate(() => testSioConnection()),
     device: {'automation/proxy/socketio_test': '{"code":7,"message":"ws_connect_timeout","data":{"connected":false,"error":"WebSocket connection failed"}}'}, show: {inline: '#sio-test-result .test-status'}},
]);

// ---------------------------------------------------------------------------------------------------------------------
// SSH: hosts, fingerprints and keys on the Security page
const wireSsh = name => fs.readFileSync(path.resolve(__dirname, '../fixtures/ssh-fingerprint', name), 'utf8');
const sshHost = {id: 'root@192.0.2.8', host: '192.0.2.8', port: 22, username: 'root', keyid: 'lab-key'};
const security = (overrides = {}) => ({
    'key/list': ok({keys: [{id: 'lab-key', type: 'rsa2048', bits: 2048}]}),
    'ssh/hosts/list': ok({hosts: [sshHost]}),
    'hosts/list': ok({hosts: [{host: '192.0.2.8', port: 22, type: 'ssh-ed25519', fingerprint: 'SHA256:abcdefghijklmnopqrstuvwxyz0123456789', added: 1700000000}]}),
    ...overrides
});
const onSecurity = p => p.evaluate(async () => { await loadSecurityPage(); });
const onSecurityConfirmed = async p => { await onSecurity(p); await page.sureToDelete(p); };
const fillDeploy = (p, fields = {}) => p.evaluate(f => { showDeployKeyModal('lab-key'); document.getElementById('deploy-host').value = f.host ?? '192.0.2.8'; document.getElementById('deploy-user').value = f.user ?? 'root'; document.getElementById('deploy-password').value = f.password ?? 'secret'; }, fields);
const fillRevoke = (p, fields = {}) => p.evaluate(f => { showRevokeKeyModal('lab-key'); document.getElementById('revoke-host').value = f.host ?? '192.0.2.8'; document.getElementById('revoke-user').value = f.user ?? 'root'; document.getElementById('revoke-password').value = f.password ?? 'secret'; }, fields);

scenarioTests('ssh', [
    // ---- the two lists
    {id: 'host.list.no-answer', setup: onSecurity, act: p => p.evaluate(() => refreshSshHostsList()), device: security({'ssh/hosts/list': DROP}), show: {inline: '#ssh-hosts-table-body'}},
    {id: 'host.list.refused', setup: onSecurity, act: p => p.evaluate(() => refreshSshHostsList()), device: security({'ssh/hosts/list': '{"code":7,"message":"Failed to list hosts"}'}), show: {inline: '#ssh-hosts-table-body'}},
    {id: 'known-host.list.refused', setup: onSecurity, act: p => p.evaluate(() => refreshKnownHostsList()), device: security({'hosts/list': '{"code":7,"message":"Failed to list hosts"}'}), show: {inline: '#known-hosts-table-body'}},
    {id: 'known-host.list.no-answer', setup: onSecurity, act: p => p.evaluate(() => refreshKnownHostsList()), device: security({'hosts/list': DROP}), show: {inline: '#known-hosts-table-body'}},
    // ---- fingerprint records
    {id: 'known-host.remove.ok', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeKnownHost(0)), device: security({'hosts/remove': '{"code":0,"message":"Host removed"}'})},
    {id: 'known-host.remove.not-found', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeKnownHost(0)), device: security({'hosts/remove': '{"code":2,"message":"Host not found"}'})},
    {id: 'known-host.remove.no-answer', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeKnownHost(0)), device: security({'hosts/remove': DROP})},
    // ---- host records
    {id: 'host.remove.ok', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeHostByIndex(0)), device: security({'ssh/hosts/remove': '{"code":0,"message":"Host removed"}'})},
    {id: 'host.remove.failed', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeHostByIndex(0)), device: security({'ssh/hosts/remove': '{"code":7,"message":"Failed to remove host"}'})},
    {id: 'host.remove.no-answer', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeHostByIndex(0)), device: security({'ssh/hosts/remove': DROP})},
    {id: 'host.remove.list-stale', setup: onSecurityConfirmed, act: p => p.evaluate(() => removeHostByIndex(7)), device: security()},
    // ---- revoke a key from a host (asks for the server's password)
    {id: 'host.revoke.wrong-password', setup: async p => { await onSecurity(p); await p.evaluate(() => { revokeKeyFromHost(0); document.getElementById('revoke-host-password').value = 'wrong'; }); },
     act: p => p.evaluate(() => doRevokeFromHost(0)), device: security({'ssh/revoke': wireSsh('wire-ssh-auth-failed.json')}), show: {inline: '#revoke-host-result'}},
    {id: 'host.revoke.key-changed', setup: async p => { await onSecurity(p); await p.evaluate(() => { revokeKeyFromHost(0); document.getElementById('revoke-host-password').value = 'secret'; }); },
     act: p => p.evaluate(() => doRevokeFromHost(0)), device: security({'ssh/revoke': wireSsh('wire-changed-host.json')}), show: {inline: '#revoke-host-result'}},
    {id: 'host.revoke.no-answer', setup: async p => { await onSecurity(p); await p.evaluate(() => { revokeKeyFromHost(0); document.getElementById('revoke-host-password').value = 'secret'; }); },
     act: p => p.evaluate(() => doRevokeFromHost(0)), device: security({'ssh/revoke': DROP}), show: {inline: '#revoke-host-result'}},
    {id: 'host.revoke.record-not-removed', setup: async p => { await onSecurity(p); await p.evaluate(() => { revokeKeyFromHost(0); document.getElementById('revoke-host-password').value = 'secret'; }); },
     act: p => p.evaluate(() => doRevokeFromHost(0)),
     device: security({'ssh/revoke': '{"code":0,"data":{"revoked":true,"removed_count":1}}', 'ssh/hosts/remove': '{"code":7,"message":"Failed to remove host"}'}), show: {inline: '#revoke-host-result'}},
    {id: 'host.revoke.missing-password', setup: async p => { await onSecurity(p); await p.evaluate(() => revokeKeyFromHost(0)); }, act: p => p.evaluate(() => doRevokeFromHost(0)), device: security(), show: {field: 'revoke-host-password'}},
    // ---- keys
    {id: 'key.delete.ok', setup: onSecurityConfirmed, act: p => p.evaluate(() => deleteKey('lab-key')), device: security({'key/delete': '{"code":0,"message":"Key deleted"}'})},
    {id: 'key.delete.not-found', setup: onSecurityConfirmed, act: p => p.evaluate(() => deleteKey('lab-key')), device: security({'key/delete': '{"code":2,"message":"key_not_found"}'})},
    {id: 'key.delete.no-answer', setup: onSecurityConfirmed, act: p => p.evaluate(() => deleteKey('lab-key')), device: security({'key/delete': DROP})},
    {id: 'key.export-public.not-found', setup: onSecurity, act: p => p.evaluate(() => exportKey('lab-key')), device: security({'key/export': '{"code":2,"message":"key_not_found"}'})},
    {id: 'key.export-public.invalid-answer', setup: onSecurity, act: p => p.evaluate(() => exportKey('lab-key')), device: security({'key/export': '{"code":0,"data":{}}'})},
    {id: 'key.export-private.no-answer', setup: onSecurityConfirmed, act: p => p.evaluate(() => exportPrivateKey('lab-key')), device: security({'key/exportPrivate': DROP})},
    // ---- deploy a key to a server
    {id: 'key.deploy.wrong-password', setup: async p => { await onSecurity(p); await fillDeploy(p); }, act: p => p.evaluate(() => deployKey()), device: security({'ssh/copyid': wireSsh('wire-ssh-auth-failed.json')}), show: {inline: '#deploy-result'}},
    {id: 'key.deploy.timeout', setup: async p => { await onSecurity(p); await fillDeploy(p); }, act: p => p.evaluate(() => deployKey()), device: security({'ssh/copyid': wireSsh('wire-ssh-timeout.json')}), show: {inline: '#deploy-result'}},
    {id: 'key.deploy.host-key-changed', setup: async p => { await onSecurity(p); await fillDeploy(p); }, act: p => p.evaluate(() => deployKey()), device: security({'ssh/copyid': wireSsh('wire-changed-host.json')}), show: {inline: '#deploy-result'}},
    {id: 'key.deploy.server-refused', setup: async p => { await onSecurity(p); await fillDeploy(p); }, act: p => p.evaluate(() => deployKey()),
     device: security({'ssh/copyid': '{"code":7,"message":"ssh_deploy_failed","data":{"detail":"sh: 1: cannot create /root/.ssh/authorized_keys: Permission denied"}}'}), show: {inline: '#deploy-result'}},
    {id: 'key.deploy.no-answer', setup: async p => { await onSecurity(p); await fillDeploy(p); }, act: p => p.evaluate(() => deployKey()), device: security({'ssh/copyid': DROP}), show: {inline: '#deploy-result'}},
    {id: 'key.deploy.missing-host', setup: async p => { await onSecurity(p); await fillDeploy(p, {host: ''}); }, act: p => p.evaluate(() => deployKey()), device: security(), show: {field: 'deploy-host'}},
    {id: 'key.deploy.missing-password', setup: async p => { await onSecurity(p); await fillDeploy(p, {password: ''}); }, act: p => p.evaluate(() => deployKey()), device: security(), show: {field: 'deploy-password'}},
    // ---- revoke a key from a server
    {id: 'key.revoke.wrong-password', setup: async p => { await onSecurity(p); await fillRevoke(p); }, act: p => p.evaluate(() => revokeKey()), device: security({'ssh/revoke': wireSsh('wire-ssh-auth-failed.json')}), show: {inline: '#revoke-result'}},
    {id: 'key.revoke.server-failed', setup: async p => { await onSecurity(p); await fillRevoke(p); }, act: p => p.evaluate(() => revokeKey()),
     device: security({'ssh/revoke': '{"code":7,"message":"ssh_revoke_failed","data":{"detail":"exit status 1"}}'}), show: {inline: '#revoke-result'}},
    {id: 'key.revoke.no-answer', setup: async p => { await onSecurity(p); await fillRevoke(p); }, act: p => p.evaluate(() => revokeKey()), device: security({'ssh/revoke': DROP}), show: {inline: '#revoke-result'}},
    {id: 'key.revoke.missing-user', setup: async p => { await onSecurity(p); await fillRevoke(p, {user: ''}); }, act: p => p.evaluate(() => revokeKey()), device: security(), show: {field: 'revoke-user'}},
    // ---- the browser's clipboard
    {id: 'clipboard.denied', setup: async p => { await onSecurity(p); await p.evaluate(() => { showFullFingerprint(0); Object.defineProperty(navigator, 'clipboard', {configurable: true, value: {writeText: () => Promise.reject(new DOMException('Write permission denied.', 'NotAllowedError'))}}); }); },
     act: p => p.evaluate(() => copyFingerprintInfo()), device: security()},
]);

// ---------------------------------------------------------------------------------------------------------------------
// SSH commands page: save / delete / run / cancel
const commandsDevice = (overrides = {}) => ({
    'ssh/hosts/list': ok({hosts: [sshHost]}),
    'ssh/commands/list': ok({commands: [{id: 'restart', host_id: sshHost.id, name: 'Restart AGX', command: 'reboot'}]}),
    ...overrides
});
const onCommands = async p => { await p.evaluate(async hostId => { await loadCommandsPage(); await selectHost(hostId); }, sshHost.id); };
const openNewCommand = async (p, fields = {}) => {
    await onCommands(p);
    await p.evaluate(f => { showAddCommandModal(); document.getElementById('cmd-name').value = f.name ?? 'Restart AGX'; document.getElementById('cmd-command').value = f.command ?? 'reboot'; document.getElementById('cmd-edit-id').value = f.id ?? 'restart2'; }, fields);
};
const startedCommand = commandsDevice({'ssh/exec_stream': '{"code":0,"data":{"session_id":5}}'});

scenarioTests('ssh-commands', [
    // ---- save
    {id: 'command.save.ok', setup: openNewCommand, act: p => p.evaluate(() => saveCommand()), device: commandsDevice({'ssh/commands/add': '{"code":0,"data":{"id":"restart2"}}'})},
    {id: 'command.save.capacity', setup: openNewCommand, act: p => p.evaluate(() => saveCommand()), device: commandsDevice({'ssh/commands/add': '{"code":7,"message":"command_capacity"}'})},
    {id: 'command.save.no-answer', setup: openNewCommand, act: p => p.evaluate(() => saveCommand()), device: commandsDevice({'ssh/commands/add': DROP})},
    {id: 'command.save.missing-name', setup: (p) => openNewCommand(p, {name: ''}), act: p => p.evaluate(() => saveCommand()), device: commandsDevice(), show: {field: 'cmd-name'}},
    {id: 'command.save.missing-command', setup: (p) => openNewCommand(p, {command: ''}), act: p => p.evaluate(() => saveCommand()), device: commandsDevice(), show: {field: 'cmd-command'}},
    {id: 'command.save.missing-id', setup: (p) => openNewCommand(p, {id: ''}), act: p => p.evaluate(() => saveCommand()), device: commandsDevice(), show: {field: 'cmd-edit-id'}},
    {id: 'command.save.bad-id', setup: (p) => openNewCommand(p, {id: '-bad id-'}), act: p => p.evaluate(() => saveCommand()), device: commandsDevice(), show: {field: 'cmd-edit-id'}},
    // ---- delete
    {id: 'command.delete.ok', setup: async p => { await onCommands(p); await page.sureToDelete(p); }, act: p => p.evaluate(() => deleteCommand(0)), device: commandsDevice({'ssh/commands/remove': '{"code":0,"message":"Command removed"}'})},
    {id: 'command.delete.service-running', setup: async p => { await onCommands(p); await page.sureToDelete(p); }, act: p => p.evaluate(() => deleteCommand(0)), device: commandsDevice({'ssh/commands/remove': '{"code":4,"message":"service_delete_protected"}'}), show: {sheet: true}},
    {id: 'command.delete.no-answer', setup: async p => { await onCommands(p); await page.sureToDelete(p); }, act: p => p.evaluate(() => deleteCommand(0)), device: commandsDevice({'ssh/commands/remove': DROP})},
    // ---- run
    {id: 'command.run.busy', setup: onCommands, act: p => p.evaluate(() => executeCommand(0)), device: commandsDevice({'ssh/exec_stream': '{"code":4,"message":"ssh_exec_busy"}'})},
    {id: 'command.run.wrong-password', setup: onCommands, act: p => p.evaluate(() => executeCommand(0)), device: commandsDevice({'ssh/exec_stream': wireSsh('wire-ssh-auth-failed.json')})},
    {id: 'command.run.start-failed', setup: onCommands, act: p => p.evaluate(() => executeCommand(0)), device: commandsDevice({'ssh/exec_stream': '{"code":7,"message":"ssh_start_failed","data":{"detail":"ESP_ERR_NO_MEM"}}'})},
    {id: 'command.run.error-while-running', setup: onCommands, act: async p => { await p.evaluate(() => executeCommand(0)); await p.evaluate(() => handleSshExecMessage({type: 'ssh_exec_error', session_id: 5, error: 'ssh_connect_timeout', stage: 'timeout', detail: 'SSH connection failed (code=263)'})); },
     device: startedCommand},
    {id: 'command.run.output-incomplete', setup: onCommands, act: async p => { await p.evaluate(() => executeCommand(0)); await p.evaluate(() => handleSshExecMessage({type: 'ssh_exec_error', session_id: 5, error: 'ssh_output_incomplete'})); },
     device: startedCommand},
    {id: 'command.run.error-text-from-old-firmware', setup: onCommands, act: async p => { await p.evaluate(() => executeCommand(0)); await p.evaluate(() => handleSshExecMessage({type: 'ssh_exec_error', session_id: 5, error: 'SSH connection failed (code=259)'})); },
     device: startedCommand},
    // ---- cancel
    {id: 'command.cancel.no-session', setup: async p => { await onCommands(p); await p.evaluate(() => { document.getElementById('exec-result-section').style.display = ''; claimExecDisplay('ssh', document.getElementById('exec-result')); currentExecSessionId = 5; }); }, act: p => p.evaluate(() => cancelExecution()),
     device: commandsDevice({'ssh/cancel': '{"code":2,"message":"ssh_no_session"}'})},
    {id: 'command.cancel.no-answer', setup: async p => { await onCommands(p); await p.evaluate(() => { document.getElementById('exec-result-section').style.display = ''; claimExecDisplay('ssh', document.getElementById('exec-result')); currentExecSessionId = 5; }); }, act: p => p.evaluate(() => cancelExecution()),
     device: commandsDevice({'ssh/cancel': DROP})},
    // ---- helper commands (log / process) write their problems into the output
    {id: 'command.helper.wrong-password', setup: async p => { await onCommands(p); await p.evaluate(hostId => { document.getElementById('exec-result-section').style.display = ''; window._cmdHostsList = window._cmdHostsList || []; currentNohupInfo = {logFile: '/tmp/x.log', pidFile: '/tmp/x.pid', hostId}; document.getElementById('exec-result').textContent = ''; }, sshHost.id); },
     act: p => p.evaluate(() => executeNohupHelperCommand('echo hi')), device: commandsDevice({'ssh/exec': wireSsh('wire-ssh-auth-failed.json')}), show: {last: '#exec-result'}},
    {id: 'command.helper.host-key-unknown', setup: async p => { await onCommands(p); await p.evaluate(hostId => { document.getElementById('exec-result-section').style.display = ''; currentNohupInfo = {logFile: '/tmp/x.log', pidFile: '/tmp/x.pid', hostId}; document.getElementById('exec-result').textContent = ''; }, sshHost.id); },
     act: p => p.evaluate(() => executeNohupHelperCommand('echo hi')), device: commandsDevice({'ssh/exec': wireSsh('wire-new-host.json')}), show: {last: '#exec-result'}},
    {id: 'command.helper.no-answer', setup: async p => { await onCommands(p); await p.evaluate(hostId => { document.getElementById('exec-result-section').style.display = ''; currentNohupInfo = {logFile: '/tmp/x.log', pidFile: '/tmp/x.pid', hostId}; document.getElementById('exec-result').textContent = ''; }, sshHost.id); },
     act: p => p.evaluate(() => executeNohupHelperCommand('echo hi')), device: commandsDevice({'ssh/exec': DROP}), show: {last: '#exec-result'}},
    // ---- the host list
    {id: 'host.list.commands-page-refused', act: p => p.evaluate(() => { document.body.insertAdjacentHTML('beforeend', '<div id="host-selector"></div>'); return loadHostSelector(); }), device: {'ssh/hosts/list': '{"code":7,"message":"Failed to list hosts"}'}, show: {inline: '#host-selector'}},
    // ---- packs of commands and hosts
    {id: 'pack.export-command.not-allowed', setup: async p => { await onCommands(p); await p.evaluate(() => showExportSshCommandModal('restart')); }, act: p => p.evaluate(() => doExportSshCommandFromModal('restart')),
     device: commandsDevice({'ssh/commands/export': wire('wire-pack-not-allowed.json')}), show: {inline: '#export-ssh-cmd-result'}},
    {id: 'pack.export-host.cert-unavailable', setup: async p => { await onSecurity(p); await p.evaluate(() => showExportSshHostModal('root@192.0.2.8')); }, act: p => p.evaluate(() => doExportSshHostFromModal('root@192.0.2.8')),
     device: security({'ssh/hosts/export': '{"code":7,"message":"device_cert_unavailable","data":{"detail":"ESP_FAIL"}}'}), show: {inline: '#export-ssh-host-result'}},
    {id: 'pack.import-host.storage-failed', setup: async p => { await onSecurity(p); await p.evaluate(() => { showImportSshHostModal(); window._importSshHostTscfg = '{}'; window._importSshHostFilename = 'lab.tscfg'; }); }, act: p => p.evaluate(() => confirmSshHostImport()),
     device: security({'ssh/hosts/import': '{"code":7,"message":"storage_failed","data":{"detail":"No such file or directory"}}'}), show: {inline: '#import-ssh-host-result'}},
]);

// ---------------------------------------------------------------------------------------------------------------------
// Behaviour the sentences depend on (not text).
for (const language of ['zh-CN', 'en-US']) {
    test(`${language}: a data source switch enables and disables without a same-id rule, then displays the device state`, async () => {
        let enabled = false;
        await kit.withPage(language, call => {
            if (call.endpoint === 'automation/rules/get') return {code: 4, message: 'Rule not found'};
            if (call.endpoint === 'automation/sources/list') return ok({sources: [{id: 'temp', label: 'Temperature', type: 'rest', enabled}]});
            if (call.endpoint === 'automation/sources/enable' || call.endpoint === 'automation/sources/disable') {
                enabled = call.endpoint.endsWith('/enable');
                return ok();
            }
        }, async ({page, calls}) => {
            await page.evaluate(async () => {
                document.body.insertAdjacentHTML('beforeend', '<div id="sources-list"></div>');
                await refreshSources();
            });
            for (const next of [true, false]) {
                const count = calls.length;
                await page.locator('#sources-list [role="switch"]').click();
                await page.waitForFunction(next => document.querySelector('#sources-list [role="switch"]')?.getAttribute('aria-checked') === String(next), next);
                assert.equal(enabled, next);
                assert.deepEqual(calls.slice(count).filter(c => c.endpoint.startsWith('automation/')).map(c => [c.endpoint, c.params]), [
                    [`automation/sources/${next ? 'enable' : 'disable'}`, {id: 'temp'}],
                    ['automation/sources/list', {}],
                ]);
            }
            assert(!calls.some(c => c.endpoint === 'automation/rules/get'), 'a source must not be looked up as a rule');
        });
    });
}

test('engine buttons: the engine status is read again whatever the answer was', async () => {
    for (const answer of [wire('wire-engine-start-ok.json'), wire('wire-engine-start-paused.json'), DROP]) {
        await kit.withPage('zh-CN', call => call.endpoint === 'automation/start' ? answer : undefined, async ({page, calls}) => {
            await page.evaluate(() => document.body.insertAdjacentHTML('beforeend', '<div id="automation-status"></div>'));
            await page.evaluate(() => automationControl('start'));
            assert(calls.some(c => c.endpoint === 'automation/status'), 'status was not refreshed');
        });
    }
});

test('a rule refused at the device refreshes the list the user acted on', async () => {
    await kit.withPage('zh-CN', call => ({'automation/rules/get': ok({revision: 7}), 'automation/rules/delete': wire('wire-rule-revision-conflict.json'), 'automation/rules/list': rulesList()})[call.endpoint], async ({page, calls}) => {
        await page.evaluate(() => { confirmAction = async () => true; document.body.insertAdjacentHTML('beforeend', '<div id="rules-list"></div>'); });
        await page.evaluate(() => deleteRule('night-off'));
        assert(calls.some(c => c.endpoint === 'automation/rules/list'), 'the list was not refreshed');
    });
});

test('validation of an action template points at the empty field and puts the cursor there', async () => {
    await kit.withPage('zh-CN', undefined, async ({page, calls}) => {
        await page.evaluate(() => showAddActionModal());
        await page.evaluate(() => submitAction());
        assert.equal(await page.evaluate(() => document.activeElement?.id), 'action-id');
        assert.equal(await page.locator('#action-id').getAttribute('aria-invalid'), 'true');
        assert.equal(calls.filter(c => c.endpoint.startsWith('automation/actions/')).length, 0, 'nothing may be sent for an incomplete form');
    });
});
