'use strict';
// 预算对照（Performance.md §四/§五/§七）：HEAD 与当前各起一个本机替身，在同一浏览器里量：
//   首访请求数与静态文件 gzip 字节、稳态 API 请求（20 秒）、DOM 节点数、同时可见的 backdrop-filter 层数、
//   仍在动画 box-shadow/width/height/backdrop-filter/all 的规则。只连 127.0.0.1，不碰真机。
//   node tests/macos27-r2/budget.cjs --head <HEAD 的 web 目录> [--steady 20]
// 说明：这里的请求数是「本机替身 + 固定夹具数据」下的数字，不代表真机。
const L = require('./lib.cjs');
const path = require('node:path');
const fs = require('node:fs');
const zlib = require('node:zlib');

const ROUTES = ['/', '/network', '/files', '/terminal', '/automation', '/commands', '/security', '/ota'];

async function measure(browser, origin, root, steady) {
  const out = { routes: {}, layers: {}, badAnim: [] };
  // 1) 首访请求（系统页）
  {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
    const page = await ctx.newPage();
    const stat = [], api = [];
    page.on('requestfinished', r => { const u = new URL(r.url()); (u.pathname.startsWith('/api/') ? api : stat).push(u.pathname); });
    await page.goto(L.pageUrl(origin, '/', {}));
    await page.waitForTimeout(3500);
    let gz = 0; const miss = [];
    for (const p of stat) {
      const f = path.join(root, p === '/' ? 'index.html' : p);
      try { gz += zlib.gzipSync(fs.readFileSync(f), { level: 9 }).length; } catch { miss.push(p); }
    }
    out.first = { staticCount: stat.length, apiCount: api.length, gz, miss: miss.length };
    // 2) 稳态：接着再观察 steady 秒的 API 请求
    const n0 = api.length;
    await page.waitForTimeout(steady * 1000);
    out.steady = { seconds: steady, api: api.length - n0 };
    await ctx.close();
  }
  // 3) 各路由 DOM 节点
  for (const r of ROUTES) {
    const { ctx, page } = await L.openPage(browser, origin, r, { wait: 2200 });
    out.routes[r] = await page.evaluate(() => document.querySelectorAll('*').length);
    if (r === '/') {
      // 4) 同时可见的 backdrop-filter 层数：常态 / 打开 sheet / 打开语言菜单
      const count = () => page.evaluate(() => {
        const vis = [];
        for (const el of document.querySelectorAll('*')) {
          const cs = getComputedStyle(el);
          const b = el.getBoundingClientRect();
          if (b.width < 2 || b.height < 2 || cs.visibility === 'hidden' || +cs.opacity === 0) continue;
          // 元素自己，以及它的 ::before / ::after（主内容面的毛玻璃画在 ::before 上）
          for (const [pe, cs2] of [['', cs], ['::before', getComputedStyle(el, '::before')], ['::after', getComputedStyle(el, '::after')]]) {
            const bf = cs2.backdropFilter || cs2.webkitBackdropFilter;
            if (!bf || bf === 'none') continue;
            if (pe && (cs2.content === 'none' || cs2.display === 'none')) continue;
            vis.push((el.id ? '#' + el.id : '') + '.' + String(el.className).split(/\s+/).slice(0, 2).join('.') + pe + ' ' + Math.round(b.width) + '×' + Math.round(b.height));
          }
        }
        return vis;
      });
      out.layers.idle = await count();
      await page.evaluate(() => { try { showShutdownSettingsModal(); } catch (e) {} });
      await page.waitForTimeout(700);
      out.layers.sheet = await count();
      await page.evaluate(() => { try { document.querySelector('.modal.active,.modal:not(.hidden)')?.remove(); } catch (e) {} });
      out.layers.afterClose = await count();
    }
    await ctx.close();
  }
  // 5) 动画/过渡里出现的禁用属性（读样式表规则；同源）
  {
    const { ctx, page } = await L.openPage(browser, origin, '/', { wait: 1500 });
    out.badAnim = await page.evaluate(() => {
      const bad = /(^|[\s,])(all|box-shadow|width|height|backdrop-filter|-webkit-backdrop-filter)([\s,]|$)/;
      const res = [];
      for (const ss of document.styleSheets) {
        let rules; try { rules = ss.cssRules; } catch { continue; }
        const walk = (list) => { for (const r of list) {
          if (r.cssRules && r.type !== 7) walk(r.cssRules);
          const st = r.style; if (!st) continue;
          const tp = st.transitionProperty || (st.transition || '');
          const an = st.animationName;
          if (tp && bad.test(tp.replace(/\d+(\.\d+)?m?s/g, ' '))) res.push('transition: ' + r.selectorText + ' → ' + tp.slice(0, 80));
          if (an && an !== 'none') res.push('animation: ' + r.selectorText + ' → ' + an);
        } };
        walk(rules);
      }
      // @keyframes 里动的属性
      for (const ss of document.styleSheets) {
        let rules; try { rules = ss.cssRules; } catch { continue; }
        for (const r of rules) if (r.type === 7) { const props = new Set(); for (const k of r.cssRules) for (const p of k.style) props.add(p); res.push('@keyframes ' + r.name + ' → ' + [...props].join(',')); }
      }
      return res;
    });
    await ctx.close();
  }
  return out;
}

