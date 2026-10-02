'use strict';
// 表单提交等价性（差分测试）：HEAD 与当前各起一个本机替身，对同一批弹窗做「同样的自动填表 + 同样的提交调用」，
// 在网络层抓 /api/ 请求并逐项比较。目的：证明这轮把弹窗全部重写之后，点提交发给设备的内容与 HEAD 一致（功能不动）。
//   node tests/macos27-r2/payloads.cjs --head <HEAD 的 web 目录> [--only 名称片段] [--verbose] [--flip]   （--flip：复选框取反，覆盖另一半取值）
// 只连本机替身（127.0.0.1），不碰真实设备。
//
// 做法：
//  - 每个场景各开一个全新页面（HEAD 一个、当前一个），冻结 setInterval（去掉轮询噪音）。
//  - setup 打开弹窗（这一段的请求也记录，验证「打开时读的数据」一致）；随后对容器做确定性填表（按元素 id 生成值，两边一致）；
//    再依次执行 steps（提交）。HEAD 的原生对话框自动接受，当前的确认 sheet 自动点主按钮。
//  - 比较：请求的 方法 + 路径 + 归一化后的 body（键排序、时间戳/随机 id 抹平）。另可比较 report 表达式的返回值（如错误文字）。
const L = require('./lib.cjs');
const path = require('node:path');

// ---- 确定性自动填表（在页面里执行）。按 id 生成值，同一 id 在两个版本里得到同样的值；幂等，可跑多遍 ----
const FILL_SRC = `(sel, flip) => {
  const root = typeof sel === 'string' ? document.querySelector(sel) : document.body; if (!root) return 'no-root:' + sel;
  const H = s => { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) >>> 0; return h; };
  const fire = (el, ev) => el.dispatchEvent(new Event(ev, { bubbles: true }));
  let n = 0;
  for (const el of root.querySelectorAll('input,select,textarea')) {
    const id = el.id || el.name; if (!id || el.disabled || el.readOnly) continue;
    const t = (el.type || '').toLowerCase(); n++;
    if (el.tagName === 'SELECT') { const o = [...el.options].filter(x => x.value !== ''); if (o.length) { el.value = o[o.length - 1].value; fire(el, 'change'); } continue; }
    if (t === 'checkbox') { const want = (H(id) % 2 === 1) !== !!flip; if (el.checked !== want) { el.checked = want; fire(el, 'input'); fire(el, 'change'); } continue; }
    if (['radio', 'file', 'hidden', 'button', 'submit'].includes(t)) continue;
    if (t === 'range') { const mn = +el.min || 0, mx = el.max !== '' ? +el.max : 100; el.value = String(Math.round(mn + (mx - mn) * 0.37)); fire(el, 'input'); fire(el, 'change'); continue; }
    if (t === 'color') { el.value = '#123456'; fire(el, 'input'); fire(el, 'change'); continue; }
    if (t === 'number') { const mn = el.min !== '' ? +el.min : 0, mx = el.max !== '' ? +el.max : 1000; el.value = String(Math.round(mn + (mx - mn) * 0.3)); fire(el, 'input'); fire(el, 'change'); continue; }
    el.value = 'v_' + id; fire(el, 'input'); fire(el, 'change');
  }
  return n;
}`;

