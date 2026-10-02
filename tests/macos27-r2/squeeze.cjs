'use strict';
// 挤压/折行/重叠检测：真机数据（profile-real.json）下，逐路由 × 宽度 × 语言，找三类问题：
//   wrap   短标签/短数值被折成多行（如「固件」竖排、「27.04 V」折成两行）
//   overlap 同一父级下的卡片互相重叠；或卡片里的内容溢出到卡片外
//   clip   overflow:hidden 的元素内容被截断但没有 title（会悄悄丢信息）——只在 --clip 时报
// 之前的 overflow.cjs 只量整页横向溢出，量不到这些。
//   node tests/macos27-r2/squeeze.cjs [--widths 1100,1280,1440,1728,1920] [--langs zh-CN,en-US] [--routes /,/network] [--profile 文件] [--shot 目录]
// 只连本机替身（127.0.0.1）。有发现则退出码 1。
const L = require('./lib.cjs');
const path = require('node:path');

const A = L.parseArgs();
const WIDTHS = String(A.widths || '1100,1280,1440,1728,1920,2560,3440').split(',').map(Number);
const LANGS = String(A.langs || 'zh-CN,en-US').split(',');
const ROUTES = String(A.routes || '/,/network,/files,/terminal,/automation,/commands,/security,/ota').split(',');
const PROFILE = A.profile ? path.resolve(A.profile) : path.join(L.REPO, 'tests/macos27-r2/profile-real.json');

// 系统页里的服务型快捷动作需要 nohup 服务命令；夹具的 rules.get 对所有规则返回同一份，这里统一按服务型渲染（最坏情况）
const PREP = {
  // 系统页：服务型快捷动作按最坏情况渲染；风扇切到「智能（auto）」并带满遥测（安全参考/预测/升温速度），检查窄列里不折行
  '/': `window.__svcTiles = true; await refreshQuickActions(); updateFanInfo({ temperature: 41.5, temp_valid: true, fans: [{ id: 0, mode: 'auto', duty: 62, target_duty: 62, rpm: 3120, auto_state: 'active', guard_temperature: 44, predicted_temperature: 43.2, slope_c_per_min: 0.8 }] }); await new Promise(r => setTimeout(r, 400));`,
};