(async () => {
  const A = L.parseArgs();
  if (!A.head) { console.error('缺 --head <HEAD 的 web 目录>'); process.exit(2); }
  const steady = Number(A.steady || 20);
  const headRoot = path.resolve(A.head), curRoot = path.join(L.REPO, 'components/ts_webui/web');
  const prof = path.join(L.REPO, 'tests/macos27-r2/profile-board.json');
  const browser = await L.launch();
  const fh = await L.startFixture(headRoot, { extra: prof }), fc = await L.startFixture(curRoot, { extra: prof });
  try {
    const h = await measure(browser, fh.origin, headRoot, steady);
    const c = await measure(browser, fc.origin, curRoot, steady);
    const row = (k, a, b) => console.log(k.padEnd(30), String(a).padStart(10), String(b).padStart(10), (typeof a === 'number' && typeof b === 'number') ? (b - a >= 0 ? '+' : '') + (b - a) : '');
    console.log('项目'.padEnd(30), 'HEAD'.padStart(10), '当前'.padStart(10), '差');
    row('首访静态文件数（系统页）', h.first.staticCount, c.first.staticCount);
    row('首访 API 请求数', h.first.apiCount, c.first.apiCount);
    row('首访静态 gzip 字节合计', h.first.gz, c.first.gz);
    row(`稳态 ${steady} 秒 API 请求`, h.steady.api, c.steady.api);
    for (const r of ROUTES) row('DOM 节点 ' + r, h.routes[r], c.routes[r]);
    for (const k of ['idle', 'sheet', 'afterClose']) row('可见 backdrop-filter 层 ' + k, h.layers[k].length, c.layers[k].length);
    console.log('\n当前可见 backdrop-filter 层：'); for (const k of ['idle', 'sheet']) console.log(' ', k, JSON.stringify(c.layers[k]));
    console.log('HEAD 可见 backdrop-filter 层：'); for (const k of ['idle', 'sheet']) console.log(' ', k, JSON.stringify(h.layers[k]));
    console.log('\n当前样式表里出现禁用属性的 transition / animation（' + c.badAnim.length + '）：'); c.badAnim.forEach(x => console.log('  ', x));
    console.log('HEAD（' + h.badAnim.length + '）：'); h.badAnim.forEach(x => console.log('  ', x));
    if (h.first.miss || c.first.miss) console.log(`\n注：有 ${h.first.miss}/${c.first.miss} 个静态请求没在磁盘上找到（gzip 合计不含）`);
  } finally { await fh.stop(); await fc.stop(); await browser.close(); }
})().catch(e => { console.error(e); process.exit(2); });
