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
        const latin = (text.replace(/[“”‘’]/g, '').match(/[A-Za-z]{2,}/g) || []).filter(word => !['SSH', 'SD', 'DNS', 'IP', 'WebUI', 'automation', 'json', 'ID', 'LED', 'REST', 'Webhook', 'QR', 'URL', 'WebSocket', 'Socket', 'IO', 'HTTP', 'API', 'OS', 'tscfg'].includes(word));
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
    return element.evaluate(node => ({type: 'inline', text: node.textContent.trim(), details: node.querySelector('[title]')?.getAttribute('title')?.replace(/\d{1,2}:\d{2}:\d{2}/, '<time>') ?? null}));
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
                    await scenario.act(page, names);
                    const show = scenario.show ?? 'toast';
                    const seen = show === 'toast' ? await observeToast(page) : show.inline ? await observeInline(page, show.inline) : await observeField(page, show.field);
                    checkSentence(language, seen.text, scenario.id);
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
     device: {'automation/actions/get': ok({id: 'restart-agx', type: 'cli'}), 'automation/actions/delete': '{"code":4,"message":"service_delete_protected"}'}},
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
    {id: 'pack.import-rule.unavailable', setup: p => p.evaluate(() => { showImportRuleModal(); window._importRuleTscfg = '{}'; window._importRuleFilename = 'rule_night.tscfg'; }),
     act: p => p.evaluate(() => confirmRuleImport()), device: {'automation/rules/import': '{"code":8,"message":"rule_import_unavailable","data":{"detail":"encrypted rule import requires a recoverable writer"}}'}, show: {inline: '#import-rule-result'}},
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
// Behaviour the sentences depend on (not text).
test('a data source switch asks the device about the source itself, not about a rule with the same id', async () => {
    await kit.withPage('zh-CN', call => call.endpoint === 'automation/sources/enable' ? '{"code":0,"message":"Source enabled"}' : undefined, async ({page, calls}) => {
        await page.evaluate(() => toggleSource('temp', true));
        assert.deepEqual(calls.filter(c => c.endpoint.startsWith('automation/')).map(c => [c.endpoint, c.params]).slice(0, 1), [['automation/sources/enable', {id: 'temp'}]]);
        assert(!calls.some(c => c.endpoint === 'automation/rules/get'), 'a source must not be looked up as a rule');
    });
});

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