const CHECK = () => {
  const out = [];
  const vis = e => { const r = e.getBoundingClientRect(); const cs = getComputedStyle(e); return r.width > 0 && r.height > 0 && cs.visibility !== 'hidden' && cs.display !== 'none'; };
  const sig = e => (e.id ? '#' + e.id : '') + '.' + String(e.className || '').split(/\s+/).filter(Boolean).slice(0, 2).join('.');
  // 1) wrap：文字短（≤14 字）的元素，用 Range 数行盒（按 top 去重），多于 1 行即折行
  const main = document.getElementById('page-content') || document.body;
  const seen = new Set();
  const lines = e => { const rg = document.createRange(); rg.selectNodeContents(e); const tops = new Set(); for (const r of rg.getClientRects()) if (r.width > 0.5 && r.height > 0.5) tops.add(Math.round(r.top / 3)); return tops.size; };
  for (const e of main.querySelectorAll('*')) {
    if (e.children.length && !(e.children.length === 1 && e.children[0].tagName === 'SVG')) continue;
    const txt = (e.textContent || '').trim();
    if (!txt || txt.length > 18 || !vis(e)) continue;
    if (e.closest('.term, .xterm, pre, textarea, select, option, script, style, svg')) continue;
    if (lines(e) > 1) { const k = txt + '|' + sig(e); if (!seen.has(k)) { seen.add(k); out.push({ kind: 'wrap', where: sig(e), text: txt, h: lines(e) + '行' }); } }
  }
  // 2) overlap：同父级的兄弟卡片相交；内容超出卡片右缘
  const cards = [...main.querySelectorAll('.card, .grp, .w-tile, .quick-action-card')].filter(vis);
  const byParent = new Map();
  for (const c of cards) { const p = c.parentElement; if (!byParent.has(p)) byParent.set(p, []); byParent.get(p).push(c); }
  for (const [, list] of byParent) for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
    const a = list[i].getBoundingClientRect(), b = list[j].getBoundingClientRect();
    const w = Math.min(a.right, b.right) - Math.max(a.left, b.left), h = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
    if (w > 2 && h > 2) out.push({ kind: 'overlap', where: sig(list[i]) + ' × ' + sig(list[j]), text: Math.round(w) + '×' + Math.round(h) });
  }
  for (const c of cards) {
    if (c.classList.contains('quick-action-card') || c.classList.contains('w-tile')) continue;
    const cr = c.getBoundingClientRect();
    for (const e of c.querySelectorAll('*')) {
      if (!e.children.length && !(e.textContent || '').trim()) continue;
      if (!vis(e)) continue;
      // 位于可滚动祖先（表格滚动容器等）里的不算
      let sc = false; for (let p = e.parentElement; p && p !== c; p = p.parentElement) { const o = getComputedStyle(p).overflowX; if (o === 'auto' || o === 'scroll' || o === 'hidden') { sc = true; break; } }
      if (sc) continue;
      const r = e.getBoundingClientRect();
      if (r.right > cr.right + 1.5 && r.width > 4) { out.push({ kind: 'overflow', where: sig(c) + ' ▸ ' + sig(e), text: (e.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 24) + '（超出 ' + Math.round(r.right - cr.right) + 'px）' }); break; }
    }
  }
  // 2b) clip：被省略号截断、且自己和祖先都没有 title（信息被悄悄丢掉）
  for (const e of main.querySelectorAll('*')) {
    if (!vis(e) || e.children.length > 1) continue;
    const cs = getComputedStyle(e);
    if (cs.textOverflow !== 'ellipsis' || cs.overflow === 'visible') continue;
    if (e.scrollWidth <= e.clientWidth + 1) continue;
    let titled = false; for (let p = e; p && p !== main; p = p.parentElement) if (p.getAttribute && p.getAttribute('title')) { titled = true; break; }
    if (!titled) out.push({ kind: 'clip', where: sig(e), text: (e.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 28) });
  }
  // 2c) align：同页多个 .pwrow（改密码）的各列左边缘要对齐
  { const rows = [...main.querySelectorAll('.pwrow')].filter(vis); if (rows.length > 1) {
    const cols = rows.map(r => [...r.children].map(c => Math.round(c.getBoundingClientRect().left)));
    for (let k = 1; k < cols.length; k++) for (let i = 0; i < Math.min(cols[0].length, cols[k].length); i++) if (Math.abs(cols[0][i] - cols[k][i]) > 1) out.push({ kind: 'align', where: '.pwrow 第' + (i + 1) + '列', text: cols[0][i] + ' ≠ ' + cols[k][i] });
  } }
  // 3) 系统页第二行：并排时两张卡等高；卡片内不应有大块空白
  const mid = document.querySelector('.sys-mid');
  if (mid && mid.children.length === 2) {
    const [c1, c2] = [...mid.children]; const b1 = c1.getBoundingClientRect(), b2 = c2.getBoundingClientRect();
    if (Math.abs(b1.top - b2.top) < 2 && Math.abs(b1.height - b2.height) > 1.5) out.push({ kind: 'height', where: '.sys-mid', text: Math.round(b1.height) + ' ≠ ' + Math.round(b2.height) });
    for (const c of [c1, c2]) {
      const cb = c.getBoundingClientRect(), pad = parseFloat(getComputedStyle(c).paddingBottom) || 0;
      let low = cb.top; for (const e of c.querySelectorAll('*')) { if (!vis(e)) continue; const r = e.getBoundingClientRect(); if (e.children.length === 0 || getComputedStyle(e).backgroundColor !== 'rgba(0, 0, 0, 0)') low = Math.max(low, r.bottom); }
      const blank = cb.bottom - pad - low;
      if (blank > 72) out.push({ kind: 'blank', where: sig(c), text: '底部空白 ' + Math.round(blank) + 'px' });
    }
  }
  return out;
};

