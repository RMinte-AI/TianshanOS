'use strict';
// 目标稿（boards/*.html）与实现页面的逐项对照：文本锚点 + 绘制盒几何/样式 + 像素差。
// node compare.cjs --board Main --root <web目录> [--route /] [--lang zh-CN] [--out 目录] [--profile 夹具补充数据.json]
//                  [--board-sel .dc] [--radius 60] [--height 自动取稿画布高] [--tag 标签] [--top 40]
const fs = require('node:fs');
const path = require('node:path');
const L = require('./lib.cjs');
const { extractLayout } = require('./extract.js');

const A = L.parseArgs();
const board = A.board || 'Main';
const root = A.root ? path.resolve(A.root) : path.join(L.REPO, 'components/ts_webui/web');
const route = A.route || '/';
const lang = A.lang || 'zh-CN';
const tag = A.tag || `${board}-${lang}`;
const outDir = L.mkdirp(A.out ? path.resolve(A.out) : path.join(L.OUT, 'compare', tag));
const radius = Number(A.radius || 60);
const TOL = Number(A.tol || 1);   // 位置/尺寸容差（px）
const topN = Number(A.top || 40);
const boardSel = A['board-sel'] || '.dc';
const ignoreRe = [/^\d{1,2}:\d{2}(:\d{2})?$/, /^\d{4}[-/]\d{1,2}[-/]\d{1,2}$/];

const core = require('./cmpcore.cjs')({ TOL, radius, ignoreRe });
const { fmt, rgba, compareLayouts } = core;

async function main() {
  const browser = await L.launch();
  // ---------- 稿 ----------
  const bctx = await browser.newContext({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });
  const bpage = await bctx.newPage();
  await bpage.goto('file://' + path.join(L.BOARDS, board + '.html'));
  if (A.quantify !== '0') await bpage.addStyleTag({ content: fs.readFileSync(L.QUANTIFY_CSS, 'utf8') });
  await bpage.addStyleTag({ content: L.FREEZE_CSS });
  await bpage.evaluate(() => document.fonts.ready);
  const rectB = await bpage.evaluate(sel => { const e = document.querySelector(sel); const r = e.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }, boardSel);
  await bpage.setViewportSize({ width: Math.ceil(rectB.x + rectB.w), height: Math.ceil(rectB.y + rectB.h) });
  await bpage.waitForTimeout(300);
  await bpage.screenshot({ path: path.join(outDir, 'board.png'), clip: { x: rectB.x, y: rectB.y, width: rectB.w, height: rectB.h } });
  const LB = await bpage.evaluate(extractLayout, { root: boardSel });
  if (rectB.x || rectB.y) {   // 子画板：坐标改成相对画板左上角
    LB.tokens.forEach(t => { t.x -= rectB.x; t.y -= rectB.y; t.cx -= rectB.x; t.cy -= rectB.y; });
    LB.boxes.forEach(b => { b.x -= rectB.x; b.y -= rectB.y; });
  }
  await bctx.close();

  // ---------- 实现 ----------
  const fx = await L.startFixture(root, { extra: A.profile ? path.resolve(A.profile) : '' });
  let LP, shotSize;
  try {
    const H = Number(A.height || Math.ceil(rectB.h));
    const { ctx, page } = await L.openPage(browser, fx.origin, route, { width: Math.ceil(rectB.w), height: H, lang, wait: Number(A.wait || 2600) });
    if (A.pre) { await page.evaluate(new Function(String(A.pre))); await page.waitForTimeout(Number(A.prewait || 600)); }   // 截图前先在页面里执行的脚本（点开状态、选中项等）
    await page.evaluate(() => { const m = document.getElementById('fixture-metrics'); if (m) m.setAttribute('data-r2-ignore', ''); window.scrollTo(0, 0); });
    const docH = await page.evaluate(() => document.documentElement.scrollHeight);
    if (docH > H) { await page.setViewportSize({ width: Math.ceil(rectB.w), height: docH }); await page.waitForTimeout(400); }
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(300);
    await page.screenshot({ path: path.join(outDir, 'page.png'), fullPage: true });
    LP = await page.evaluate(extractLayout, { root: null });
    shotSize = LP.size;
    await ctx.close();
  } finally { await fx.stop(); }
  await browser.close();

  // ---------- 对照 ----------
  const band = A.band ? String(A.band).split(',').map(Number) : null;              // 稿侧
  const bandP = A['band-page'] ? String(A['band-page']).split(',').map(Number) : band; // 页面侧
  const { TA, TB, tm, bm, all, tokExact, boxExact, shift, kinds } = compareLayouts(LB, LP, { band, bandP });

  const pix = JSON.parse(L.py(path.join(__dirname, 'pixdiff.py'), [path.join(outDir, 'board.png'), path.join(outDir, 'page.png'), outDir]));

  const summary = {
    tag, board, route, lang, root,
    board_size: [Math.round(rectB.w), Math.round(rectB.h)], page_size: [shotSize.w, shotSize.h],
    tokens: { board: TA.length, page: TB.length, matched: tm.matches.length, exact: tokExact, missing: tm.missing.length, extra: tm.extra.length },
    boxes: { board: LB.boxes.length, page: LP.boxes.length, matched: bm.matches.length, exact: boxExact, missing: bm.missing.length, extra: bm.extra.length },
    shift, pixel: pix, kinds,
  };
  fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify({ summary, deviations: all }, null, 1));
  { const pick = b => ({ x: b.x, y: b.y, w: b.w, h: b.h, glyph: b.glyph, label: b.label, fill: b.fill }); const isI = b => b.tag === 'svg' && b.glyph;
    fs.writeFileSync(path.join(outDir, 'icons.json'), JSON.stringify({ shift, board: LB.boxes.filter(isI).map(pick), page: LP.boxes.filter(isI).map(pick) })); }

  const md = [];
  md.push(`# 对照：${board}.html ↔ ${route}（${lang}）`);
  md.push('');
  md.push(`- 画布 ${summary.board_size.join('×')}，页面 ${summary.page_size.join('×')}；整体偏移（中位）dx=${fmt(shift.dx)} dy=${fmt(shift.dy)}`);
  md.push(`- 文本记号：稿 ${TA.length}，页面 ${TB.length}，匹配 ${tm.matches.length}（完全一致 ${tokExact}），缺失 ${tm.missing.length}，多出 ${tm.extra.length}`);
  md.push(`- 绘制盒：稿 ${LB.boxes.length}，页面 ${LP.boxes.length}，匹配 ${bm.matches.length}（完全一致 ${boxExact}），缺失 ${bm.missing.length}，多出 ${bm.extra.length}`);
  md.push(`- 像素：不一致 ${pix.mismatch_pct}%，MAE ${pix.mae}`);
  md.push(`- 偏差类别：${Object.entries(kinds).map(([k, v]) => `${k}×${v}`).join('，') || '无'}`);
  md.push('');
  md.push(`## 偏差 Top ${Math.min(topN, all.length)}`);
  md.push('| 类别 | 位置 | 稿 | 实际 |');
  md.push('|---|---|---|---|');
  for (const d of all.slice(0, topN)) md.push(`| ${d.kind} | ${d.where.replace(/\|/g, '/')} | ${String(d.want).replace(/\|/g, '/')} | ${String(d.got).replace(/\|/g, '/')} |`);
  fs.writeFileSync(path.join(outDir, 'report.md'), md.join('\n') + '\n');
  console.log(md.slice(0, 9).join('\n'));
  console.log(`输出：${outDir}`);
}

main().catch(e => { console.error('ERR', e.stack || e.message); process.exit(1); });
