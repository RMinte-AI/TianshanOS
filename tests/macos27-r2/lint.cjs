'use strict';
// 全局规则检查：页面用到的控件高度/圆角/字号/字重/颜色/阴影 必须落在“稿的词表”内；
// 另查 transition:all、backdrop-filter 层数、红色元素数（不得多于对应稿）。
//   node lint.cjs --build-vocab                      从全部 19 个稿生成 output/macos27-r2/design-vocab.json
//   node lint.cjs --root <web目录> [--routes /,/network,...] [--langs zh-CN] [--tag 名称] [--json]
const fs = require('node:fs');
const path = require('node:path');
const L = require('./lib.cjs');
const { extractLayout } = require('./extract.js');

const A = L.parseArgs();
const VOCAB = path.join(L.OUT, 'design-vocab.json');

const q = (v, s = 1) => Math.round(v * s) / s;
const hex = c => '#' + [c[0], c[1], c[2]].map(v => Math.round(v).toString(16).padStart(2, '0')).join('') + (c[3] < 0.995 ? '/' + q(c[3], 100) : '');
const isRed = c => c[3] > 0.35 && c[0] > 170 && c[1] < 115 && c[2] < 125;
const clampR = (r, b) => r.map(v => Math.min(v, Math.min(b.w, b.h) / 2));
const normShadow = s => s.replace(/\s+/g, '').toLowerCase();

function digest(layout) {
  const d = { controlHeights: {}, radii: {}, fontSizes: {}, weights: {}, textColors: {}, bgColors: {}, letterSpacings: {}, shadows: {}, families: {}, red: 0, backdrop: 0 };
  const add = (k, v) => { d[k][v] = (d[k][v] || 0) + 1; };
  for (const c of layout.controls) add('controlHeights', q(c.h, 2));
  for (const t of layout.tokens) {
    add('fontSizes', q(t.fs, 100)); add('weights', t.fw); add('textColors', hex(t.color)); add('letterSpacings', q(t.ls, 100)); add('families', t.ff);
  }
  const redEls = new Set();
  for (const t of layout.tokens) if (isRed(t.color)) redEls.add(t.el + '@' + Math.round(t.y / 20));
  for (const b of layout.boxes) {
    for (const r of clampR(b.r, b)) if (r > 0.5) add('radii', q(r, 2));
    if (b.bg[3] > 0.02) add('bgColors', hex(b.bg));
    if (b.sh) add('shadows', normShadow(b.sh));
    if (b.bf) d.backdrop++;
    if (isRed(b.bg) || (b.fill && b.tag === 'svg' && isRed(layoutParse(b.fill)))) redEls.add(b.sig + '@' + Math.round(b.y / 20));
  }
  d.red = redEls.size;
  return d;
}
function layoutParse(c) { const m = c.match(/rgba?\(([^)]+)\)/); if (!m) return [0, 0, 0, 0]; const p = m[1].split(/[ ,\/]+/).map(Number); return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1]; }

async function buildVocab(browser) {
  const files = fs.readdirSync(L.BOARDS).filter(f => f.endsWith('.html')).sort();
  const total = { controlHeights: {}, radii: {}, fontSizes: {}, weights: {}, textColors: {}, bgColors: {}, letterSpacings: {}, shadows: {}, families: {} };
  const perBoard = {};
  for (const f of files) {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
    const p = await ctx.newPage();
    await p.goto('file://' + path.join(L.BOARDS, f));
    await p.addStyleTag({ content: fs.readFileSync(L.QUANTIFY_CSS, 'utf8') });
    await p.addStyleTag({ content: L.FREEZE_CSS });
    await p.evaluate(() => document.fonts.ready);
    const lay = await p.evaluate(extractLayout, { root: null });
    const d = digest(lay);
    perBoard[f.replace('.html', '')] = { red: d.red, backdrop: d.backdrop, controls: lay.controls.length };
    for (const k of Object.keys(total)) for (const [v, n] of Object.entries(d[k])) total[k][v] = (total[k][v] || 0) + n;
    await ctx.close();
  }
  fs.writeFileSync(VOCAB, JSON.stringify({ built: new Date().toISOString(), boards: files.length, perBoard, vocab: total }, null, 1));
  const sk = o => Object.keys(o).sort((a, b) => parseFloat(a) - parseFloat(b));
  console.log('控件高度', sk(total.controlHeights).join(' '));
  console.log('圆角', sk(total.radii).join(' '));
  console.log('字号', sk(total.fontSizes).join(' '));
  console.log('字重', sk(total.weights).join(' '));
  console.log('字距', sk(total.letterSpacings).join(' '));
  console.log('字体族', Object.keys(total.families).join(' | '));
  console.log('文字色种类', Object.keys(total.textColors).length, '| 底色种类', Object.keys(total.bgColors).length, '| 阴影种类', Object.keys(total.shadows).length);
  console.log('各稿红色元素数', JSON.stringify(Object.fromEntries(Object.entries(perBoard).map(([k, v]) => [k, v.red]))));
  console.log('写入', VOCAB);
}