// ---- 场景表 ----
// route：进入的页面；state：populated | provisioned；setup：打开弹窗的 JS；fill：要填表的容器（字符串，或 {head, cur}）；
// steps：提交调用（依次执行）；files：[{sel, files:[{name,mimeType,text}]}]；report：执行后比较的表达式；head/cur：只对某个版本覆盖的字段
const C = (s) => `document.querySelector(${JSON.stringify(s)})`;
// 报告：某容器内所有带 id 的表单控件的当前值（只比 id，不比 class——两版的标记结构不同）
const FS = (sel) => `JSON.stringify(Object.fromEntries([...document.querySelectorAll(${JSON.stringify(sel + ' input,' + sel + ' select,' + sel + ' textarea')})].filter(e => e.id).map(e => [e.id, (e.type === 'checkbox' || e.type === 'radio') ? e.checked : e.value]).sort((a, b) => a[0] < b[0] ? -1 : 1)))`;
const SC = [
  // —— 安全页 ——
  { name: 'sec/keygen', route: '/security', setup: "showGenerateKeyModal()", fill: '#keygen-modal', steps: ['generateKey()'] },
  { name: 'sec/deploy', route: '/security', setup: "showDeployKeyModal('fixture-key')", fill: '#deploy-key-modal', steps: ['deployKey()'] },
  { name: 'sec/revoke-key', route: '/security', setup: "showRevokeKeyModal('fixture-key')", fill: '#revoke-key-modal', steps: ['revokeKey()'] },
  { name: 'sec/revoke-host', route: '/security', setup: "revokeKeyFromHost(0)", fill: '#revoke-host-modal', steps: ['doRevokeFromHost(0)'] },
  { name: 'sec/csr', route: '/security', setup: "showCertCSRModal()", fill: '#cert-csr-modal', steps: ['generateCSR()'] },
  { name: 'sec/cert-install', route: '/security', setup: "showCertInstallModal()", fill: '#cert-install-modal', steps: ['installCertificate()'] },
  { name: 'sec/cert-ca', route: '/security', setup: "showCertInstallCAModal()", fill: '#cert-ca-modal', steps: ['installCAChain()'] },
  { name: 'sec/cert-genkey', route: '/security', setup: "showCertGenKeyModal()", steps: ['generateCertKeypair()'] },
  { name: 'sec/cert-view', route: '/security', state: 'provisioned', setup: "showCertViewModal()", steps: [] },
  { name: 'sec/pack-verify-import', route: '/security', setup: "showConfigPackImportModal()", fill: '#pack-import-modal',
    steps: ["document.getElementById('pack-import-content').value='{\"tscfg_version\":\"1.0\"}'", 'verifyConfigPack()', 'importConfigPack()'] },
  { name: 'sec/pack-export', route: '/security', state: 'provisioned',
    setup: "showConfigPackExportModal();0", fill: '#pack-export-modal',
    steps: ["packExportSelectedFiles.set('/sdcard/config/a.json',{name:'a.json',content:'{\"a\":1}',status:'ok'});packExportUpdateSelectedDisplay();document.getElementById('pack-export-name').value='pk1'", 'exportConfigPack()'] },
  { name: 'sec/pack-list', route: '/security', setup: "showConfigPackListModal()", steps: ["document.getElementById('pack-list-path').value='/sdcard/x'", 'refreshConfigPackList()'] },
  { name: 'sec/ssh-host-export', route: '/security', setup: "showExportSshHostModal('fixture-host')", fill: '#export-ssh-host-modal', steps: ["doExportSshHostFromModal('fixture-host')"] },
  { name: 'sec/ssh-host-import', route: '/security', setup: "showImportSshHostModal()", files: [{ sel: '#import-ssh-host-file', files: [{ name: 'h.tscfg', mimeType: 'application/json', text: '{"tscfg":"x"}' }] }],
    fill: '#import-ssh-host-modal', steps: ['confirmSshHostImport()'], wait: 1400 },
  { name: 'sec/root-pw', route: '/security', setup: "0", fill: '#root-new-password,#root-confirm-password',
    steps: ["document.getElementById('root-new-password').value='abcd1';document.getElementById('root-confirm-password').value='abcd1'", 'submitRootPasswordSet()'] },
  { name: 'sec/root-pw-mismatch', route: '/security', expectNone: true, setup: "0", steps: ["document.getElementById('root-new-password').value='abcd1';document.getElementById('root-confirm-password').value='abcd2'", 'submitRootPasswordSet()'],
    report: "document.getElementById('root-password-error').textContent" },
  { name: 'sec/admin-pw', route: '/security', setup: "0", steps: ["document.getElementById('admin-new-password').value='abcd1';document.getElementById('admin-confirm-password').value='abcd1'", 'submitAdminPasswordSet()'] },
  { name: 'sec/admin-pw-short', route: '/security', expectNone: true, setup: "0", steps: ["document.getElementById('admin-new-password').value='ab';document.getElementById('admin-confirm-password').value='ab'", 'submitAdminPasswordSet()'],
    report: "document.getElementById('admin-password-error').textContent" },
  // —— 文件页 ——
  { name: 'files/newfolder', route: '/files', setup: "showNewFolderDialog()", fill: '#newfolder-modal', steps: ['createNewFolder()'] },
  { name: 'files/rename', route: '/files', setup: "showRenameDialog('/sdcard/fixture.txt','fixture.txt')", fill: '#rename-modal', steps: ['doRename()'] },
  { name: 'files/upload', route: '/files', setup: "showUploadDialog()", files: [{ sel: '#file-input', files: [{ name: 'up.txt', mimeType: 'text/plain', text: 'hello' }] }], steps: ['uploadFiles()'], wait: 1400 },
  // —— 系统页 ——
  { name: 'sys/shutdown', route: '/', setup: "showShutdownSettingsModal()", fill: '#shutdown-settings-modal', steps: ['saveShutdownSettings()'] },
  { name: 'sys/timezone-select', route: '/', setup: "showTimezoneModal()", steps: ["document.getElementById('timezone-select').value='JST-9'", 'applyTimezone()'] },
  { name: 'sys/timezone-custom', route: '/', setup: "showTimezoneModal()", steps: ["document.getElementById('timezone-custom').value='XYZ-3'", 'applyTimezone()'] },
  { name: 'sys/fan-curve', route: '/', setup: "showFanCurveModal(0)", fill: '#fan-curve-modal', steps: ['addCurvePoint()', 'applyFanCurve()'], wait: 1400 },
  { name: 'sys/fan-unbind', route: '/', setup: "showFanCurveModal(0)", steps: ['unbindTempVariable()'] },
  { name: 'sys/widget-add-save', route: '/', setup: "showWidgetManager();addWidgetFromPreset('cpu');0",
    fill: { head: '#dw-manager-main', cur: '#widget-edit-modal' },
    steps: ["saveWidgetEdit(dataWidgets[dataWidgets.length-1].id)"],
    report: "JSON.stringify(dataWidgets.map(w=>({...w,id:'ID'})))", wait: 1200 },
  { name: 'sys/widget-refresh-interval', route: '/', setup: "showWidgetManager()", steps: ["document.getElementById('dw-refresh-interval').value='10000';updateRefreshInterval()"] },
  { name: 'led/effect', route: '/', setup: "window.ledDevicesCache=[{name:'board',layout:'strip',effects:['rainbow','fire'],current:{animation:'fire',speed:50,color:{r:255,g:102,b:0}}}];ledStates.board=true;openLedModal('board','effect')",
    steps: ["selectEffectInModal('board','rainbow',document.querySelector('.effect-btn'))", "document.getElementById('modal-effect-speed-board').value='73';updateEffectSliderValue('board','73')", 'applyEffectFromModal(\'board\')'], wait: 1500 },
  { name: 'led/text', route: '/', setup: "window.ledDevicesCache=[{name:'matrix',layout:'matrix',effects:[],current:{}}];openLedModal('matrix','text')", fill: '#led-modal',
    steps: ["document.getElementById('modal-text-align').value='right'", 'displayTextFromModal()'] },
  { name: 'led/image', route: '/', setup: "window.ledDevicesCache=[{name:'matrix',layout:'matrix',effects:[],current:{}}];openLedModal('matrix','content')", fill: '#led-modal', steps: ['displayImageFromModal()'] },
  { name: 'led/qr', route: '/', setup: "window.ledDevicesCache=[{name:'matrix',layout:'matrix',effects:[],current:{}}];openLedModal('matrix','content')", fill: '#led-modal', steps: ['generateQrCodeFromModal()'] },
  { name: 'led/filter', route: '/', setup: "window.ledDevicesCache=[{name:'matrix',layout:'matrix',effects:[],current:{}}];openLedModal('matrix','filter');selectFilterInModal('blink',document.querySelector('.filter-btn[data-filter=blink]'))", fill: '#led-modal', steps: ['applyFilterFromModal()'] },
  { name: 'led/color-correction', route: '/', setup: "window.ledDevicesCache=[{name:'matrix',layout:'matrix',effects:[],current:{}}];openLedModal('matrix','colorcorrection')", wait: 1200,
    steps: ["for(const [id,v] of Object.entries({'cc-wp-r':80,'cc-wp-g':90,'cc-wp-b':110,'cc-gamma':150,'cc-brightness':120,'cc-saturation':60})){const e=document.getElementById(id);e.value=v;e.dispatchEvent(new Event('input',{bubbles:true}))}document.getElementById('cc-enabled').checked=true", 'applyColorCorrection()', 'ccExport()', 'ccImport()'] },
  { name: 'auth/login', route: '/', setup: "showLoginModal()", fill: '#login-modal', steps: ["document.getElementById('username').value='admin';document.getElementById('password').value='pw'", "document.getElementById('login-form').requestSubmit()"], wait: 1400 },
  { name: 'auth/pw-reminder', route: '/', setup: "showPasswordChangeReminder()", fill: '#password-change-modal', steps: ["document.getElementById('change-old-pwd').value='old1';document.getElementById('change-new-pwd').value='new12';document.getElementById('change-confirm-pwd').value='new12'", 'submitPasswordChange()'] },
  // —— 网络页 ——
  { name: 'net/ap-config', route: '/network', setup: "showApConfig()", fill: '#ap-config-modal', steps: ['applyApConfig()'] },
  { name: 'net/wifi-scan', route: '/network', setup: "showWifiScan()", steps: [] },
  { name: 'net/ap-stations', route: '/network', setup: "showApStations()", steps: [] },
  { name: 'net/dhcp', route: '/network', setup: "showDhcpClients()", steps: ["document.getElementById('dhcp-iface-select').value='eth'", 'loadDhcpClients()'] },
  // —— 自动化页 ——
  { name: 'auto/source-rest', route: '/automation', setup: "showAddSourceModal()", fill: '#add-source-modal',
    steps: ["document.getElementById('source-id').value='s_rest'", 'submitAddSource()'] },
  { name: 'auto/source-ws', route: '/automation', setup: "showAddSourceModal();switchSourceType('websocket')", fill: '#add-source-modal',
    steps: ["document.getElementById('source-id').value='s_ws'", 'submitAddSource()'] },
  { name: 'auto/source-sio', route: '/automation', setup: "showAddSourceModal();switchSourceType('socketio')", fill: '#add-source-modal',
    steps: ["document.getElementById('source-id').value='s_sio'", 'submitAddSource()'] },
  { name: 'auto/source-var', route: '/automation', setup: "showAddSourceModal();switchSourceType('variable')", fill: '#add-source-modal',
    steps: ["document.getElementById('source-ssh-host').value='fixture-host';onSshHostChangeForSource()", "document.getElementById('source-ssh-cmd').value='0';onSshCmdChange()", 'submitAddSource()'], wait: 1400 },
  { name: 'auto/rule-add', route: '/automation', setup: "showAddRuleModal()", fill: '#add-rule-modal', wait: 1400,
    steps: ["addConditionRow('fixture.value','gt','5')", "await addActionTemplateRow('fixture-action')", "document.getElementById('rule-id').value='r_new';document.getElementById('rule-name').value='Rule New'", 'submitAddRule()'] },
  { name: 'auto/rule-edit', route: '/automation', setup: "await editRule('fixture-rule')", wait: 1400, report: FS('#add-rule-modal'),
    steps: ["await new Promise(r => setTimeout(r, 900))", "document.getElementById('rule-name').value='Renamed'", "document.querySelector('#add-rule-modal button[onclick^=\"submitAddRule\"]').click()"] },
  { name: 'auto/action-cli', route: '/automation', setup: "showAddActionModal()", fill: '#action-modal', steps: ["document.getElementById('action-id').value='a_cli'", 'submitAction()'] },
  { name: 'auto/action-ssh', route: '/automation', setup: "showAddActionModal();document.querySelector('input[name=\"action-type\"][value=\"ssh_cmd_ref\"]').click()", fill: '#action-modal', wait: 900,
    steps: ["document.getElementById('action-id').value='a_ssh'", 'submitAction()'] },
  { name: 'auto/action-led', route: '/automation', setup: "showAddActionModal();document.querySelector('input[name=\"action-type\"][value=\"led\"]').click()", fill: '#action-modal', wait: 900,
    steps: ["document.getElementById('action-id').value='a_led'", 'submitAction()'] },
  { name: 'auto/action-log', route: '/automation', setup: "showAddActionModal();document.querySelector('input[name=\"action-type\"][value=\"log\"]').click()", fill: '#action-modal',
    steps: ["document.getElementById('action-id').value='a_log'", 'submitAction()'] },
  { name: 'auto/action-setvar', route: '/automation', setup: "showAddActionModal();document.querySelector('input[name=\"action-type\"][value=\"set_var\"]').click()", fill: '#action-modal',
    steps: ["document.getElementById('action-id').value='a_var'", 'submitAction()'] },
  { name: 'auto/action-webhook', route: '/automation', setup: "showAddActionModal();document.querySelector('input[name=\"action-type\"][value=\"webhook\"]').click()", fill: '#action-modal',
    steps: ["document.getElementById('action-id').value='a_hook'", 'submitAction()'] },
  { name: 'auto/export-rule', route: '/automation', setup: "showExportRuleModal('fixture-rule')", fill: '#export-rule-modal', steps: ["doExportRule('fixture-rule')"] },
  { name: 'auto/export-source', route: '/automation', setup: "showExportSourceModal('fixture-source')", fill: '#export-source-modal', steps: ["doExportSource('fixture-source')"] },
  { name: 'auto/export-action', route: '/automation', setup: "showExportActionModal('fixture-action')", fill: '#export-action-modal', steps: ["doExportAction('fixture-action')"] },
  { name: 'auto/import-rule', route: '/automation', setup: "showImportRuleModal()", files: [{ sel: '#import-rule-file', files: [{ name: 'r.tscfg', mimeType: 'application/json', text: '{"tscfg":"x"}' }] }],
    steps: ['confirmRuleImport()'], wait: 1400 },
  // —— 指令页 ——
  { name: 'auto/action-edit', route: '/automation', setup: "await editAction('fixture-action')", wait: 1200, report: FS('#action-modal'),
    steps: ["document.getElementById('action-name').value='Edited'", "document.querySelector('#action-modal button[onclick^=\"updateAction\"]').click()"] },
  { name: 'auto/import-source', route: '/automation', setup: "showImportSourceModal()", files: [{ sel: '#import-source-file', files: [{ name: 's.tscfg', mimeType: 'application/json', text: '{"tscfg":"x"}' }] }], steps: ['confirmSourceImport()'], wait: 1400 },
  { name: 'auto/import-action', route: '/automation', setup: "showImportActionModal()", files: [{ sel: '#import-action-file', files: [{ name: 'a.tscfg', mimeType: 'application/json', text: '{"tscfg":"x"}' }] }], steps: ['confirmActionImport()'], wait: 1400 },
  { name: 'auto/varselect-condition', route: '/automation', setup: "showAddRuleModal();addConditionRow('','eq','');await openConditionVarSelector(0)", wait: 900, steps: ["selectVariable('fixture.value')"], report: "document.querySelector('.cond-variable').value" },
  { name: 'auto/varselect-widget-expr', route: '/', setup: "showWidgetManager();addWidgetFromPreset('cpu');await selectVariableForWidget()", wait: 900, steps: ["selectVariable('fixture.value')"], report: "document.getElementById('edit-expression')?.value" },
  { name: 'cmd/export-sheet', route: '/commands', setup: "selectHost('fixture-host');showExportSshCommandModal('fixture-command')", fill: '#export-ssh-cmd-modal', steps: ["doExportSshCommandFromModal('fixture-command')"] },
  { name: 'cmd/import-sheet', route: '/commands', setup: "selectHost('fixture-host');await showImportSshCommandModal()", files: [{ sel: '#import-ssh-cmd-file', files: [{ name: 'c.tscfg', mimeType: 'application/json', text: '{"tscfg":"x"}' }] }], fill: '#import-ssh-cmd-modal', steps: ['confirmSshCommandImport()'], wait: 1400 },
  { name: 'cmd/add', route: '/commands', setup: "selectHost('fixture-host');showAddCommandModal()", fill: '#command-modal', wait: 1000,
    steps: ["document.getElementById('cmd-edit-id').value='c_new';document.getElementById('cmd-name').value='New cmd';document.getElementById('cmd-command').value='echo hi'", 'saveCommand()'] },
  { name: 'cmd/add-nohup-service', route: '/commands', setup: "selectHost('fixture-host');showAddCommandModal()", fill: '#command-modal', wait: 1000,
    steps: ["document.getElementById('cmd-edit-id').value='c_svc';document.getElementById('cmd-name').value='Svc';document.getElementById('cmd-command').value='run';const nh=document.getElementById('cmd-nohup');nh.checked=true;updateNohupState();const sm=document.getElementById('cmd-service-mode');sm.checked=true;updateServiceModeState();document.getElementById('cmd-var-name').value='svc_var';document.getElementById('cmd-ready-pattern').value='Running on'", 'saveCommand()'] },
  // —— 终端页：只加载（用来收集页面加载时的 getElementById 空引用） ——
  { name: 'term/load', route: '/terminal', expectNone: true, setup: "0", steps: [], wait: 1500 },
  // —— 固件升级页 ——
  { name: 'ota/server-save', route: '/ota', setup: "0", steps: ["document.getElementById('ota-server-input').value='http://10.0.0.9:57807'", 'saveOtaServer()'] },
  { name: 'ota/check', route: '/ota', setup: "0", steps: ["document.getElementById('ota-server-input').value='http://10.0.0.9:57807'", 'checkForUpdates()'] },
  { name: 'ota/from-url', route: '/ota', setup: "0", steps: ["document.getElementById('ota-url-input').value='http://x/fw.bin';document.getElementById('ota-url-include-www').checked=false;document.getElementById('ota-url-skip-verify').checked=true", 'otaFromUrl()'] },
  { name: 'ota/from-file', route: '/ota', setup: "0", steps: ["document.getElementById('ota-file-input').value='/sdcard/fw.bin';document.getElementById('ota-file-include-www').checked=true", 'otaFromFile()'] },
];

