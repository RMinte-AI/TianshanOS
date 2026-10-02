'use strict';
// 快速探针：在本机替身上打开一个路由，在页面里执行一段表达式并打印结果（只连 127.0.0.1）。
//   node tests/macos27-r2/probe.cjs --route /security --js "expr" [--width 1440] [--height 1000] [--lang zh-CN] [--state populated|provisioned] [--role root|admin] [--pre "js"] [--wait 2600] [--shot out.png]
const L = require('./lib.cjs');
const path = require('node:path');
(async () => {
  const A = L.parseArgs();
  const root = path.join(L.REPO, 'components/ts_webui/web');
  const browser = await L.launch();
  const fx = await L.startFixture(root, { extra: A.profile ? path.resolve(A.profile) : path.join(L.REPO, 'tests/macos27-r2/profile-board.json') });
  try {
    const { ctx, page } = await L.openPage(browser, fx.origin, A.route || '/', { width: Number(A.width || 1440), height: Number(A.height || 1000), lang: A.lang || 'zh-CN', state: A.state || 'populated', role: A.role || 'root', wait: Number(A.wait || 2600) });
    const errs = [];
    page.on('pageerror', e => errs.push(String(e)));
    if (A.pre) { await page.evaluate(new Function(String(A.pre))); await page.waitForTimeout(Number(A.prewait || 500)); }
    if (A.js) { const r = await page.evaluate(new Function('return (' + A.js + ')')); console.log(typeof r === 'string' ? r : JSON.stringify(r, null, 1)); }
    if (A.shot) await page.screenshot({ path: path.resolve(A.shot), fullPage: true });
    if (errs.length) console.log('PAGEERRORS', errs);
    await ctx.close();
  } finally { await fx.stop(); await browser.close(); }
})().catch(e => { console.error(e); process.exit(1); });