const near = (set, v, tol) => set.some(x => Math.abs(x - v) <= tol);
const colorNear = (list, c, tol) => list.some(x => Math.abs(x[0] - c[0]) + Math.abs(x[1] - c[1]) + Math.abs(x[2] - c[2]) + 255 * Math.abs(x[3] - c[3]) <= tol);
const parseHex = h => { const [a, al] = h.split('/'); return [parseInt(a.slice(1, 3), 16), parseInt(a.slice(3, 5), 16), parseInt(a.slice(5, 7), 16), al ? Number(al) : 1]; };

function lintLayout(lay, V, opts = {}) {
  const issues = [];
  const push = (kind, detail, n, sample) => issues.push({ kind, detail, n, sample });
  const V_h = Object.keys(V.controlHeights).map(Number);
  const V_r = Object.keys(V.radii).map(Number);
  const V_f = Object.keys(V.fontSizes).map(Number);
  const V_w = new Set(Object.keys(V.weights));
  const V_tc = Object.keys(V.textColors).map(parseHex);
  const V_bg = Object.keys(V.bgColors).map(parseHex);
  const V_ls = Object.keys(V.letterSpacings).map(Number);
  const V_sh = new Set(Object.keys(V.shadows));
  const V_ff = new Set(Object.keys(V.families));
  const grp = (arr, keyf, samplef) => { const m = new Map(); for (const x of arr) { const k = keyf(x); if (!m.has(k)) m.set(k, { n: 0, s: samplef(x) }); m.get(k).n++; } return m; };
  const ctl = lay.controls.filter(c => !(c.tag === 'input' && c.type === 'range'));
  for (const [k, v] of grp(ctl.filter(c => !near(V_h, c.h, 0.6)), c => q(c.h, 2), c => c.sig)) push('控件高度', `${k}px 不在稿的高度集`, v.n, v.s);
  for (const [k, v] of grp(lay.tokens.filter(t => !near(V_f, t.fs, 0.06)), t => q(t.fs, 100), t => `“${t.s}” ${t.el}`)) push('字号', `${k}px`, v.n, v.s);
  for (const [k, v] of grp(lay.tokens.filter(t => !V_w.has(t.fw)), t => t.fw, t => `“${t.s}” ${t.el}`)) push('字重', `${k}`, v.n, v.s);
  for (const [k, v] of grp(lay.tokens.filter(t => !V_ff.has(t.ff)), t => t.ff, t => `“${t.s}” ${t.el}`)) push('字体族', `${k}`, v.n, v.s);
  for (const [k, v] of grp(lay.tokens.filter(t => !near(V_ls, q(t.ls, 100), 0.06)), t => q(t.ls, 100), t => `“${t.s}” ${t.el}`)) push('字距', `${k}px`, v.n, v.s);
  for (const [k, v] of grp(lay.tokens.filter(t => !colorNear(V_tc, t.color, 10)), t => hex(t.color), t => `“${t.s}” ${t.el}`)) push('文字色', k, v.n, v.s);
  const rr = [];
  for (const b of lay.boxes) for (const r of clampR(b.r, b)) if (r > 0.5 && !near(V_r, q(r, 2), 0.6)) rr.push({ r: q(r, 2), sig: b.sig });
  for (const [k, v] of grp(rr, x => x.r, x => x.sig)) push('圆角', `${k}px`, v.n, v.s);
  for (const [k, v] of grp(lay.boxes.filter(b => b.bg[3] > 0.02 && !colorNear(V_bg, b.bg, 10)), b => hex(b.bg), b => b.sig)) push('底色', k, v.n, v.s);
  for (const [k, v] of grp(lay.boxes.filter(b => b.sh && !V_sh.has(normShadow(b.sh))), b => normShadow(b.sh).slice(0, 70), b => b.sig)) push('阴影', k, v.n, v.s);
  const bdf = lay.boxes.filter(b => b.bf);
  if (bdf.length > (opts.maxBackdrop ?? 3)) push('backdrop-filter', `可见层数 ${bdf.length} > ${opts.maxBackdrop ?? 3}`, bdf.length, bdf.map(b => b.sig).join(' , '));
  if (opts.maxRed !== undefined) {
    const d = digest(lay);
    if (d.red > opts.maxRed) push('红色元素', `${d.red} > 稿 ${opts.maxRed}`, d.red, '');
  }
  return issues;
}

