// The failure catalogue (failure.reasons / failure.ops in the language packs) against the writing standard,
// and against the reason tokens the firmware can send: a token without a message is a test failure.
const {test} = require('node:test'), assert = require('node:assert/strict'), vm = require('node:vm');
const fs = require('node:fs'), path = require('node:path');
const {read} = require('./harness.cjs');

const langs = {};
for (const lang of ['zh-CN', 'en-US']) vm.runInNewContext(read(`js/lang/${lang}.js`), {i18n: {registerLanguage: (k, v) => langs[k] = v}});
const failure = lang => langs[lang].failure;
const slots = text => [...new Set([...text.matchAll(/(?<!\$)\{(\w+)\}/g)].map(m => m[1]))].sort();
const CJK = /[㐀-鿿＀-￯　-〿]/;
// "automation" is the service name exactly as the System page's service list shows it; "json" is in a file name; "ID" is the field's label
const ALLOWED_LATIN = new Set(['SSH', 'SD', 'DNS', 'IP', 'WebUI', 'automation', 'json', 'ID', 'HTTP', 'WebSocket', 'Socket', 'IO', 'REST', 'OS', 'tscfg', 'JSON', 'UTF', 'LPMU', 'CA', 'bin', 'www', 'root', 'admin', 'OTA', 'USB', 'LED', 'NAT', 'WiFi', 'AP', 'STA', 'CSR', 'PKI', 'FAT', 'URL', 'QR']);
const flat = (object, prefix = '') => Object.entries(object).flatMap(([k, v]) => typeof v === 'string' ? [[prefix + k, v]] : flat(v, prefix + k + '.'));

test('both languages have the same reasons, the same parts of each reason and the same placeholders', () => {
    const zh = failure('zh-CN'), en = failure('en-US');
    assert.deepEqual(Object.keys(zh.reasons).sort(), Object.keys(en.reasons).sort());
    for (const token of Object.keys(zh.reasons)) {
        assert.deepEqual(Object.keys(zh.reasons[token]).sort(), Object.keys(en.reasons[token]).sort(), token);
        for (const part of Object.keys(zh.reasons[token])) assert.deepEqual(slots(zh.reasons[token][part]), slots(en.reasons[token][part]), `${token}.${part}`);
        assert(zh.reasons[token].why || zh.reasons[token].hint, `${token} says nothing`);
    }
    assert.deepEqual(flat(zh.ops).map(([k]) => k).sort(), flat(en.ops).map(([k]) => k).sort());
    for (const [k, v] of flat(zh.ops)) assert.deepEqual(slots(v), slots(Object.fromEntries(flat(en.ops))[k]), `ops.${k}`);
    assert.deepEqual(flat(zh.details).map(([k]) => k).sort(), flat(en.details).map(([k]) => k).sort());
});

