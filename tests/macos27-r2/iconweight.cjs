'use strict';
// 图标结构与粗细断言（2026-10-03 起图标换成 Phosphor Regular，见 icons_from_phosphor.py 与 phosphor-map.json）。
// ①精灵：82 个 symbol，id 与对应表一致；对应表里有 Phosphor 名字的必须是 256 栅格 + <g fill="currentColor" stroke="none">；
//   保留自绘的（路由器、实心圆点）必须是 16 栅格；任何 symbol 都不能自带 stroke-width（有的话会盖过 .i 的描边，粗细不一致）。
// ②8 个路由上所有 <svg><use href="#ri-…">：尺寸只能是 16 或 24；自绘（16 栅格）的描边型图标，计算描边必须是 1。
// ③82 个 symbol 逐个以 32px 渲染：包围盒非空、没有超出画布。
// boardcss.cjs 把稿一侧的 .i 描边也改成同值再比，所以产品侧的实际结构由本脚本兜底。用法：
//   node iconweight.cjs [--web <web 目录，默认源码>] [--lang zh-CN]
const fs = require('node:fs');
const path = require('node:path');
const L = require('./lib.cjs');
const A = L.parseArgs();
const web = A.web ? path.resolve(A.web) : path.join(L.REPO, 'components/ts_webui/web');
const lang = A.lang || 'zh-CN';
const ROUTES = '/,/network,/files,/terminal,/automation,/commands,/security,/ota'.split(',');
const MAP = JSON.parse(fs.readFileSync(path.join(__dirname, 'phosphor-map.json'), 'utf8'));

(async () => {
  const bad = [];
  // ① 精灵静态检查
  const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
  const symbols = [...html.matchAll(/<symbol id="(ri-[^"]+)" viewBox="([^"]*)">([\s\S]*?)<\/symbol>/g)].map(m => ({ id: m[1], vb: m[2], body: m[3] }));
  const ids = new Set(symbols.map(s => s.id));
  const want = Object.keys(MAP.icons);
  const miss = want.filter(i => !ids.has(i)), extra = [...ids].filter(i => !MAP.icons[i]);
  let phosphor = 0, own = 0;
  for (const s of symbols) {
    const m = MAP.icons[s.id]; if (!m) continue;
    if (/stroke-width/.test(s.body)) bad.push(`${s.id}：symbol 自带 stroke-width`);
    if (m.phosphor) {
      phosphor++;
      if (s.vb !== '0 0 256 256') bad.push(`${s.id}：应是 256 栅格，实际 viewBox=${s.vb}`);
      if (!s.body.startsWith('<g fill="currentColor" stroke="none">')) bad.push(`${s.id}：Phosphor 图形缺少 <g fill="currentColor" stroke="none"> 外层`);
    } else {
      own++;
      if (s.vb !== '0 0 16 16') bad.push(`${s.id}：自绘图标应是 16 栅格，实际 viewBox=${s.vb}`);
    }
  }
  console.log(`精灵 symbol ${symbols.length} 个：Phosphor ${phosphor}，自绘 ${own}${miss.length ? '；表里有精灵里没有 ' + miss : ''}${extra.length ? '；精灵里有表里没有 ' + extra : ''}`);
  if (miss.length || extra.length || symbols.length !== 82) bad.push(`精灵与对应表不一致（symbol ${symbols.length} 个）`);

  // ②③ 页面
  const browser = await L.launch();
  const fx = await L.startFixture(web, { extra: path.join(L.REPO, 'tests/macos27-r2/profile-real.json') });
  const agg = {};
  let total = 0;
  for (const r of ROUTES) {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 2 });
    const page = await ctx.newPage();
    await page.goto(L.pageUrl(fx.origin, r, { lang }));
    await page.waitForTimeout(r === '/' ? 2600 : 1800);
    const rows = await page.evaluate(() => [...document.querySelectorAll('svg')].filter(s => s.querySelector('use')).map(s => {
      const bb = s.getBoundingClientRect();
      if (!bb.width || !bb.height) return null;
      const href = s.querySelector('use').getAttribute('href') || '';
      const sym = document.querySelector(href);
      return { w: Math.round(bb.width), sw: parseFloat(getComputedStyle(s).strokeWidth) || 0, id: href.slice(1), vb: sym ? sym.getAttribute('viewBox') : null };
    }).filter(Boolean));
    for (const x of rows) {
      total++;
      const grid = x.vb === '0 0 256 256' ? 'Phosphor' : '自绘';
      const k = `${x.w}px ${grid}${grid === '自绘' ? ' 描边 ' + x.sw : ''}`;
      agg[k] = (agg[k] || 0) + 1;
      if (x.vb === null) bad.push(`${r} ${x.id}：精灵里没有这个 symbol`);
      else if (x.w !== 16 && x.w !== 24) bad.push(`${r} ${x.id}：出现 ${x.w}px 图标（只应有 16/24）`);
      else if (grid === '自绘' && Math.abs(x.sw - 1) > 0.001) bad.push(`${r} ${x.id}：自绘图标描边 ${x.sw}，应为 1`);
    }
    if (r === '/') {
      const empty = await page.evaluate((ids) => {
        const out = [];
        for (const id of ids) {
          const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
          svg.setAttribute('width', 32); svg.setAttribute('height', 32); svg.setAttribute('class', 'i');
          svg.style.position = 'absolute'; svg.style.left = '-100px';
          const use = document.createElementNS('http://www.w3.org/2000/svg', 'use'); use.setAttribute('href', '#' + id);
          svg.appendChild(use); document.body.appendChild(svg);
          const b = use.getBBox();
          if (!(b.width > 4 && b.height > 4 && b.width <= 32.5 && b.height <= 32.5)) out.push(`${id}(${b.width.toFixed(1)}×${b.height.toFixed(1)})`);
          svg.remove();
        }
        return out;
      }, want);
      console.log(`逐个渲染 ${want.length} 个 symbol（32px）：${empty.length ? '异常 ' + empty.join(' ') : '包围盒全部正常'}`);
      if (empty.length) bad.push('渲染异常：' + empty.join(' '));
    }
    await ctx.close();
  }
  await fx.stop();
  await browser.close();
  for (const [k, n] of Object.entries(agg).sort((a, b) => b[1] - a[1])) console.log(`  ${k}：${n} 个`);
  if (!total) bad.push('一个图标都没量到');
  console.log(bad.length ? `❌ ${bad.length} 处不符：\n  ` + bad.slice(0, 20).join('\n  ') : `✅ ${total} 个页面图标、82 个 symbol 全部符合（16/24px；自绘描边 1；Phosphor 带填充外层）`);
  process.exitCode = bad.length ? 1 : 0;
})();