async function runLint(browser) {
  const V = JSON.parse(fs.readFileSync(VOCAB, 'utf8'));
  const root = A.root ? path.resolve(A.root) : path.join(L.REPO, 'components/ts_webui/web');
  const routes = String(A.routes || '/,/network,/files,/terminal,/automation,/commands,/security,/ota').split(',');
  const langs = String(A.langs || 'zh-CN').split(',');
  const capBoard = { '/': 'Main', '/network': 'Net', '/files': 'Files', '/terminal': 'Term', '/automation': 'Auto', '/commands': 'Cmds', '/security': 'Sec', '/ota': 'Ota' };
  const fx = await L.startFixture(root, { extra: A.profile ? path.resolve(A.profile) : '' });
  const results = [];
  try {
    for (const lang of langs) for (const route of routes) {
      const { ctx, page } = await L.openPage(browser, fx.origin, route, { lang, width: 1440, height: 1000, wait: 2200 });
      await page.evaluate(() => { const m = document.getElementById('fixture-metrics'); if (m) m.setAttribute('data-r2-ignore', ''); });
      const lay = await page.evaluate(extractLayout, { root: null });
      const cap = V.perBoard[capBoard[route]];
      const issues = lintLayout(lay, V.vocab, { maxRed: cap ? cap.red : undefined, maxBackdrop: 3 });
      // 额外硬规则：transition: all（动画只允许 transform/opacity）
      const ta = await page.evaluate(() => { let n = 0; const s = []; for (const e of document.querySelectorAll('body *')) { const cs = getComputedStyle(e); if (cs.transitionProperty === 'all' && parseFloat(cs.transitionDuration) > 0) { n++; if (s.length < 3) s.push(e.tagName.toLowerCase() + '.' + String(e.className).split(' ')[0]); } } return { n, s }; });
      if (ta.n) issues.push({ kind: 'transition:all', detail: '动画只允许 transform/opacity', n: ta.n, sample: ta.s.join(' , ') });
      const bad = await page.evaluate(() => [...document.querySelectorAll('button,input:not([type=checkbox]):not([type=radio]):not([type=range]):not([type=hidden]),select,textarea')].filter(e => { const r = e.getBoundingClientRect(); if (!r.width) return false; const cs = getComputedStyle(e); return cs.borderTopStyle === 'outset' || cs.borderTopStyle === 'inset' || (cs.fontSize === '13.3333px'); }).length);
      if (bad) issues.push({ kind: '浏览器默认控件样式', detail: '存在未设样式的控件（默认 outset 边框或 13.333px 字号）', n: bad, sample: '' });
      results.push({ route, lang, nodes: lay.tokens.length, controls: lay.controls.length, issues });
      await ctx.close();
    }
  } finally { await fx.stop(); }
  const tag = A.tag || 'lint';
  const outDir = L.mkdirp(path.join(L.OUT, 'lint'));
  fs.writeFileSync(path.join(outDir, tag + '.json'), JSON.stringify(results, null, 1));
  let totalIssues = 0;
  const md = [`# 词表检查：${tag}`, ''];
  for (const r of results) {
    md.push(`## ${r.route}（${r.lang}）控件 ${r.controls}，问题类别 ${r.issues.length}`);
    for (const i of r.issues.sort((a, b) => b.n - a.n)) { totalIssues += 1; md.push(`- ${i.kind}：${i.detail} ×${i.n}  例：${i.sample}`); }
    md.push('');
  }
  md.unshift(`> 问题类别合计 ${totalIssues}`);
  fs.writeFileSync(path.join(outDir, tag + '.md'), md.join('\n') + '\n');
  if (A.json) console.log(JSON.stringify(results.map(r => ({ route: r.route, lang: r.lang, issues: r.issues.length })))); else console.log(md.slice(0, 60).join('\n'));
  console.log('输出', path.join(outDir, tag + '.md'));
  return totalIssues;
}

(async () => {
  const browser = await L.launch();
  try {
    if (A['build-vocab']) await buildVocab(browser); else { const n = await runLint(browser); process.exitCode = n ? 1 : 0; }
  } finally { await browser.close(); }
})().catch(e => { console.error('ERR', e.stack || e.message); process.exit(2); });
