// Execute production deployment/list functions and API business-code handling.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync('components/ts_webui/web/js/app.js', 'utf8');
function fn(name) {
    const match = new RegExp('^(?:async )?function ' + name + '\\(', 'm').exec(source);
    assert(match, name);
    const text = source.slice(match.index), end = text.indexOf('\n}') + 2;
    assert(end > 1);
    return text.slice(0, end);
}
const languages = {};
for (const lang of ['zh-CN', 'en-US']) {
    vm.runInNewContext(fs.readFileSync(`components/ts_webui/web/js/lang/${lang}.js`, 'utf8'), {
        i18n: {registerLanguage: (id, data) => languages[id] = data}
    });
}
let locale = 'zh-CN', current = true, calls = 0, listCalls = 0, listFailure = null;
let deployment, listing, toasts = [];
const nodes = new Map();
function el(id) {
    if (!nodes.has(id)) {
        const classes = new Set();
        nodes.set(id, {value: '', disabled: false, textContent: '', innerHTML: '',
            classList: {add: (...names) => names.forEach(n => classes.add(n)),
                        remove: (...names) => names.forEach(n => classes.delete(n)),
                        contains: n => classes.has(n)}});
    }
    return nodes.get(id);
}
function t(key, args = {}) {
    let value = key.split('.').reduce((v, k) => v?.[k], languages[locale]) ?? key;
    for (const [name, text] of Object.entries(args)) value = value.replaceAll(`{${name}}`, text);
    return value;
}
const context = vm.createContext({console: {log() {}, error() {}}, document: {getElementById: el},
    window: {}, t, capturePageValidity: () => () => current,
    escapeHtml: text => String(text).replaceAll('&', '&amp;').replaceAll('<', '&lt;'),
    showToast: (msg, tone) => toasts.push({msg, tone}),
    api: {
        sshCopyid: async (host, user, password, key, port, verify) => {
            calls++;assert.equal(key, 'synthetic-key');assert.equal(port, 2222);assert.equal(verify, true);
            // All stale result styles must be cleared before the request.
            for (const style of ['hidden', 'success', 'warning', 'error']) assert(!el('deploy-result').classList.contains(style));
            return deployment;
        },
        call: async method => {
            assert.equal(method, 'ssh.hosts.list');listCalls++;
            if (listFailure) throw Error('synthetic network failure');
            return listing;
        }
    }
});
const apiSource = fs.readFileSync('components/ts_webui/web/js/api.js', 'utf8');
vm.runInContext(apiSource.slice(apiSource.indexOf('function apiErrorReason'), apiSource.indexOf('class TianShanAPI')), context);
vm.runInContext("let currentDeployKeyId = 'synthetic-key';", context);
const loadVersion = source.match(/^let sshHostsLoadVersion[^\n]+/m);
if (loadVersion) vm.runInContext(loadVersion[0], context);
for (const name of ['loadSshHostsData', 'refreshSshHostsList', 'deployKey']) vm.runInContext(fn(name), context);
const target = {id: 'test@192.0.2.8:2222', host: '192.0.2.8', port: 2222, username: 'test', keyid: 'synthetic-key'};
function reset() {
    deployment = {code: 0, data: {deployed: true, verified: true, registered: true, host_id: target.id}};
    listing = {code: 0, data: {hosts: [target]}};listFailure = null;
    calls = listCalls = 0;toasts = [];current = true;
    el('deploy-host').value = '192.0.2.8';el('deploy-user').value = 'test';
    el('deploy-password').value = 'synthetic';el('deploy-port').value = '2222';
    el('deploy-result').classList.add('hidden', 'success', 'warning', 'error');
    context.window._sshHostsData = {old: {id: 'old', host: '192.0.2.9'}};
    context.window._sshHostsList = [context.window._sshHostsData.old];
}
async function deploy(tone, key) {
    await context.deployKey();
    assert.equal(calls, 1);assert.equal(el('deploy-btn').disabled, false);
    assert(el('deploy-result').classList.contains(tone));
    assert(el('deploy-result').textContent.includes(t(key).split('{')[0]));
    if (tone !== 'error') assert.equal(toasts.at(-1).tone, tone);
    if (tone !== 'success') assert(!el('deploy-result').classList.contains('success'));
}
(async () => {
    for (locale of ['zh-CN', 'en-US']) {
        for (const key of ['keyVerified', 'keyUnverified', 'keyRegistrationFailed', 'keyRegistrationUnconfirmed',
                           'hostRegistrationBusy', 'hostRegistrationFull', 'hostRegistrationStorage', 'deployedHostsRefreshFailed']) {
            assert.notEqual(t('promptRepair.' + key), 'promptRepair.' + key);
        }
        reset();await deploy('success', 'promptRepair.keyVerified');
        assert.equal(listCalls, 1);assert(el('ssh-hosts-table-body').innerHTML.includes(target.id));
        assert(el('ssh-hosts-table-body').innerHTML.includes(target.keyid));
        assert.equal(context.window._sshHostsList[0].id, target.id);
        reset();deployment.data.verified = false;await deploy('warning', 'promptRepair.keyUnverified');
        for (const [err, code, reason] of [['ESP_ERR_INVALID_STATE', 4, 'hostRegistrationBusy'],
                                         ['ESP_ERR_NO_MEM', 6, 'hostRegistrationFull'], ['ESP_FAIL', 7, 'hostRegistrationStorage']]) {
            reset();deployment.code = code;deployment.data.registered = false;deployment.data.registration_error = err;
            await deploy('warning', 'promptRepair.keyRegistrationFailed');
            assert(el('deploy-result').textContent.includes(t('promptRepair.' + reason, {detail: err})));
        }
        reset();deployment.data.registered = false;await deploy('warning', 'promptRepair.keyRegistrationFailed');
        for (const fail of ['business', 'network', 'format']) {
            reset();const oldCache = context.window._sshHostsData, oldList = context.window._sshHostsList;
            if (fail === 'business') listing = {code: 7, data: {hosts: []}};
            if (fail === 'network') listFailure = true;
            if (fail === 'format') listing = {code: 0, data: {}};
            await deploy('warning', 'promptRepair.keyVerified');
            assert.equal(context.window._sshHostsData, oldCache);assert.equal(context.window._sshHostsList, oldList);
            assert(el('ssh-hosts-table-body').innerHTML.includes(t('common.loadFailed')));
            assert(!el('ssh-hosts-table-body').innerHTML.includes(t('securityPage.noDeployedHostsHint')));
            assert(el('deploy-result').textContent.includes(t('promptRepair.deployedHostsRefreshFailed')));
        }
        reset();delete deployment.data.registered;await deploy('success', 'promptRepair.keyVerified');
        for (const changed of ['host', 'port', 'username', 'keyid']) {
            reset();delete deployment.data.registered;
            listing.data.hosts = [{...target, [changed]: changed === 'port' ? 22 : 'different'}];
            await deploy('warning', 'promptRepair.keyRegistrationUnconfirmed');
        }
        reset();delete deployment.data.registered;listFailure = true;await deploy('warning', 'promptRepair.keyRegistrationUnconfirmed');
        reset();delete deployment.data.registered;listing.data.hosts = [];await deploy('warning', 'promptRepair.keyRegistrationUnconfirmed');
        assert(el('ssh-hosts-table-body').innerHTML.includes(t('securityPage.noDeployedHostsHint')));
        assert.equal(context.window._sshHostsList.length, 0);
        reset();deployment = {code: 7, message: 'synthetic deployment failure'};
        await deploy('error', 'pkiPage.deployFailedMsg');assert.equal(listCalls, 0);
        reset();current = false;await context.deployKey();assert.equal(listCalls, 0);assert.equal(toasts.length, 0);
        for (const oldResponse of [{code: 0, data: {hosts: []}}, {code: 7, data: {hosts: []}}]) {
            reset();const originalCall = context.api.call;let finishOld, reads = 0;
            context.api.call = async (...args) => ++reads === 1 ? new Promise(resolve => finishOld = resolve) : originalCall(...args);
            const oldRefresh = context.refreshSshHostsList();
            await deploy('success', 'promptRepair.keyVerified');
            finishOld(oldResponse);await oldRefresh;context.api.call = originalCall;
            assert.equal(context.window._sshHostsData[target.id].keyid, target.keyid);
            assert(el('ssh-hosts-table-body').innerHTML.includes(target.id));
        }

    }
    console.log('PASS production bilingual SSH UI/API helpers: rendered ID/key, verification/registration partial outcomes, cache-preserving list errors, true empty list, old-firmware tuple confirmation, stale style/page cleanup, out-of-order list replies, no repeat deployment');
})().catch(error => {console.error(error);process.exitCode = 1});
