'use strict';
const RED = 'rgb(173, 27, 33)';   // --bad，校验错误红字（NativeDialogs.md F）
// 第 4 步验证：原生 confirm/alert/prompt 已被 sheet 取代。只连本机替身。
//   node tests/macos27-r2/native-dialogs.cjs
const L = require('./lib.cjs');
const path = require('node:path');
(async () => {
  const root = path.join(L.REPO, 'components/ts_webui/web');
  const browser = await L.launch();
  const fx = await L.startFixture(root, { extra: path.join(L.REPO, 'tests/macos27-r2/profile-board.json') });
  const res = []; const ok = (n, c, d = '') => { res.push([n, !!c, d]); console.log((c ? 'PASS ' : 'FAIL ') + n + (d ? '  ' + d : '')); };
  try {
    const { ctx, page } = await L.openPage(browser, fx.origin, '/', { width: 1440, height: 1000, lang: 'zh-CN', state: 'populated', role: 'root', wait: 2600 });
    const natives = []; const errs = [];
    page.on('dialog', d => { natives.push(d.type() + ':' + d.message()); d.dismiss(); });
    page.on('pageerror', e => errs.push(String(e)));
    const sheetInfo = () => page.evaluate(() => { const m = document.querySelector('.confirm-sheet'); if (!m) return null; const p = m.querySelector('button[data-r="1"]'); return { title: m.querySelector('.st').textContent, body: m.querySelector('.sb').innerText, primary: p.textContent, danger: p.classList.contains('bad'), buttons: [...m.querySelectorAll('.sf button')].map(b => b.textContent) }; });
    const click = r => page.evaluate(r => document.querySelector(`.confirm-sheet button[data-r="${r}"]`).click(), r);
    // 记录 api 调用
    await page.evaluate(() => { window.__calls = []; for (const k of new Set([...Object.keys(api), ...Object.getOwnPropertyNames(Object.getPrototypeOf(api))])) if (k !== 'constructor' && typeof api[k] === 'function') { const o = api[k].bind(api); api[k] = (...a) => { window.__calls.push(k + ':' + JSON.stringify(a).slice(0, 80)); return o(...a); }; } });
    const calls = () => page.evaluate(() => window.__calls.filter(x => !/\.list|"GET"/.test(x)));
    const reset = () => page.evaluate(() => { window.__calls.length = 0; });

    // 1. confirmSheet 基本行为
    const run = async (fn) => { await page.evaluate(fn); await page.waitForSelector('.confirm-sheet'); };
    for (const [name, act, want] of [['取消', () => click('0'), false], ['主按钮', () => click('1'), true], ['第三项', () => click('alt'), 'alt'], ['Esc', () => page.keyboard.press('Escape'), false], ['遮罩', () => page.mouse.click(5, 5), false]]) {
      await page.evaluate(() => { window.__r = 'pending'; confirmSheet({ title: 'T', body: 'B', primary: 'P', third: 'X' }).then(v => window.__r = v); });
      await page.waitForSelector('.confirm-sheet'); await act(); await page.waitForTimeout(50);
      ok('confirmSheet ' + name, JSON.stringify(await page.evaluate(() => window.__r)) === JSON.stringify(want), String(await page.evaluate(() => window.__r)));
    }
    await page.evaluate(() => { window.__r = 'pending'; confirmSheet({ title: 'T', body: 'B', primary: 'P' }).then(v => window.__r = v); });
    await page.waitForSelector('.confirm-sheet'); await page.keyboard.press('Enter'); await page.waitForTimeout(50);
    ok('confirmSheet 中性 Enter=确认', await page.evaluate(() => window.__r) === true);
    await page.evaluate(() => { window.__r = 'pending'; confirmSheet({ title: 'T', body: 'B', primary: 'P', tone: 'danger' }).then(v => window.__r = v); });
    await page.waitForSelector('.confirm-sheet'); await page.keyboard.press('Enter'); await page.waitForTimeout(50);
    ok('confirmSheet 红色 Enter 不确认（焦点在取消）', await page.evaluate(() => window.__r) === false);
    await page.evaluate(() => document.querySelector('.confirm-sheet')?.remove());

    // 2. 真实调用点：取消 → 无请求；确认 → 有请求
    const cases = [
      ['confirmReboot', "confirmReboot()", 'reboot', '重启', true],
      ['deleteFile', "deleteFile('/sdcard/x.txt')", 'storage', '删除', true],
      ['deleteRule', "deleteRule('r1')", '', '删除', true],
      ['deleteSource', "deleteSource('s1')", '', '删除', true],
      ['deleteAction', "deleteAction('a1')", '', '删除', true],
      ['unmountSdCard', "unmountSdCard()", '', '卸载', false],
      ['resetShutdownSettings', "resetShutdownSettings()", '', '恢复默认', false],
    ];
    for (const [name, call, , primaryText, danger] of cases) {
      await reset(); await page.evaluate(c => { window.eval('(' + 'async()=>{' + c + '})()'); }, call);
      await page.waitForSelector('.confirm-sheet');
      const info = await sheetInfo();
      ok(name + ' 弹出确认 sheet', info && info.primary.includes(primaryText) && info.danger === danger, JSON.stringify(info));
      await click('0'); await page.waitForTimeout(200);
      const c1 = await calls();
      ok(name + ' 取消：无请求', c1.length === 0, c1.join('|'));
      await page.evaluate(c => { window.eval('(' + 'async()=>{' + c + '})()'); }, call);
      await page.waitForSelector('.confirm-sheet'); await click('1'); await page.waitForTimeout(400);
      const c2 = await calls();
      ok(name + ' 确认：发出请求', c2.length > 0, c2.join('|'));
    }

    // 3. 导出指令三按钮
    await page.evaluate(() => { window._configPackStatus = { can_export: false }; window.__exp = []; window.doExportSshCommand = (id, cert, inc) => { window.__exp.push([id, cert, inc]); }; });
    for (const [b, want] of [['0', 0], ['alt', 1], ['1', 1]]) {
      await page.evaluate(() => { window.__exp.length = 0; window.eval('exportSshCommand("c1")'); });
      await page.waitForSelector('.confirm-sheet');
      const info = await sheetInfo();
      if (b === '0') ok('导出指令：三个按钮', info.buttons.length === 3, info.buttons.join('/'));
      await click(b); await page.waitForTimeout(100);
      const e = await page.evaluate(() => window.__exp);
      ok('导出指令 ' + b, e.length === want && (b === '0' || e[0][2] === (b === '1')), JSON.stringify(e));
    }

    // 4. TOFU 指纹
    await page.evaluate(() => { window._sshHostsList = [{ id: 'h', host: '10.0.0.1', port: 22, username: 'u' }]; window.__ex = 0; const o = api.call.bind(api); api.call = (n, p) => { if (n === 'ssh.exec') { window.__ex++; window.__last = p; return Promise.resolve(window.__ex === 1 ? { code: 1002, data: { fingerprint: 'SHA256:abcd' } } : { code: 0, data: {} }); } return o(n, p); }; });
    for (const b of ['0', '1']) {
      await page.evaluate(() => { window.__ex = 0; window.eval('testSshHostByIndex(0)'); });
      await page.waitForSelector('.confirm-sheet');
      const info = await sheetInfo();
      if (b === '0') ok('TOFU：显示主机与指纹，中性色', /10\.0\.0\.1:22/.test(info.body) && /SHA256:abcd/.test(info.body) && !info.danger && info.primary === '信任并连接', JSON.stringify(info));
      await click(b); await page.waitForTimeout(300);
      const n = await page.evaluate(() => window.__ex);
      ok('TOFU ' + (b === '1' ? '信任 → 再次 exec 带 confirmed_fingerprint' : '取消 → 不再 exec'), b === '1' ? (n === 2 && (await page.evaluate(() => window.__last.confirmed_fingerprint)) === 'SHA256:abcd') : n === 1, 'exec=' + n);
    }

    // 5. 查看完整指纹
    await page.evaluate(() => { window._knownHostsList = [{ host: '10.0.0.1', port: 22, type: 'ssh-ed25519', fingerprint: 'SHA256:zzzz' }]; showFullFingerprint(0); });
    const fp = await page.evaluate(() => document.getElementById('fingerprint-info-modal')?.innerText || '');
    ok('完整指纹 sheet', /SHA256:zzzz/.test(fp) && /ssh-ed25519/.test(fp) && /复制/.test(fp) && /关闭/.test(fp), fp.replace(/\s+/g, ' '));
    await page.evaluate(() => document.getElementById('fingerprint-info-modal').remove());

    // 6. WiFi 密码
    for (const [b, want] of [['cancel', 0], ['ok', 1]]) {
      await reset();
      await page.evaluate(() => connectWifi('Office-5G'));
      const title = await page.evaluate(() => document.querySelector('#wifi-connect-modal .st').textContent);
      if (b === 'cancel') ok('WiFi 密码 sheet 标题', title === '连接 Office-5G', title);
      const type = await page.evaluate(() => document.getElementById('wifi-connect-password').type);
      if (b === 'cancel') ok('WiFi 密码为安全输入', type === 'password');
      if (b === 'ok') await page.fill('#wifi-connect-password', 'pw123');
      await page.evaluate(b => document.querySelector(b === 'ok' ? '#wifi-connect-modal .btn.primary' : '#wifi-connect-modal .sf .btn:not(.primary)').click(), b);
      await page.waitForTimeout(200);
      const cs = (await calls()).filter(x => x.startsWith('wifiConnect'));
      ok('WiFi 连接 ' + b, cs.length === want && (b === 'cancel' || cs[0].includes('Office-5G') && cs[0].includes('pw123')), cs.join('|'));
    }
    ok('WiFi 弹窗已关闭', await page.evaluate(() => !document.getElementById('wifi-connect-modal')));

    // 7. 必填校验（内联）
    const val = async (label, route, open, submit, field) => {
      await page.goto(fx.origin + '/?state=populated&role=root&lang=zh-CN#' + route); await page.reload(); await page.waitForTimeout(1500);
      await page.evaluate(open); await page.waitForTimeout(400);
      await page.evaluate(submit); await page.waitForTimeout(200);
      const r = await page.evaluate(f => { const el = document.getElementById(f); return { err: el?.classList.contains('err'), msg: document.querySelector('.fe-msg')?.textContent, focus: document.activeElement === el, red: getComputedStyle(document.querySelector('.fe-msg') || document.body).color }; }, field);
      ok(label + ' 内联校验', r.err && r.msg && (r.focus || field === 'actions-container') && r.red === RED, JSON.stringify(r));
    };
    await val('数据源 ID', '/automation', 'showAddSourceModal()', 'submitAddSource()', 'source-id');
    await val('规则 ID', '/automation', 'showAddRuleModal()', 'submitAddRule()', 'rule-id');
    await page.evaluate(() => { document.getElementById('rule-id').value = 'x'; }); await page.evaluate('submitAddRule()'); await page.waitForTimeout(200);
    ok('规则名称 内联校验（红字）', await page.evaluate(RED => { const el = document.getElementById('rule-name'); const m = document.querySelector('.fe-msg'); return el.classList.contains('err') && !!m && getComputedStyle(m).color === RED; }, RED));
    await page.evaluate(() => { document.getElementById('rule-name').value = 'n'; }); await page.evaluate('submitAddRule()'); await page.waitForTimeout(200);
    ok('动作模板 内联校验（红字）', await page.evaluate(RED => { const m = document.querySelector('.fe-msg'); return !!m && getComputedStyle(m).color === RED; }, RED));
    await page.evaluate(() => { document.getElementById('rule-name').value = 'n2'; }); // 输入触发 clear
    await page.evaluate(() => document.getElementById('rule-name').dispatchEvent(new Event('input')));

    // 7b. 14 个必填校验点全部触发：字段带 .err、下方 .fe-msg 为红字（--bad）、焦点在字段上（actions-container 是分组，不聚焦）
    const checkErr = async (label, id) => {
      const r = await page.evaluate(([id, RED]) => {
        const el = document.getElementById(id); const m = document.querySelector('.fe-msg');
        return { err: !!el && el.classList.contains('err'), msg: !!m && m.textContent.length > 0, red: !!m && getComputedStyle(m).color === RED, focus: document.activeElement === el };
      }, [id, RED]);
      ok(label, r.err && r.msg && r.red && (r.focus || id === 'actions-container'), JSON.stringify(r));
    };
    const step = async (js) => { await page.evaluate(js); await page.waitForTimeout(150); };
    await page.goto(fx.origin + '/?state=populated&role=root&lang=zh-CN#/automation'); await page.reload(); await page.waitForTimeout(1500);
    await step('showAddSourceModal()'); await page.waitForTimeout(300);
    await step('testRestConnection()');                        await checkErr('校验 1/14 REST 测试：空地址', 'source-rest-url');
    await step("switchSourceType('websocket')"); await step('testWsConnection()');  await checkErr('校验 2/14 WS 测试：空地址', 'source-ws-uri');
    await step("switchSourceType('socketio')");  await step('testSioConnection()'); await checkErr('校验 3/14 SIO 测试：空地址', 'source-sio-url');
    await step("switchSourceType('rest')");      await step('submitAddSource()');   await checkErr('校验 4/14 提交：数据源 ID', 'source-id');
    await step(() => { document.getElementById('source-id').value = 'x'; }); await step('submitAddSource()'); await checkErr('校验 5/14 提交 REST：URL', 'source-rest-url');
    await step("switchSourceType('websocket')"); await step(() => { document.getElementById('source-id').value = 'x'; }); await step('submitAddSource()'); await checkErr('校验 6/14 提交 WS：URI', 'source-ws-uri');
    await step("switchSourceType('socketio')");  await step(() => { document.getElementById('source-id').value = 'x'; }); await step('submitAddSource()'); await checkErr('校验 7/14 提交 SIO：地址', 'source-sio-url');
    await step(() => { document.getElementById('source-sio-url').value = 'http://127.0.0.1:1'; }); await step('submitAddSource()'); await checkErr('校验 8/14 提交 SIO：事件名', 'source-sio-event');
    await step("switchSourceType('variable')"); await page.waitForTimeout(900);
    await step(() => { document.getElementById('source-id').value = 'x'; }); await step('submitAddSource()'); await checkErr('校验 9/14 提交 指令变量：主机', 'source-ssh-host');
    await step(() => { document.getElementById('source-ssh-host').value = 'fixture-host'; }); await step('submitAddSource()'); await checkErr('校验 10/14 提交 指令变量：指令未选', 'source-ssh-cmd');
    await step(() => { const s = document.getElementById('source-ssh-cmd'); s.insertAdjacentHTML('beforeend', '<option value="99">ghost</option>'); s.value = '99'; }); await step('submitAddSource()'); await checkErr('校验 11/14 提交 指令变量：指令不存在', 'source-ssh-cmd');
    await step(() => { document.getElementById('add-source-modal')?.remove(); }); await step('showAddRuleModal()'); await page.waitForTimeout(300);
    await step('submitAddRule()');                                                   await checkErr('校验 12/14 规则 ID', 'rule-id');
    await step(() => { document.getElementById('rule-id').value = 'r-x'; }); await step('submitAddRule()'); await checkErr('校验 13/14 规则名称', 'rule-name');
    await step(() => { document.getElementById('rule-name').value = 'n'; }); await step('submitAddRule()'); await checkErr('校验 14/14 至少一个动作模板', 'actions-container');

    ok('页面全程没有原生对话框', natives.length === 0, natives.join('|'));
    ok('页面无 JS 错误', errs.length === 0, errs.join('|'));
    await ctx.close();
  } finally { await fx.stop(); await browser.close(); }
  const bad = res.filter(r => !r[1]);
  console.log(`\n${res.length - bad.length}/${res.length} 通过`);
  process.exit(bad.length ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
