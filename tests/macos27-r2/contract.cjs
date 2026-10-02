'use strict';
// 契约对比：HEAD 与当前工作区。功能、接口、路由、语言包 key 不得被改动（新增可以，删除/改值必须逐条说明）。
//   node contract.cjs [--against <web目录>] [--tag 名称]
// 检查项：函数名（多重集）、内联事件处理器（event:callee，多重集）、api.* 调用与 fetch 端点、路由表、
//         getElementById/querySelector('#id') 引用、i18n 用到的 key、语言包 key 与取值、zh/en 对称。
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const os = require('node:os');
const { execSync } = require('node:child_process');
const L = require('./lib.cjs');
const acorn = require(path.join(L.REPO, 'tests/prompts/node_modules/acorn'));

const A = L.parseArgs();
const cur = A.against ? path.resolve(A.against) : path.join(L.REPO, 'components/ts_webui/web');
const tag = A.tag || 'contract';

function headDir() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ts-r2-contract-'));
  execSync(`git -C ${L.REPO} archive HEAD components/ts_webui/web | tar -x -C ${tmp}`);
  return path.join(tmp, 'components/ts_webui/web');
}

const JS_FILES = ['js/app.js', 'js/api.js', 'js/router.js', 'js/terminal.js', 'js/dragSort.js', 'js/i18n.js'];

function walk(node, fn) {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) { node.forEach(n => walk(n, fn)); return; }
  if (typeof node.type === 'string') fn(node);
  for (const k of Object.keys(node)) { if (k === 'loc' || k === 'start' || k === 'end') continue; const v = node[k]; if (v && typeof v === 'object') walk(v, fn); }
}

function flatten(obj, prefix = '', out = {}) {
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? prefix + '.' + k : k;
    if (v && typeof v === 'object' && !Array.isArray(v)) flatten(v, key, out); else out[key] = typeof v === 'function' ? '[fn]' : JSON.stringify(v);
  }
  return out;
}

function loadLang(file) {
  const src = fs.readFileSync(file, 'utf8');
  let captured = null;
  const sandbox = { i18n: { registerLanguage: (c, o) => { captured = o; } } };
  vm.runInNewContext(src, sandbox, { timeout: 5000 });
  return flatten(captured || {});
}

function inc(m, k, n = 1) { m[k] = (m[k] || 0) + n; }

