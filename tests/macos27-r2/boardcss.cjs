'use strict';
// 组件层验证：把每张稿的 HTML 里的 spec.css 换成产品 style.css（另补画布脚手架类），与原稿逐像素对比。
// 通过 = 产品样式表对稿里全部组件类的渲染与设计源一致。用法：
//   node boardcss.cjs [--board Main] [--css <style.css 路径，默认源码>] [--tag 名称]
const fs = require('node:fs');
const path = require('node:path');
const L = require('./lib.cjs');
const A = L.parseArgs();
const cssPath = A.css ? path.resolve(A.css) : path.join(L.REPO, 'components/ts_webui/web/css/style.css');
const tag = A.tag || 'boardcss';
const outDir = L.mkdirp(path.join(L.OUT, 'boardcss', tag));
// 有意偏离稿的元素：①页脚品牌字按用户要求用 Bold（稿为 Regular）；②复选框选中态加白色对勾（稿只画了蓝色方块，没有对勾，不符合 macOS 习惯）；
// ③图标描边：稿 1.35 → 产品 1.0（图标换成 Phosphor Regular 之后，.i 的描边只剩保留的自绘图标在用，Regular 在 16px 下约 1.0px）。
// ③不遮图标，而是把稿那一侧的描边改成同样的值，图形、位置、尺寸仍逐像素比对；产品里图标的实际结构与粗细另由 iconweight.cjs 断言。
const INTENDED_MASK = '.foot,.chk.on{visibility:hidden!important}.i{stroke-width:1!important}';
const fontB64 = fs.readFileSync(path.join(L.REPO, 'components/ts_webui/web/fonts/quantify-bold.woff2')).toString('base64');
let css = fs.readFileSync(cssPath, 'utf8').replace('/fonts/quantify-bold.woff2', 'data:font/woff2;base64,' + fontB64);
// 画布脚手架（仅稿里用，不属于产品）
// 稿里 .m-surface 常带内联 overflow:hidden；产品把毛玻璃挪到 ::before 后，外阴影会被它裁掉。两边都放开裁剪再比，才是组件层的公平比较。
const NOCLIP = '.m-surface{overflow:visible!important}';
const scaffold = `.dc{font-family:var(--font);color:var(--ink);font-size:14px;line-height:20px;-webkit-font-smoothing:antialiased}
.wall{position:relative;overflow:hidden;background:#cddbe7;background-image:var(--wall)}
.sheet{position:absolute}.dim{position:absolute;inset:0;background:rgba(20,30,45,.20)}
.dis{display:flex;align-items:center;gap:8px;font-size:15px;font-weight:600;padding:12px 16px}
.edge-top::before{content:"";position:sticky;top:0;display:block;height:16px;margin-bottom:-16px;background:linear-gradient(rgba(240,244,249,.9),rgba(240,244,249,0));pointer-events:none;z-index:1}`;

(async () => {
  const boards = A.board ? [A.board] : fs.readdirSync(L.BOARDS).filter(f => f.endsWith('.html')).map(f => f.replace('.html', '')).sort();
  const browser = await L.launch();
  const rows = [];
  for (const b of boards) {
    const raw = fs.readFileSync(path.join(L.BOARDS, b + '.html'), 'utf8');
    const modified = raw.replace('<link rel="stylesheet" href="spec.css">', `<style>${css}\n${scaffold}\n${NOCLIP}</style>`)
      .replace(/<a class="on"/g, '<a class="active"');   // 产品里导航选中沿用路由的 active 类（仅改名，样式等价）
    const shots = {};
    for (const [name, html, useOrigCss] of [['orig', raw, true], ['prod', modified, false]]) {
      const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
      const page = await ctx.newPage();
      if (useOrigCss) { await page.goto('file://' + path.join(L.BOARDS, b + '.html')); await page.addStyleTag({ content: fs.readFileSync(L.QUANTIFY_CSS, 'utf8') + NOCLIP }); }
      else await page.setContent(html, { waitUntil: 'load' });
      await page.addStyleTag({ content: L.FREEZE_CSS });
      await page.addStyleTag({ content: INTENDED_MASK });   // 有意偏离稿的三处（见上）：前两处两边都遮掉，图标描边两边统一成新值，其余部分照常逐像素比对
      await page.evaluate(() => document.fonts.ready);
      const r = await page.evaluate(() => { const e = document.querySelector('.dc'); const q = e.getBoundingClientRect(); return { x: q.left, y: q.top, w: q.width, h: q.height }; });
      await page.setViewportSize({ width: Math.ceil(r.x + r.w), height: Math.ceil(r.y + r.h) });
      await page.waitForTimeout(250);
      const f = path.join(outDir, `${b}-${name}.png`);
      await page.screenshot({ path: f, clip: { x: r.x, y: r.y, width: r.w, height: r.h } });
      shots[name] = f;
      await ctx.close();
    }
    const d = JSON.parse(L.py(path.join(__dirname, 'pixdiff.py'), [shots.orig, shots.prod, outDir]));
    fs.renameSync(path.join(outDir, 'diff.png'), path.join(outDir, `${b}-diff.png`));
    fs.rmSync(path.join(outDir, 'side.png'), { force: true });
    rows.push({ board: b, mismatch_pct: d.mismatch_pct, mae: d.mae, size: d.board_size });
    console.log(`${b.padEnd(13)} 不一致 ${String(d.mismatch_pct).padStart(7)}%  MAE ${String(d.mae).padStart(6)}  ${d.board_size.join('×')}`);
  }
  await browser.close();
  fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(rows, null, 1));
  const bad = rows.filter(r => r.mismatch_pct > 0.05);
  console.log(bad.length ? `⚠ ${bad.length} 张稿与原稿不一致：${bad.map(r => r.board).join(', ')}` : '✅ 全部稿与原稿像素一致（阈值 0.05%）');
  process.exitCode = bad.length ? 1 : 0;
})().catch(e => { console.error('ERR', e.stack || e.message); process.exit(2); });
