// Failure messages ("<what failed>: <why>. <what to do next>") in the real page, in both languages.
//   - the failure toast: stays 8 s, pauses under the pointer, folds the technical details, can be dismissed
//   - describeFailure: the exact sentences, fallbacks, and that no machine code / placeholder / "unavailable" text leaks
//   - rules: enable / disable (real list, real switch) against the bytes the firmware sends
//   - quick actions: why a card cannot start, and what the device says when it refuses
// The device bytes are the contract fixtures written by tests/runtime/test_failure_wire.py.
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const kit = require('./pagekit.cjs');
const {open, withPage, toastText, toastIs, expectToast} = kit;

const fixture = name => fs.readFileSync(path.resolve(__dirname, '../fixtures/failure-wire', name), 'utf8');
kit.install();

for (const language of ['zh-CN', 'en-US']) {
    const zh = language === 'zh-CN';

    // ------------------------------------------------------------------------------------------ the toast itself
    test(`${language}: failure toast shows the sentence, folds the details, and can be closed`, async () => {
        await withPage(language, undefined, async ({page}) => {
            await page.evaluate(() => showFailureToast('Sentence one. Sentence two.', 'Reason code: busy\nAPI: x.y'));
            const toast = page.locator('#toast');
            assert.equal(await toastText(page), 'Sentence one. Sentence two.');
            assert.equal(await toast.evaluate(e => e.classList.contains('show') && e.classList.contains('toast-error') && e.classList.contains('rich')), true);
            assert.equal(await toast.getAttribute('role'), 'alert');
            const more = toast.locator('.toast-more'), detail = toast.locator('.toast-detail');
            assert.equal(await detail.isHidden(), true);
            assert.equal(await more.textContent(), await page.evaluate(() => t('failure.details.show')));
            assert.equal(await more.getAttribute('aria-expanded'), 'false');
            await more.click();
            assert.equal(await detail.isVisible(), true);
            assert.equal(await detail.textContent(), 'Reason code: busy\nAPI: x.y');
            assert.equal(await more.textContent(), await page.evaluate(() => t('failure.details.hide')));
            assert.equal(await more.getAttribute('aria-expanded'), 'true');
            await more.click();
            assert.equal(await detail.isHidden(), true);
            await toast.locator('.toast-close').click();
            assert.equal(await toast.evaluate(e => e.classList.contains('show')), false);
            // a hidden toast must not sit invisibly over the page
            assert.equal(await toast.evaluate(e => getComputedStyle(e).pointerEvents), 'none');
        });
    });

    test(`${language}: failure toast stays 8 seconds, waits while the pointer is on it or the details are open, and is not erased by a routine success`, async () => {
        await withPage(language, undefined, async ({page}) => {
            await page.evaluate(() => showFailureToast('Failure.', 'details'));
            const left = await page.evaluate(() => toastDeadline - Date.now());
            assert(left > 7000 && left <= 8000, `deadline ${left}`);
            // a routine success arriving meanwhile does not wipe the failure the user is reading
            await page.evaluate(() => showToast('All good', 'success'));
            assert.equal(await toastText(page), 'Failure.');
            // pointer pause (short duration to keep the test quick)
            await page.evaluate(() => showFailureToast('Pause me.', 'details', {duration: 700}));
            const box = await page.locator('#toast').boundingBox();
            await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
            await page.waitForTimeout(1300);
            assert.equal(await page.locator('#toast').evaluate(e => e.classList.contains('show')), true);
            await page.mouse.move(5, 5);
            await page.waitForFunction(() => !document.getElementById('toast').classList.contains('show'), null, {timeout: 6000});
            // open details also keep it up
            await page.evaluate(() => showFailureToast('Read me.', 'details', {duration: 600}));
            await page.locator('#toast .toast-more').click();
            await page.mouse.move(5, 5);
            await page.waitForTimeout(1200);
            assert.equal(await page.locator('#toast').evaluate(e => e.classList.contains('show')), true);
            await page.locator('#toast .toast-more').click();   // collapse: the pointer is on the toast again, so it still waits...
            await page.waitForTimeout(900);
            assert.equal(await page.locator('#toast').evaluate(e => e.classList.contains('show')), true);
            await page.mouse.move(5, 5);                           // ...until the pointer leaves
            await page.waitForFunction(() => !document.getElementById('toast').classList.contains('show'), null, {timeout: 6000});
            // a normal toast afterwards is a normal toast again
            await page.evaluate(() => showToast('Plain', 'success'));
            assert.equal(await page.locator('#toast').textContent(), 'Plain');
            assert.equal(await page.locator('#toast').evaluate(e => e.classList.contains('rich') || !!e.querySelector('button')), false);
        });
    });

    // ------------------------------------------------------------------------------------------ the sentences
    const name = zh ? '夜间关灯' : 'Night off';
    const ssh = {host: '192.0.2.8', port: 22, user: 'test'};
    const cases = [
        // [op, phase, params, device result, token override, expected]
        ['rule.disable', 'failed', {name}, {code: 1, rawMessage: 'execution_busy'}, null,
            zh ? '禁用规则“夜间关灯”失败：它正在执行中。等执行完成后再试。' : 'Failed to disable rule “Night off”: it is running right now. Wait for it to finish, then try again.'],
        ['rule.enable', 'failed', {name}, {code: 1, rawMessage: 'revision_conflict'}, null,
            zh ? '启用规则“夜间关灯”失败：设备上的这份配置刚被其他操作修改过。请刷新后确认最新内容，再重新操作。' : 'Failed to enable rule “Night off”: the configuration on the device was just changed by another operation. Refresh, check the latest content, then try again.'],
        ['rule.enable', 'failed', {name}, {code: 1, rawMessage: 'service_busy'}, null,
            zh ? '启用规则“夜间关灯”失败：它绑定的服务正在运行，或状态还没有确认。请先核实并停止该服务。' : 'Failed to enable rule “Night off”: its bound service is running or its state is unconfirmed. Verify the service state and stop it first.'],
        ['rule.save', 'failed', {name}, {code: 1, rawMessage: 'capacity'}, null,
            zh ? '保存规则“夜间关灯”失败：规则数量已达上限。请先删除不用的规则。' : 'Failed to save rule “Night off”: the rule limit has been reached. Delete unused rules first.'],
        ['rule.save', 'failed', {name}, {code: 1, rawMessage: 'commit_unknown'}, null,
            zh ? '保存规则“夜间关灯”的结果未确认。请重新读取并核对当前状态，不要重复提交。' : 'The result of saving rule “Night off” is unconfirmed. Reload and check the current state before submitting again.'],
        ['quick.start', 'blocked', {name}, null, 'rule_disabled',
            zh ? '无法启动“夜间关灯”：这条规则已禁用。请先到「自动化」页启用它。' : 'Can\'t start “Night off”: the rule is disabled. Enable it on the Automation page first.'],
        ['quick.start', 'blocked', {name}, null, 'manual_not_allowed',
            zh ? '无法启动“夜间关灯”：这条规则没有开启“允许手动执行”。请在规则设置里打开“允许手动执行”。' : 'Can\'t start “Night off”: the rule does not allow manual execution. Turn on “Allow manual execution” in the rule settings.'],
        ['quick.start', 'blocked', {name}, null, 'service_not_confirmed_stopped',
            zh ? '无法启动“夜间关灯”：还没有确认它的服务已停止。请先点「核验远端状态」，确认后再启动。' : 'Can\'t start “Night off”: its service is not confirmed to be stopped. Use “Verify remote state” first, then start it.'],
        ['quick.start', 'failed', {name}, {code: 4, rawMessage: 'engine_not_accepting'}, null,
            zh ? '启动“夜间关灯”失败：自动化引擎暂时没有接收执行请求（可能正在重载配置，或已停止）。请稍等几秒再试。一直不行，请到「系统」页的服务列表里重启“automation”服务。'
               : 'Failed to start “Night off”: the automation engine is not accepting runs right now (it may be reloading its configuration, or stopped). Wait a few seconds and try again. If it keeps happening, restart the “automation” service in the service list on the System page.'],
        ['quick.start', 'failed', {name}, {code: 2, rawMessage: 'Rule not found'}, null,
            zh ? '启动“夜间关灯”失败：设备上找不到这条规则。它可能已被删除，请刷新页面。' : 'Failed to start “Night off”: the rule was not found on the device. It may have been deleted; refresh the page.'],
        ['ssh.connect', 'failed', ssh, {code: 10, rawMessage: 'ssh_auth_failed'}, null,
            zh ? '连接 192.0.2.8 失败：主机拒绝了用户“test”的登录。请检查用户名，以及密码或密钥是否正确。' : 'Failed to connect to 192.0.2.8: the host rejected the login for user “test”. Check the username and that the password or key is correct.'],
        ['ssh.connect', 'failed', ssh, {code: 10, rawMessage: 'ssh_port_refused'}, null,
            zh ? '连接 192.0.2.8 失败：对方拒绝了连接（端口 22）。请确认目标主机上的 SSH 服务已启动、端口号正确。' : 'Failed to connect to 192.0.2.8: the connection was refused (port 22). Make sure SSH is running on the host and the port number is right.'],
        // a reason the page does not know is described honestly, by the class of the error - never as a code or an English device text
        ['rule.disable', 'failed', {name}, {code: 7, rawMessage: 'some_future_reason'}, null,
            zh ? '禁用规则“夜间关灯”失败：设备没有返回具体原因。可在「终端」页打开「系统日志」查看这次操作的详细记录。' : 'Failed to disable rule “Night off”: the device did not return a specific reason. Open Terminal → System Logs to see the details of this operation.'],
        ['rule.disable', 'failed', {name}, {code: 4, rawMessage: 'Some English text'}, null,
            zh ? '禁用规则“夜间关灯”失败：设备正在处理另一项操作。等它完成后再试。' : 'Failed to disable rule “Night off”: the device is busy with another operation. Wait for it to finish, then try again.'],
        // new page, firmware that predates the reason tokens: what the old firmware sent is still described honestly
        ['ssh.connect', 'failed', ssh, {code: 10, rawMessage: 'SSH connection failed (code=259)'}, null,
            zh ? '连接 192.0.2.8 失败：无法连接到目标。请检查网络连接和目标的地址。' : 'Failed to connect to 192.0.2.8: the target could not be reached. Check the network connection and the target address.'],
        ['quick.start', 'failed', {name}, {code: 7, rawMessage: 'Failed to trigger rule'}, null,
            zh ? '启动“夜间关灯”失败：设备没有返回具体原因。可在「终端」页打开「系统日志」查看这次操作的详细记录。' : 'Failed to start “Night off”: the device did not return a specific reason. Open Terminal → System Logs to see the details of this operation.'],
        ['rule.disable', 'failed', {name}, {code: 1, rawMessage: 'invalid_configuration'}, null,
            zh ? '禁用规则“夜间关灯”失败：规则内容不完整或不合法。请检查触发条件和动作，修正后再保存。' : 'Failed to disable rule “Night off”: the rule is incomplete or invalid. Check the conditions and actions, fix them, then save again.'],
        // an operation the page has no text for still gets a sentence
        ['nothing.like.this', 'failed', {}, {code: 6, rawMessage: ''}, null,
            zh ? '操作失败：设备可用内存不足。请稍后再试；仍然失败请重启设备。' : 'The operation failed: the device is low on memory. Try again later; restart the device if it keeps failing.'],
    ];
    test(`${language}: describeFailure gives the exact sentence for each case and never leaks codes or placeholders`, async () => {
        await withPage(language, undefined, async ({page}) => {
            for (const [op, phase, params, result, token, expected] of cases) {
                const out = await page.evaluate(([op, phase, params, result, token]) => describeFailure({op, phase, params, result, token, endpoint: 'x.y'}), [op, phase, params, result, token]);
                assert.equal(out.message, expected, `${op}/${phase}/${token || result?.rawMessage}`);
                assert(!/[a-z]+_[a-z_]+/.test(out.message), 'machine code in: ' + out.message);
                assert(!/[{}]|undefined|null|\[object/.test(out.message), 'placeholder leak in: ' + out.message);
                assert(!out.message.includes(await page.evaluate(() => i18n.unavailable())), 'unavailable text');
            }
        });
    });

    test(`${language}: describeFailure keeps the device text in the folded details, and survives a missing catalogue`, async () => {
        await withPage(language, undefined, async ({page}) => {
            const out = await page.evaluate(() => describeFailure({op: 'rule.disable', params: {name: 'x'}, result: {code: 7, rawMessage: 'Failed to get layer', data: {stage: 'auth', detail: 'raw detail'}}, endpoint: 'automation.rules.disable'}));
            assert(out.details.includes('no_detail') && out.details.includes('Failed to get layer') && out.details.includes('automation.rules.disable') && out.details.includes('auth'), out.details);
            assert(!out.message.includes('Failed to get layer'));
            // a transport failure of a write is "unconfirmed", of a read it is "cannot reach"
            const write = await page.evaluate(() => describeFailure({op: 'rule.save', params: {name: 'x'}, error: Object.assign(new Error('x'), {kind: 'network', uncertain: true, rawMessage: ''})}));
            assert.equal(write.uncertain, true);
            const read = await page.evaluate(() => describeFailure({op: 'rule.save', params: {name: 'x'}, error: Object.assign(new Error('x'), {kind: 'network', uncertain: false, rawMessage: ''})}));
            assert.equal(read.token, 'network_lost');
            // placeholders the caller cannot fill never produce the "unavailable" sentence
            const own = await page.evaluate(() => describeFailure({op: 'quick.start', phase: 'blocked', params: {name: 'x'}, token: 'rule_disabled'}));
            assert.equal(own.details, '');   // the page worked it out itself: nothing was sent, nothing to look into
            const missing = await page.evaluate(() => describeFailure({op: 'ssh.connect', params: {host: 'h'}, result: {code: 10, rawMessage: 'ssh_auth_failed'}}));
            assert.equal(missing.token, 'target_unreachable');   // the class of the error (10 = cannot reach the target) is still said honestly
            assert(!missing.message.includes('unavailable') && !/[{}]/.test(missing.message));
            // no language catalogue at all: the old composed sentence, never an empty toast
            const old = await page.evaluate(() => { const complete = i18n.complete; i18n.complete = () => false; try { return describeFailure({op: 'rule.disable', params: {name: 'x'}, result: {code: 4, rawMessage: 'busy now'}}); } finally { i18n.complete = complete; } });
            assert(old.message.length > 5);
        });
    });

    // ------------------------------------------------------------------------------------------ rules: enable / disable
    const rule = {id: 'night-off', name, enabled: true, manual_trigger: false, conditions_count: 1, actions_count: 1, trigger_count: 0};
    const rulesDevice = (write, state = {}) => call => {
        if (call.endpoint === 'automation/rules/list') return {code: 0, data: {loaded: true, rules: [{...rule, enabled: state.enabled ?? true}]}};
        if (call.endpoint === 'automation/rules/get') return {code: 0, data: {revision: 7}};
        if (call.endpoint === 'automation/rules/disable' || call.endpoint === 'automation/rules/enable') return write(call);
    };
    const rulesPage = async page => {
        await page.evaluate(async () => { document.body.insertAdjacentHTML('beforeend', '<div id="rules-list"></div>'); await refreshRules(); });
        return page.locator('#rules-list .switch');
    };

    test(`${language}: rules: a rule that is running cannot be disabled - the toast names the rule, the reason and the next step`, async () => {
        await withPage(language, rulesDevice(() => fixture('wire-rule-execution-busy.json')), async ({page, calls}) => {
            await (await rulesPage(page)).click();
            await expectToast(page, 'error', zh ? '禁用规则“夜间关灯”失败：它正在执行中。等执行完成后再试。' : 'Failed to disable rule “Night off”: it is running right now. Wait for it to finish, then try again.');
            // the machine word lives only in the folded details
            assert(!(await toastText(page)).includes('execution_busy'));
            await page.locator('#toast .toast-more').click();
            const detail = await page.locator('#toast .toast-detail').textContent();
            assert(detail.includes('execution_busy') && detail.includes('automation.rules.disable'), detail);
            const write = calls.filter(c => c.endpoint === 'automation/rules/disable');
            assert.equal(write.length, 1);
            assert.equal(write[0].params.expected_revision, 7);
        });
    });

    test(`${language}: rules: a conflicting edit refreshes the list the user acted on`, async () => {
        await withPage(language, rulesDevice(() => fixture('wire-rule-revision-conflict.json')), async ({page, calls}) => {
            await (await rulesPage(page)).click();
            await expectToast(page, 'error', zh ? '禁用规则“夜间关灯”失败：设备上的这份配置刚被其他操作修改过。请刷新后确认最新内容，再重新操作。' : 'Failed to disable rule “Night off”: the configuration on the device was just changed by another operation. Refresh, check the latest content, then try again.');
            await page.waitForFunction(() => true);
            assert.equal(calls.filter(c => c.endpoint === 'automation/rules/list').length, 2);
        });
    });

    test(`${language}: rules: success says what happened to which rule, not "ok"`, async () => {
        await withPage(language, rulesDevice(() => fixture('wire-rule-ok.json')), async ({page}) => {
            await (await rulesPage(page)).click();
            await expectToast(page, 'success', zh ? '已禁用规则“夜间关灯”' : 'Rule “Night off” disabled');
            assert.equal(await page.locator('#toast').evaluate(e => !!e.querySelector('button')), false);   // a plain toast
        });
        await withPage(language, rulesDevice(() => fixture('wire-rule-ok-mirror.json'), {enabled: false}), async ({page}) => {
            await (await rulesPage(page)).click();
            await expectToast(page, 'warning', zh ? '已启用规则“夜间关灯”，但备用副本未同步；请勿移除 SD 卡。' : 'Rule “Night off” enabled, but the backup copy is not synced; do not remove the SD card.');
        });
    });

    test(`${language}: rules: an unconfirmed result is a warning that says not to repeat it`, async () => {
        await withPage(language, rulesDevice(() => fixture('wire-rule-commit-unknown.json')), async ({page}) => {
            await (await rulesPage(page)).click();
            await expectToast(page, 'warning', zh ? '禁用规则“夜间关灯”的结果未确认。请重新读取并核对当前状态，不要重复提交。' : 'The result of disabling rule “Night off” is unconfirmed. Reload and check the current state before submitting again.');
        });
        // the write never got an answer
        await withPage(language, rulesDevice(() => { throw new Error('unused'); }), async ({page, calls}) => {
            const toggle = await rulesPage(page);
            await page.route('**/api/v1/automation/rules/disable', route => route.abort());
            await toggle.click();
            await expectToast(page, 'warning', zh ? '禁用规则“夜间关灯”的结果未确认。请先刷新核对当前状态，不要重复提交。' : 'The result of disabling rule “Night off” is unconfirmed. Refresh and check the current state before repeating the operation.');
        });
    });

    // ------------------------------------------------------------------------------------------ quick actions
    const quickRule = {id: 'night-off', name, enabled: true, manual_trigger: true, allow_manual_trigger: true, show_on_dashboard: true, actions_count: 0};
    const quickDevice = (extra = {}, trigger = null) => call => {
        if (call.endpoint === 'ssh/hosts/list') return {code: 0, data: {hosts: []}};
        if (call.endpoint === 'ssh/commands/list') return {code: 0, data: {commands: []}};
        if (call.endpoint === 'automation/rules/list') return {code: 0, data: {loaded: true, rules: [{...quickRule, ...extra}]}};
        if (call.endpoint === 'automation/rules/trigger' && trigger) return trigger;
    };
    const quickPage = async page => {
        await page.evaluate(async () => {
            document.body.insertAdjacentHTML('beforeend', '<div id="quick-actions-grid"></div>');
            await refreshQuickActions();
            stopServiceStatusRefresh();
            if (typeof quickActionsTimeoutId !== 'undefined' && quickActionsTimeoutId) { clearTimeout(quickActionsTimeoutId); quickActionsTimeoutId = null; }
        });
        return page.locator('#quick-action-night-off');
    };

    test(`${language}: quick actions: every reason a card cannot start is its own sentence`, async () => {
        for (const [extra, token] of [[{enabled: false}, 'rule_disabled'], [{allow_manual_trigger: false}, 'manual_not_allowed'], [{reference_unresolved: true}, 'reference_unresolved'], [{pending_change: 'delete'}, 'rule_pending_delete'], [{runtime_active: false}, 'rule_not_active']]) {
            await withPage(language, quickDevice(extra), async ({page, calls}) => {
                const card = await quickPage(page);
                await card.waitFor();
                assert.equal(await card.getAttribute('data-allowed'), 'false');
                assert.equal(await card.getAttribute('data-block'), token);
                await card.locator('.quick-action-name').click();
                const expected = await page.evaluate(token => describeFailure({op: 'quick.start', phase: 'blocked', params: {name: document.querySelector('.quick-action-name').textContent}, token}).message, token);
                await expectToast(page, 'warning', expected);
                assert(expected.startsWith(zh ? '无法启动“夜间关灯”：' : 'Can\'t start “Night off”: '), expected);
                assert.equal(calls.filter(c => c.endpoint === 'automation/rules/trigger').length, 0);   // nothing was sent
            });
        }
    });

    test(`${language}: quick actions: the card's own state decides the message (exhaustive)`, async () => {
        await withPage(language, quickDevice(), async ({page}) => {
            const reasons = await page.evaluate(() => {
                const out = {};
                for (const state of ['stopped', 'running', 'ready', 'starting', 'checking', 'stopping', 'unknown', 'failed', 'timeout', 'something-new'])
                    out[state] = quickActionBlockReason({dataset: {allowed: 'true', service: 'svc', state}});
                out.noService = quickActionBlockReason({dataset: {allowed: 'true', service: '', state: 'running'}});
                out.blocked = quickActionBlockReason({dataset: {allowed: 'false', block: 'rule_disabled', service: 'svc', state: 'running'}});
                out.blockedNoReason = quickActionBlockReason({dataset: {allowed: 'false', service: '', state: 'stopped'}});
                return out;
            });
            assert.deepEqual(reasons, {stopped: '', running: 'service_running', ready: 'service_running', starting: 'service_starting', checking: 'service_checking',
                stopping: 'service_stopping', unknown: 'service_not_confirmed_stopped', failed: 'service_not_confirmed_stopped', timeout: 'service_not_confirmed_stopped',
                'something-new': 'service_not_confirmed_stopped', noService: '', blocked: 'rule_disabled', blockedNoReason: 'rule_details_unavailable'});
        });
    });

    for (const [file, expectedZh, expectedEn] of [
        ['wire-trigger-engine-not-accepting.json', '启动“夜间关灯”失败：自动化引擎暂时没有接收执行请求（可能正在重载配置，或已停止）。请稍等几秒再试。一直不行，请到「系统」页的服务列表里重启“automation”服务。',
            'Failed to start “Night off”: the automation engine is not accepting runs right now (it may be reloading its configuration, or stopped). Wait a few seconds and try again. If it keeps happening, restart the “automation” service in the service list on the System page.'],
        ['wire-trigger-rule-disabled.json', '启动“夜间关灯”失败：这条规则已禁用。请先到「自动化」页启用它。', 'Failed to start “Night off”: the rule is disabled. Enable it on the Automation page first.'],
        ['wire-trigger-manual-not-allowed.json', '启动“夜间关灯”失败：这条规则没有开启“允许手动执行”。请在规则设置里打开“允许手动执行”。',
            'Failed to start “Night off”: the rule does not allow manual execution. Turn on “Allow manual execution” in the rule settings.'],
        ['wire-trigger-execution-busy.json', '启动“夜间关灯”失败：它正在执行中。等执行完成后再试。', 'Failed to start “Night off”: it is running right now. Wait for it to finish, then try again.'],
        ['wire-trigger-service-not-admissible.json', '启动“夜间关灯”失败：它的服务当前的状态不允许启动（正在运行、正在启动，或状态未确认）。请先核验服务状态。',
            'Failed to start “Night off”: its service cannot be started in its current state (running, starting, or state unconfirmed). Verify the service state first.'],
        ['wire-trigger-not-found.json', '启动“夜间关灯”失败：设备上找不到这条规则。它可能已被删除，请刷新页面。', 'Failed to start “Night off”: the rule was not found on the device. It may have been deleted; refresh the page.'],
        ['wire-trigger-generic.json', '启动“夜间关灯”失败：设备没有返回具体原因。可在「终端」页打开「系统日志」查看这次操作的详细记录。',
            'Failed to start “Night off”: the device did not return a specific reason. Open Terminal → System Logs to see the details of this operation.'],
    ]) {
        test(`${language}: quick actions: when the device refuses (${file.replace('wire-trigger-', '').replace('.json', '')}) the sentence says why`, async () => {
            await withPage(language, quickDevice({}, fixture(file)), async ({page, calls}) => {
                const card = await quickPage(page);
                await card.waitFor();
                await card.locator('.quick-action-name').click();
                await expectToast(page, 'error', zh ? expectedZh : expectedEn);
                assert.equal(calls.filter(c => c.endpoint === 'automation/rules/trigger').length, 1);
                // the card is usable again and shows no stuck spinner
                await page.waitForFunction(() => !document.getElementById('quick-action-night-off').classList.contains('triggering'));
            });
        });
    }

    test(`${language}: quick actions: an accepted trigger keeps its plain success toast`, async () => {
        await withPage(language, quickDevice({}, fixture('wire-trigger-ok.json')), async ({page}) => {
            const card = await quickPage(page);
            await card.waitFor();
            await card.locator('.quick-action-name').click();
            await expectToast(page, 'success', await page.evaluate(() => t('toast.operationExecuted')));
        });
    });

    test(`${language}: quick actions: a second card too soon is told to wait, in a complete sentence`, async () => {
        await withPage(language, quickDevice({}, fixture('wire-trigger-ok.json')), async ({page}) => {
            await quickPage(page);
            await page.evaluate(() => { _quickActionLastTriggeredId = 'other'; _quickActionTriggerCooldownUntil = Date.now() + 5000; });
            await page.locator('#quick-action-night-off .quick-action-name').click();
            await expectToast(page, 'warning', zh ? '刚刚触发过其他快捷操作，请等几秒再试。' : 'Another quick action was just triggered. Wait a few seconds, then try again.');
        });
    });
}
