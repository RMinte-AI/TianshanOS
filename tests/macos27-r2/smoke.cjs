'use strict';
// 冒烟：逐路由加载，收集 console.error / pageerror / 4xx-5xx（501 未知端点单独统计），不做断言业务结果。
//   node smoke.cjs [--root web目录] [--routes /,/network,...] [--lang zh-CN] [--profile 夹具补充数据.json]
const path = require('node:path');
const L = require('./lib.cjs');
const A = L.parseArgs();
(async () => {
  const root = A.root ? path.resolve(A.root) : path.join(L.REPO, 'components/ts_webui/web');
  const routes = String(A.routes || '/,/network,/files,/terminal,/automation,/commands,/security,/ota').split(',');
  const langs = String(A.lang || 'zh-CN').split(',');
  const browser = await L.launch();
  const fx = await L.startFixture(root, { extra: A.profile ? path.resolve(A.profile) : '' });
  let bad = 0;
  try {
    for (const lang of langs) for (const r of routes) {
      const { ctx, page } = await L.openPage(browser, fx.origin, r, { lang, wait: 2600 });
      const errs = [];
      // openPage 已经等过 2.6s；错误在此之前发生，所以重新加载并监听
      await page.close();
      const p2 = await ctx.newPage();
      p2.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text().slice(0, 160)); });
      p2.on('pageerror', e => errs.push('pageerror: ' + String(e.message).slice(0, 200)));
      const unk = new Set();
      p2.on('response', rsp => { if (rsp.status() >= 400) { const u = new URL(rsp.url()); (rsp.status() === 501 ? unk : { add: x => errs.push(rsp.status() + ' ' + x) }).add(u.pathname); } });
      await p2.goto(L.pageUrl(fx.origin, r, { lang }));
      await p2.waitForTimeout(3200);
      const n = await p2.evaluate(() => document.querySelectorAll('#page-content *').length);
      console.log(`${lang} ${r.padEnd(12)} 节点 ${String(n).padStart(4)}  错误 ${errs.length}  未知端点(501) ${unk.size}${errs.length ? '\n   ' + errs.slice(0, 6).join('\n   ') : ''}`);
      bad += errs.length;
      await ctx.close();
    }
  } finally { await fx.stop(); await browser.close(); }
  process.exitCode = bad ? 1 : 0;
})().catch(e => { console.error('ERR', e.stack || e.message); process.exit(2); });