// ---- 归一化 ----
const stable = (v) => Array.isArray(v) ? v.map(stable) : (v && typeof v === 'object') ? Object.fromEntries(Object.keys(v).sort().map(k => [k, stable(v[k])])) : (typeof v === 'number' && v > 1e11) ? 'TS' : (typeof v === 'string' ? v.replace(/\b\d{12,}\b/g, 'TS').replace(/\b(w|widget|dw)_[a-z0-9]{5,}\b/gi, 'WID') : v);
const normBody = (raw, ct) => {
  if (raw == null || raw === '') return '';
  if (/multipart|octet/.test(ct || '')) return `[binary ${String(ct).split(';')[0]} ${raw.length}B]`;
  try { return JSON.stringify(stable(JSON.parse(raw))); } catch { return String(raw).replace(/\b\d{12,}\b/g, 'TS'); }
};

async function runOne(browser, origin, sc, ver, flip) {
  const s = { ...sc, ...(sc[ver] || {}) };
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
  // 冻结定时器；并记下所有「getElementById 取不到元素」的 id（两版对比：当前多出的空引用 = 可能丢了元素）
  await ctx.addInitScript(() => {
    window.setInterval = () => 0;
    window.__nullIds = new Set();
    const g = Document.prototype.getElementById;
    Document.prototype.getElementById = function (id) { const el = g.call(this, id); if (!el) window.__nullIds.add(String(id)); return el; };
  });
  const page = await ctx.newPage();
  const rec = { open: [], submit: [], natives: [], errs: [], report: null, filled: null, inline: [], nullIds: [] };
  let phase = null;
  page.on('request', r => {
    if (!phase) return; const u = new URL(r.url()); if (!u.pathname.startsWith('/api/')) return;
    const search = [...u.searchParams.entries()].filter(([k]) => !['_', 't', 'ts', 'nocache'].includes(k)).map(([k, v]) => k + '=' + v).join('&');
    rec[phase].push(`${r.method()} ${u.pathname.replace('/api/v1', '')}${search ? '?' + search : ''} ${normBody(r.postData(), r.headers()['content-type'])}`);
  });
  page.on('dialog', d => { rec.natives.push(d.type() + ':' + d.message().slice(0, 60)); d.accept(d.type() === 'prompt' ? 'pw123' : undefined); });
  page.on('pageerror', e => rec.errs.push(String(e).slice(0, 120)));
  await page.goto(L.pageUrl(origin, s.route, { state: s.state || 'populated', role: s.role || 'root', lang: 'zh-CN' }));
  await page.waitForTimeout(1800);
  // 当前版本的确认 sheet：自动点主按钮（HEAD 用原生 confirm，已在 dialog 事件里接受）
  await page.evaluate(() => { new MutationObserver(() => { const b = document.querySelector('.confirm-sheet button[data-r="1"]'); if (b) b.click(); }).observe(document.body, { childList: true, subtree: true }); });
  const exec = (code) => page.evaluate(async c => { try { await Promise.race([(0, eval)('(async()=>{' + c + '\n})()'), new Promise(r => setTimeout(r, 2500))]); } catch (e) { return 'ERR ' + String(e).slice(0, 100); } }, code);
  const wait = s.wait || 900;
  phase = 'open';
  const so = await exec(s.setup || '0'); if (so) rec.errs.push('setup:' + so);
  await page.waitForTimeout(700);
  for (const f of (s.files || [])) await page.setInputFiles(f.sel, f.files.map(x => ({ name: x.name, mimeType: x.mimeType, buffer: Buffer.from(x.text) })));
  if (s.files) await page.waitForTimeout(700);
  phase = 'submit';
  const fillSel = typeof s.fill === 'object' ? s.fill[ver === 'head' ? 'head' : 'cur'] : s.fill;
  if (fillSel) { for (let i = 0; i < 2; i++) { rec.filled = await page.evaluate(`(${FILL_SRC})(${JSON.stringify(fillSel)}, ${!!flip})`); await page.waitForTimeout(250); } }
  for (const st of (s.steps || [])) { const e = await exec(st); if (e) rec.errs.push('step:' + e); await page.waitForTimeout(400); }
  await page.waitForTimeout(wait);
  if (s.report) rec.report = await page.evaluate(s.report).catch(e => 'ERR ' + e);
  rec.inline = await page.evaluate(() => [...document.querySelectorAll('.fe-msg')].map(e => e.textContent.slice(0, 60)));
  rec.nullIds = await page.evaluate(() => [...(window.__nullIds || [])].sort());
  phase = null; await ctx.close(); return rec;
}