// 4) jump：风扇卡在 手动 / 关闭 / 曲线 / 智能（无遥测、带满遥测）之间切换时，状态标签、大数字、模式按钮、滑块、测试行的位置必须不变（否则切换时界面会「一跳一跳」）
const JUMP = `(async () => {
  const mk = m => ({ temperature: 41.5, temp_valid: true, fans: [m] });
  const cases = {
    manual: { id: 0, mode: 'manual', duty: 19, target_duty: 19, rpm: 0 },
    off: { id: 0, mode: 'off', duty: 0, target_duty: 0, rpm: 0 },
    curve: { id: 0, mode: 'curve', duty: 35, target_duty: 35, rpm: 1500 },
    smart: { id: 0, mode: 'auto', duty: 40, target_duty: 40, rpm: 1800, auto_state: 'baseline' },
    smartFull: { id: 0, mode: 'auto', duty: 62, target_duty: 62, rpm: 3120, auto_state: 'active', guard_temperature: 44, predicted_temperature: 43.2, slope_c_per_min: 0.8 },
  };
  const sels = { state: '.fan-card .fan-head .state', big: '.fan-card .fan-big', seg: '.fan-card .seg', slider: '.fan-card .slrow', test: '.fan-test' };
  const pos = {};
  for (const [name, c] of Object.entries(cases)) {
    updateFanInfo(mk(c)); await new Promise(r => setTimeout(r, 60));
    const base = document.querySelector('.fan-panel').getBoundingClientRect().top;
    pos[name] = Object.fromEntries(Object.entries(sels).map(([k, s]) => { const e = document.querySelector(s); return [k, e ? Math.round((e.getBoundingClientRect().top - base) * 10) / 10 : null]; }));
  }
  const out = [];
  for (const k of Object.keys(sels)) { const vals = Object.entries(pos).map(([n, p]) => [n, p[k]]); const ref = vals[0][1]; for (const [n, v] of vals) if (ref !== null && v !== null && Math.abs(v - ref) > 1) out.push(k + ': ' + vals[0][0] + '=' + ref + ' ≠ ' + n + '=' + v); }
  return out;
})()`;

(async () => {
  const browser = await L.launch();
  const fx = await L.startFixture(path.join(L.REPO, 'components/ts_webui/web'), { extra: PROFILE });
  let total = 0; const rows = [];
  try {
    for (const lang of LANGS) for (const w of WIDTHS) for (const r of ROUTES) {
      const { ctx, page } = await L.openPage(browser, fx.origin, r, { width: w, height: 900, lang, wait: 2400 });
      if (PREP[r]) {
        await page.evaluate(new Function('return (async()=>{' + `
          const orig = window.checkRuleHasNohupSsh;
          window.checkRuleHasNohupSsh = async (rule) => rule.name === '清理显存' ? null : ({ logFile: '/tmp/x.log', pidFile: '/tmp/x.pid', keyword: 'x', progName: 'x', hostId: 'agx0', cmdName: rule.name, commandId: 'cmd-' + rule.id, serviceMode: true, varName: '', readyPattern: '', serviceFailPattern: '' });
          ` + PREP[r] + '})()')).catch(() => {});
        await page.waitForTimeout(500);
      }
      const found = await page.evaluate(CHECK);
      if (r === '/') { const jumps = await page.evaluate(JUMP).catch(e => ['脚本出错 ' + e]); for (const j of jumps) found.push({ kind: 'jump', where: '.fan-panel', text: j }); }
      if (A.shot && found.length) { await page.screenshot({ path: path.join(A.shot, `${lang}-${w}-${r.replace(/\//g, '_') || 'root'}.png`), fullPage: true }); }
      for (const f of found) rows.push(`${lang} ${String(w).padStart(4)} ${r.padEnd(12)} ${f.kind.padEnd(8)} ${f.where}  「${f.text}」${f.h ? ' h=' + f.h : ''}`);
      total += found.length; await ctx.close();
    }
  } finally { await fx.stop(); await browser.close(); }
  console.log(rows.join('\n'));
  console.log(`\n挤压/折行/重叠：${total} 处（${LANGS.join(',')} × ${WIDTHS.join(',')} × ${ROUTES.length} 路由）`);
  process.exit(total ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