test('every message follows the writing standard in both languages', () => {
    const problems = [];
    for (const lang of ['zh-CN', 'en-US']) {
        const zh = lang === 'zh-CN';
        const texts = [...flat(failure(lang).reasons, 'reasons.'), ...flat(failure(lang).ops, 'ops.')];
        for (const [key, text] of texts) {
            const where = `${lang} ${key}`;
            if (text !== text.trim()) problems.push(`${where}: surrounding whitespace`);
            if (/[a-z]+_[a-z0-9_]+/.test(text)) problems.push(`${where}: machine word`);
            if (/undefined|null|NaN|\[object|TODO|FIXME|code=|ESP_|errno/i.test(text)) problems.push(`${where}: internal word`);
            if (/未知错误|unknown error/i.test(text)) problems.push(`${where}: "unknown error" explains nothing`);
            if (key.endsWith('.hint') && (zh ? /^请(稍后)?(再)?重试。$/ : /^(please )?(try again|retry)( later)?\.$/i).test(text)) problems.push(`${where}: a bare "retry" is not a next step`);
            if (zh) {
                if (!CJK.test(text)) problems.push(`${where}: no Chinese`);
                for (const word of text.replace(/\{\w+\}/g, '').match(/[A-Za-z]{2,}/g) || []) if (!ALLOWED_LATIN.has(word)) problems.push(`${where}: English word "${word}" in the Chinese text`);
            } else if (CJK.test(text.replace(/[“”’]/g, ''))) problems.push(`${where}: Chinese in the English text`);
            if (key.startsWith('reasons.') && key.endsWith('.why') && /[。.!?！？：:;；]$/.test(text)) problems.push(`${where}: a reason has no final punctuation (the sentence adds it)`);
            if (key.startsWith('reasons.') && key.endsWith('.hint') && !(zh ? /。$/ : /\.$/).test(text)) problems.push(`${where}: a next step is a full sentence`);
            if (key.startsWith('reasons.') && key.endsWith('.why') && /^(请|please)/i.test(text)) problems.push(`${where}: a reason is not an instruction`);
            if (!zh && key.endsWith('.hint') && !/^[A-Z“]/.test(text)) problems.push(`${where}: sentence should start with a capital`);
        }
    }
    assert.deepEqual(problems, []);
});

// The operations the page reports through describeFailure / reportFailure / failureNote. Every call site needs the texts it
// can ask for: `failed` (or `blocked` when it passes phase: 'blocked'), and `unknown` when it passes `write` (a write may
// end without an answer). Operations chosen at run time cannot be read from the call sites: they are listed here and
// exercised by the page-level tests.
const DYNAMIC_OPS = ['engine.start', 'engine.stop', 'engine.pause', 'engine.reload'];   // automationControl: `engine.${action}`
function enclosingObject(source, index) {   // the {...} literal around `index`
    let depth = 0, from = -1;
    for (let i = index; i >= 0; i--) {
        if (source[i] === '}') depth++;
        else if (source[i] === '{') { if (depth === 0) { from = i; break; } depth--; }
    }
    depth = 0;
    for (let i = from; i < source.length; i++) {
        if (source[i] === '{') depth++;
        else if (source[i] === '}' && --depth === 0) return source.slice(from, i + 1);
    }
    return '';
}
function usedOperations(app) {
    const used = new Map();   // 'group.name' -> {phases:Set, write:boolean}
    const entryOf = op => used.get(op) || used.set(op, {phases: new Set(), write: false}).get(op);
    for (const m of app.matchAll(/\bop:\s*'([a-z]+\.[a-z]+)'/g)) {
        const object = enclosingObject(app, m.index), entry = entryOf(m[1]);
        entry.phases.add((object.match(/\bphase:\s*'(\w+)'/) || [, 'failed'])[1]);
        if (/\bwrite\b/.test(object.replace(/\bwrite:\s*false\b/g, ''))) entry.write = true;
    }
    // const op = enable ? 'rule.enable' : 'rule.disable'   (these are writes)
    for (const m of app.matchAll(/const op = ([^;\n]*);/g)) for (const lit of m[1].matchAll(/'([a-z]+\.[a-z]+)'/g)) { const entry = entryOf(lit[1]); entry.phases.add('failed'); entry.write = true; }
    for (const op of DYNAMIC_OPS) { const entry = entryOf(op); entry.phases.add('failed'); entry.write = true; }
    return used;
}

test('the operations the page reports through describeFailure exist, with the texts that call needs', () => {
    const used = usedOperations(read('js/app.js'));
    assert(used.size >= 20, [...used.keys()].join());
    for (const lang of ['zh-CN', 'en-US']) {
        const ops = failure(lang).ops;
        for (const [op, {phases, write}] of used) {
            const [group, name] = op.split('.');
            assert(ops[group]?.[name], `${lang}: no texts for ${op}`);
            for (const phase of phases) assert.equal(typeof ops[group][name][phase], 'string', `${lang}: ${op}.${phase}`);
            if (write) assert.equal(typeof ops[group][name].unknown, 'string', `${lang}: ${op}.unknown (a write may end unconfirmed)`);
        }
    }
});

test('every operation in the catalogue is used by the page (a text nobody can reach is a trap for the next edit)', () => {
    const app = read('js/app.js');
    const used = usedOperations(app);
    const dead = [];
    for (const [group, entries] of Object.entries(failure('zh-CN').ops)) {
        if (group === 'generic') continue;
        for (const name of Object.keys(entries)) {
            const op = `${group}.${name}`;
            if (!used.has(op) && !app.includes(`failure.ops.${op}`) && !app.includes(`'${op}'`)) dead.push(op);
        }
    }
    assert.deepEqual(dead, []);
});

// ---------------------------------------------------------------------------------------------------------------------
// Tokens the firmware sends must have a message. They are read from the C sources, so a new error code in firmware
// fails here until its sentence is written.
const component = file => fs.readFileSync(path.resolve(__dirname, '../../components', file), 'utf8');
const functionBody = (source, name) => {
    const m = new RegExp('^(?:static )?[^\\n;]+\\b' + name + '\\([^;]+?\\)\\s*\\{', 'm').exec(source);
    assert(m, name);
    return source.slice(m.index, source.indexOf('\n}', m.index));
};
const returned = body => [...body.matchAll(/return "([a-z][a-z0-9_]*)";/g)].map(m => m[1]);
// words in these places are statuses, not failures
const NOT_FAILURES = new Set(['ok', 'no_change', 'mirror_failed', 'recovery_cleanup_pending']);

test('every reason token the firmware can send has a message in both languages', () => {
    const automation = component('ts_api/src/ts_api_automation.c');
    const ssh = component('ts_api/src/ts_api_ssh.c');
    const tokens = new Set([
        ...returned(functionBody(automation, 'trigger_reject_token')),
        ...returned(functionBody(automation, 'engine_refusal_token')),
        ...returned(functionBody(component('ts_config_pack/src/ts_config_pack.c'), 'ts_config_pack_reason')),
        ...returned(functionBody(component('ts_security/src/ts_ssh_client.c'), 'ts_ssh_connect_reason')),
        // rule save / enable / disable: the engine, the store and the API layer
        ...[component('ts_automation/src/ts_rule_engine.c'), component('ts_automation/src/ts_rule_store.c'), automation]
            .flatMap(source => [...source.matchAll(/error_code\s*=\s*"([a-z][a-z0-9_]*)"/g)].map(m => m[1])),
        ...[...automation.matchAll(/ts_api_result_error\(result, TS_API_ERR_\w+, "([a-z][a-z0-9_]*)"\)/g)].map(m => m[1]),
        // ts_api_result_reason(result, CODE, "token", detail): the helper the handlers use for a reason the page can state
        ...[...automation.matchAll(/ts_api_result_reason\(result, TS_API_ERR_\w+, "([a-z][a-z0-9_]*)"/g)].map(m => m[1]),
        // reasons the handlers put straight into the message: result->message = strdup("token")
        ...[...automation.matchAll(/\bstrdup\("([a-z][a-z0-9_]*)"\)/g)].map(m => m[1]),
        // the SSH exec errors the WebSocket sends: {"type":"ssh_exec_error","error":"<token>"}
        ...[...component('ts_webui/src/ts_webui_ws.c').matchAll(/"error", "([a-z][a-z0-9_]*)"\)\)!=NULL/g)].map(m => m[1]),
        ...[...component('ts_webui/src/ts_ws_operation.c').matchAll(/failed \? "([a-z][a-z0-9_]*)" : "([a-z][a-z0-9_]*)"/g)].flatMap(m => [m[1], m[2]]),
        // the replies the handlers' tails delegate to
        ...[...automation.matchAll(/message = strdup\([^;]*?"(action_failed|action_timeout)"/g)].map(m => m[1]),
    ]);
    assert(tokens.size > 20, [...tokens].join());
    for (const lang of ['zh-CN', 'en-US']) {
        const reasons = failure(lang).reasons;
        const missing = [...tokens].filter(token => !NOT_FAILURES.has(token) && !reasons[token]);
        assert.deepEqual(missing, [], `${lang}: reason tokens the firmware sends but the page has no sentence for`);
    }
});

test('the catalogue has no reason that nothing can produce (a sentence nobody can reach is a trap for the next edit)', () => {
    const sources = [component('ts_api/src/ts_api_automation.c'), component('ts_api/src/ts_api_ssh.c'), component('ts_api/src/ts_api_key.c'), component('ts_automation/src/ts_rule_engine.c'),
        component('ts_automation/src/ts_rule_store.c'), component('ts_config_pack/src/ts_config_pack.c'), component('ts_security/src/ts_ssh_client.c'), component('ts_automation/src/ts_rule_store_set.c'), component('ts_automation/src/ts_rule_pack.c'), component('ts_cert/src/ts_cert.c'), component('ts_webui/src/ts_webui_api.c'), component('ts_webui/src/ts_webui_ws.c'), component('ts_webui/src/ts_ws_operation.c'), read('js/app.js'), read('js/api.js')].join('\n');
    // reasons the page produces from the error class, from a transport state, or from the API envelope of other modules
    const GENERIC = new Set(['busy', 'no_memory', 'not_found', 'no_permission', 'timeout', 'invalid_request', 'invalid_argument', 'not_supported', 'hardware_failed',
        'target_unreachable', 'auth_required', 'network_lost', 'invalid_response', 'no_detail', 'result_unknown', 'storage_failed']);
    const unreachable = Object.keys(failure('zh-CN').reasons).filter(token => !GENERIC.has(token) && !new RegExp('["\']' + token + '["\']').test(sources));
    assert.deepEqual(unreachable, []);
});