(async () => {
  const A = L.parseArgs();
  if (!A.head) { console.error('缺 --head <HEAD 的 web 目录>'); process.exit(2); }
  const headRoot = path.resolve(A.head), curRoot = path.join(L.REPO, 'components/ts_webui/web');
  const prof = path.join(L.REPO, 'tests/macos27-r2/profile-board.json');
  const browser = await L.launch();
  const fh = await L.startFixture(headRoot, { extra: prof }), fc = await L.startFixture(curRoot, { extra: prof });
  let diffs = 0, warns = 0, n = 0;
  try {
    for (const sc of SC) {
      if (A.only && !sc.name.includes(String(A.only))) continue;
      n++;
      const h = await runOne(browser, fh.origin, sc, 'head', !!A.flip), c = await runOne(browser, fc.origin, sc, 'cur', !!A.flip);
      const bag = arr => arr.slice().sort();
      const same = (a, b) => JSON.stringify(bag(a)) === JSON.stringify(bag(b));
      const sameOrder = (a, b) => JSON.stringify(a) === JSON.stringify(b);
      const problems = [];
      for (const ph of ['open', 'submit']) {
        if (!same(h[ph], c[ph])) {
          const hs = new Set(h[ph]), cs = new Set(c[ph]);
          problems.push(`  [${ph}] 只在 HEAD: ${[...hs].filter(x => !cs.has(x)).join('  ||  ') || '（数量不同）'}\n  [${ph}] 只在当前: ${[...cs].filter(x => !hs.has(x)).join('  ||  ') || '（数量不同）'}`);
        } else if (!sameOrder(h[ph], c[ph])) problems.push(`  [${ph}] 请求相同但顺序不同（HEAD ${h[ph].length} 个）`);
      }
      if (JSON.stringify(h.report) !== JSON.stringify(c.report)) problems.push(`  [report] HEAD=${JSON.stringify(h.report)}  当前=${JSON.stringify(c.report)}`);
      // HEAD 的 alert（必填校验）↔ 当前的字段内联错误：文案必须一致；其它原生对话框（prompt 等）当前不应出现
      const hAl = h.natives.filter(x => x.startsWith('alert:')).map(x => x.slice(6)), cIn = c.inline;
      if (JSON.stringify(hAl) !== JSON.stringify(cIn)) problems.push(`  [校验提示] HEAD 的 alert=${JSON.stringify(hAl)}  当前的内联错误=${JSON.stringify(cIn)}`);
      const cn = c.natives; if (cn.length) problems.push(`  [原生对话框] 当前版本仍弹出原生对话框: ${JSON.stringify(cn)}`);
      const nullOnlyC = c.nullIds.filter(x => !h.nullIds.includes(x)), nullOnlyH = h.nullIds.filter(x => !c.nullIds.includes(x));
      if (nullOnlyC.length) problems.push(`  [getElementById 空引用] 只在当前取不到元素: ${nullOnlyC.join(', ')}`);
      if (h.errs.length !== c.errs.length) problems.push(`  [页面/步骤错误] HEAD=${JSON.stringify(h.errs)}  当前=${JSON.stringify(c.errs)}`);
      const empty = h.submit.length === 0 && h.open.length === 0 && !sc.expectNone;
      if (empty) warns++;
      const head = `${problems.length ? 'DIFF' : 'OK  '} ${sc.name.padEnd(26)} 打开 ${String(h.open.length).padStart(2)}/${String(c.open.length).padStart(2)} 提交 ${String(h.submit.length).padStart(2)}/${String(c.submit.length).padStart(2)} 填表 ${h.filled}/${c.filled}${empty ? '  ⚠ HEAD 无请求（场景可能写错）' : ''}`;
      console.log(head); if (problems.length) { diffs++; console.log(problems.join('\n')); }
      if (A.verbose && nullOnlyH.length) console.log('   （仅 HEAD 取不到、当前取得到的 id：' + nullOnlyH.join(', ') + '）');
      if (A.verbose) { console.log('   HEAD 提交:', h.submit.join(' | ').slice(0, 600)); console.log('   report HEAD:', JSON.stringify(h.report), ' 当前:', JSON.stringify(c.report)); console.log('   HEAD errs:', JSON.stringify(h.errs), 'natives:', JSON.stringify(h.natives), '| 当前 errs:', JSON.stringify(c.errs), 'inline:', JSON.stringify(c.inline)); }
    }
  } finally { await fh.stop(); await fc.stop(); await browser.close(); }
  console.log(`\n共 ${n} 个场景：一致 ${n - diffs}，有差异 ${diffs}；HEAD 上无请求的场景 ${warns} 个`);
  process.exit(diffs ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
