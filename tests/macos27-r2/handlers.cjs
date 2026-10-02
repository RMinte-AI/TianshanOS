'use strict';
// 第 6 步：DOM 级处理器等价性。HEAD 与当前各渲染 8 个路由并打开 dialogs.json 里的弹窗，收集 on* 属性里被调用的函数名；
// 当前版本相对 HEAD 不应缺函数名，且这些函数都必须存在于 window。只连本机替身。
//   node tests/macos27-r2/handlers.cjs --head /tmp/headweb
const L = require('./lib.cjs'); const path = require('node:path'); const fs = require('node:fs');
(async () => {
  const A = L.parseArgs();
  const cur = path.join(L.REPO, 'components/ts_webui/web');
  const dialogs = JSON.parse(fs.readFileSync(path.join(L.REPO, 'tests/macos27-r2/dialogs.json'), 'utf8'));
  const routes = ['/', '/network', '/files', '/terminal', '/automation', '/commands', '/security', '/ota'];
  const browser = await L.launch();
  const collect = async (root) => {
    const fx = await L.startFixture(root, { extra: path.join(L.REPO, 'tests/macos27-r2/profile-board.json') });
    const names = new Set(); const missing = new Set();
    try {
      for (const r of routes) for (const state of ['populated', 'provisioned']) {
        const { ctx, page } = await L.openPage(browser, fx.origin, r, { width: 1440, height: 1000, lang: 'zh-CN', state, role: 'root', wait: 1500 });
        const grab = () => page.evaluate(() => { const out = new Set(); document.querySelectorAll('*').forEach(e => { for (const a of e.attributes) if (/^on/.test(a.name)) for (const m of a.value.matchAll(/(?:^|[;\s(!{,=&|])([A-Za-z_$][\w$]*)\s*\(/g)) out.add(m[1]); }); return { names: [...out], miss: [...out].filter(n => typeof window[n] !== 'function' && !['if', 'function', 'return', 'event', 'confirm', 'alert', 'setTimeout', 'Number', 'parseInt', 'stopPropagation', 'preventDefault', 'click', 'remove', 'closest', 'focus', 'select', 'querySelector', 'getElementById', 'add', 'toggle', 'contains', 'stop', 'this', 'showToast'].includes(n)) }; });
        let g = await grab(); g.names.forEach(n => names.add(n)); g.miss.forEach(n => missing.add(n));
        for (const d of Object.values(dialogs)) {
          if (d.route !== r || (d.state && d.state !== state)) continue;
          try { if (d.pre) await page.evaluate(new Function(String(d.pre))); await page.evaluate(async c => { await Promise.race([(0, eval)(c), new Promise(x => setTimeout(x, 600))]); }, String(d.open)); await page.waitForTimeout(300); g = await grab(); g.names.forEach(n => names.add(n)); g.miss.forEach(n => missing.add(n)); } catch (e) { }
        }
        await ctx.close();
      }
    } finally { await fx.stop(); }
    return { names, missing };
  };
  const head = await collect(path.resolve(A.head)); const now = await collect(cur);
  await browser.close();
  const lost = [...head.names].filter(n => !now.names.has(n) && !(now.names.size && false));
  const gained = [...now.names].filter(n => !head.names.has(n));
  console.log('HEAD 处理器函数名', head.names.size, '当前', now.names.size);
  console.log('当前缺（相对 HEAD）:', lost.join(', ') || '无');
  console.log('当前新增:', gained.join(', ') || '无');
  console.log('当前 window 上不存在的处理器函数:', [...now.missing].join(', ') || '无');
  console.log('HEAD 自身 window 上不存在:', [...head.missing].join(', ') || '无');
})().catch(e => { console.error(e); process.exit(1); });
