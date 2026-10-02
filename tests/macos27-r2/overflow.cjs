'use strict';
// 第 6 步：各路由在多个宽度、多语言下是否横向溢出（只连本机替身）
//   node overflow.cjs [--widths 390,768,1024,1280] [--langs zh-CN,en-US]
const L = require('./lib.cjs'); const path = require('node:path');
(async () => {
  const root = path.join(L.REPO, 'components/ts_webui/web');
  const browser = await L.launch();
  const fx = await L.startFixture(root, { extra: path.join(L.REPO, 'tests/macos27-r2/profile-board.json') });
  const A = L.parseArgs(); const widths = (A.widths || '390,768,1024,1280').split(',').map(Number); const langs = String(A.langs || 'zh-CN').split(',');
  const routes = ['/', '/network', '/files', '/terminal', '/automation', '/commands', '/security', '/ota'];
  let bad = 0;
  try {
    for (const lang of langs) for (const w of widths) for (const r of routes) {
      const { ctx, page } = await L.openPage(browser, fx.origin, r, { width: w, height: 900, lang, state: 'populated', role: 'root', wait: 1800 });
      const o = await page.evaluate(() => { const W = innerWidth; const off = []; document.querySelectorAll('body *').forEach(e => { const b = e.getBoundingClientRect(); if (b.width && b.right > W + 1 && getComputedStyle(e).position !== 'fixed' && !e.closest('.hidden,[hidden],svg,#xterm,.xterm')) off.push((e.className && e.className.baseVal === undefined ? '.' + String(e.className).split(' ')[0] : e.tagName) + ':' + Math.round(b.right)); }); return { sw: document.documentElement.scrollWidth, W, off: [...new Set(off)].slice(0, 5) }; });
      const flag = o.sw > o.W; if (flag) bad++;
      console.log((flag ? 'OVER ' : 'ok   ') + lang + ' ' + w + ' ' + r + ' sw=' + o.sw + (flag ? ' ' + o.off.join(',') : ''));
      await ctx.close();
    }
  } finally { await fx.stop(); await browser.close(); }
  console.log('溢出:', bad);
})();