function extract(root) {
  const res = { functions: {}, handlers: {}, apiCalls: {}, fetches: {}, routes: {}, ids: {}, i18nUsed: {}, zh: {}, en: {} };
  const html = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
  const texts = [{ name: 'index.html', text: html }];
  for (const f of JS_FILES) { const p = path.join(root, f); if (fs.existsSync(p)) texts.push({ name: f, text: fs.readFileSync(p, 'utf8') }); }
  for (const { name, text } of texts) {
    if (name.endsWith('.js')) {
      try {
        const ast = acorn.parse(text, { ecmaVersion: 'latest', locations: false });
        walk(ast, n => {
          if (n.type === 'FunctionDeclaration' && n.id) inc(res.functions, name + ':' + n.id.name);
          if (n.type === 'VariableDeclarator' && n.id && n.id.name && n.init && /Function|Arrow/.test(n.init.type)) inc(res.functions, name + ':' + n.id.name);
        });
      } catch (e) { res.functions['PARSE-ERROR:' + name] = String(e.message); }
    } else {
      for (const m of text.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)) {
        if (!m[1].trim()) continue;
        try { walk(acorn.parse(m[1], { ecmaVersion: 'latest' }), n => { if (n.type === 'FunctionDeclaration' && n.id) inc(res.functions, name + '(inline):' + n.id.name); }); } catch (e) { res.functions['PARSE-ERROR:' + name] = String(e.message); }
      }
    }
    for (const m of text.matchAll(/\son(click|change|input|submit|keydown|keyup|keypress|focus|blur|dblclick|mouseover|mouseout|mousedown|mouseup|contextmenu|dragstart|dragover|drop|error|load)=\\?["']([^"']*)["']/g)) {
      const callees = [...m[2].matchAll(/([A-Za-z_$][\w$.]*)\s*\(/g)].map(x => x[1]).filter(x => !['if', 'function', 'return'].includes(x));
      for (const c of new Set(callees)) inc(res.handlers, `${m[1]}:${c}`);
    }
    for (const m of text.matchAll(/\bapi\.([a-zA-Z]+)\(/g)) inc(res.apiCalls, 'api.' + m[1]);
    for (const m of text.matchAll(/\bapi\.call\(\s*['"`]([^'"`]+)['"`]/g)) inc(res.apiCalls, 'call:' + m[1]);
    for (const m of text.matchAll(/\bfetch\(\s*['"`]([^'"`]+)['"`]/g)) inc(res.fetches, m[1].replace(/\$\{[^}]*\}/g, '${}'));
    for (const m of text.matchAll(/router\.register\(\s*['"]([^'"]+)['"]/g)) inc(res.routes, m[1]);
    for (const m of text.matchAll(/getElementById\(\s*['"`]([^'"`$]+)['"`]\s*\)/g)) inc(res.ids, m[1]);
    for (const m of text.matchAll(/querySelector(?:All)?\(\s*['"`]#([A-Za-z0-9_-]+)/g)) inc(res.ids, m[1]);
    for (const m of text.matchAll(/\b(?:t|certText|runtimeText|i18n\.t)\(\s*['"`]([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+)['"`]/g)) inc(res.i18nUsed, m[1]);
    for (const m of text.matchAll(/data-i18n(?:-[a-z]+)?="([A-Za-z0-9_.]+)"/g)) inc(res.i18nUsed, m[1]);
  }
  res.zh = loadLang(path.join(root, 'js/lang/zh-CN.js'));
  res.en = loadLang(path.join(root, 'js/lang/en-US.js'));
  return res;
}

function diffCount(a, b) {
  const removed = [], added = [], changed = [];
  for (const k of Object.keys(a)) { if (!(k in b)) removed.push(k); else if (a[k] !== b[k]) changed.push(`${k}: ${a[k]} → ${b[k]}`); }
  for (const k of Object.keys(b)) if (!(k in a)) added.push(k);
  return { removed: removed.sort(), added: added.sort(), changed: changed.sort() };
}

const head = extract(headDir());
const now = extract(cur);
const report = {};
let hardFail = 0;
for (const cat of ['functions', 'handlers', 'apiCalls', 'fetches', 'routes', 'ids', 'i18nUsed']) {
  report[cat] = diffCount(head[cat], now[cat]);
}
report.zhKeys = diffCount(head.zh, now.zh);
report.enKeys = diffCount(head.en, now.en);
const zhK = new Set(Object.keys(now.zh)), enK = new Set(Object.keys(now.en));
const hzK = new Set(Object.keys(head.zh)), heK = new Set(Object.keys(head.en));
const zhOnlyHead = new Set([...hzK].filter(k => !heK.has(k))), enOnlyHead = new Set([...heK].filter(k => !hzK.has(k)));
report.zhOnly = [...zhK].filter(k => !enK.has(k) && !zhOnlyHead.has(k)).sort();   // 只统计相对 HEAD 新出现的不对称
report.enOnly = [...enK].filter(k => !zhK.has(k) && !enOnlyHead.has(k)).sort();
report.asymAtHead = { zhOnly: zhOnlyHead.size, enOnly: enOnlyHead.size };
const langAll = new Set([...zhK, ...enK]);
report.usedButUndefined = Object.keys(now.i18nUsed).filter(k => !langAll.has(k) && !k.endsWith('.')).sort();
report.usedButUndefinedAtHead = Object.keys(head.i18nUsed).filter(k => !new Set([...Object.keys(head.zh), ...Object.keys(head.en)]).has(k)).sort();

const md = [`# 契约对比：HEAD ↔ 当前（${tag}）`, ''];
const sect = (name, d) => {
  md.push(`## ${name}：删除 ${d.removed.length}，新增 ${d.added.length}` + (d.changed ? `，改值/改次数 ${d.changed.length}` : ''));
  for (const x of d.removed.slice(0, 200)) md.push(`- ➖ ${x}`);
  for (const x of d.changed.slice(0, 200)) md.push(`- ✏️ ${x}`);
  for (const x of d.added.slice(0, 60)) md.push(`- ➕ ${x}`);
  if (d.added.length > 60) md.push(`- …另有 ${d.added.length - 60} 项新增`);
  md.push('');
};
for (const cat of ['functions', 'handlers', 'apiCalls', 'fetches', 'routes', 'ids', 'i18nUsed']) sect(cat, report[cat]);
sect('语言包 zh-CN key（含取值）', report.zhKeys);
sect('语言包 en-US key（含取值）', report.enKeys);
md.push(`## zh/en 新增不对称（HEAD 自带 仅zh ${zhOnlyHead.size} / 仅en ${enOnlyHead.size} 不计）：仅 zh ${report.zhOnly.length}，仅 en ${report.enOnly.length}`);
report.zhOnly.slice(0, 40).forEach(k => md.push(`- 仅 zh：${k}`)); report.enOnly.slice(0, 40).forEach(k => md.push(`- 仅 en：${k}`));
md.push('', `## 使用但未定义的 key：当前 ${report.usedButUndefined.length}（HEAD ${report.usedButUndefinedAtHead.length}）`);
const newUndefined = report.usedButUndefined.filter(k => !report.usedButUndefinedAtHead.includes(k));
newUndefined.forEach(k => md.push(`- 新增未定义：${k}`));
const outDir = L.mkdirp(path.join(L.OUT, 'contract'));
fs.writeFileSync(path.join(outDir, tag + '.json'), JSON.stringify(report, null, 1));
fs.writeFileSync(path.join(outDir, tag + '.md'), md.join('\n') + '\n');

// 汇总：删除项和语言包改值属“需逐条说明”，新增项属“需确认预期”
const removedTotal = ['functions', 'handlers', 'apiCalls', 'fetches', 'routes', 'ids', 'i18nUsed'].reduce((s, c) => s + report[c].removed.length, 0);
const langRemoved = report.zhKeys.removed.length + report.enKeys.removed.length;
const langChanged = report.zhKeys.changed.length + report.enKeys.changed.length;
console.log(`契约：删除项 ${removedTotal}（函数 ${report.functions.removed.length} / 处理器 ${report.handlers.removed.length} / api ${report.apiCalls.removed.length} / fetch ${report.fetches.removed.length} / 路由 ${report.routes.removed.length} / id ${report.ids.removed.length} / i18n 用法 ${report.i18nUsed.removed.length}）`);
console.log(`      语言包：删除 key ${langRemoved}，改值 ${langChanged}，新增 zh ${report.zhKeys.added.length} / en ${report.enKeys.added.length}，zh/en 新增不对称 ${report.zhOnly.length + report.enOnly.length}（HEAD 自带 ${zhOnlyHead.size + enOnlyHead.size}），新增未定义 key ${newUndefined.length}`);
console.log(`      新增：函数 ${report.functions.added.length}，处理器 ${report.handlers.added.length}，id ${report.ids.added.length}`);
console.log('报告', path.join(outDir, tag + '.md'));
process.exitCode = (removedTotal + langRemoved + langChanged + report.zhOnly.length + report.enOnly.length + newUndefined.length) ? 1 : 0;
