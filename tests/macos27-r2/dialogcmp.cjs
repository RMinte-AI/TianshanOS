'use strict';
// 弹窗（sheet）对照：稿里的一个 .sheet ↔ 产品里打开后的弹窗元素；文本记号 + 绘制盒 + 像素，坐标都相对各自弹窗左上角。
//   node dialogcmp.cjs --board Sheets2 --nth 4 --route /security --open "showGenerateKeyModal()" --sel "#keygen-modal .sheet" [--tag 名]
//   选稿里的弹窗：--nth <序号，文档顺序，从 0 起> 或 --title <标题里的文字>
//   选产品里的弹窗：--sel <选择器，取最后一个可见的>；--open <打开它的 JS>；--pre <先执行的 JS>；--after <打开后再执行的 JS>
//   其它：--lang zh-CN --state populated|provisioned --role root|admin --profile <夹具补充 JSON> --wait 500 --tol 1 --board-pre <稿里先执行的 JS>
//   命令行只连本机 127.0.0.1 替身，不碰真实设备。
const fs = require('node:fs');
const path = require('node:path');
const L = require('./lib.cjs');
const { extractLayout } = require('./extract.js');

const A = L.parseArgs();
const board = A.board;
if (!board) { console.error('缺 --board'); process.exit(2); }
const lang = A.lang || 'zh-CN';
const tag = A.tag || `${board}-${A.nth ?? A.title ?? 'x'}`;
const outDir = L.mkdirp(path.join(L.OUT, 'dialogs', tag));
const TOL = Number(A.tol || 1);
const core = require('./cmpcore.cjs')({ TOL, radius: Number(A.radius || 60), ignoreRe: [/^\d{1,2}:\d{2}(:\d{2})?$/, /^\d{4}[-/]\d{1,2}[-/]\d{1,2}$/] });
const { fmt, compareLayouts } = core;

async function main() {
  const browser = await L.launch();
  // ---------- 稿 ----------
  const bctx = await browser.newContext({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });
  const bpage = await bctx.newPage();
  await bpage.goto('file://' + path.join(L.BOARDS, board + '.html'));
  if (A.quantify !== '0') await bpage.addStyleTag({ content: fs.readFileSync(L.QUANTIFY_CSS, 'utf8') });
  await bpage.addStyleTag({ content: L.FREEZE_CSS });
  await bpage.evaluate(() => document.fonts.ready);
  if (A['board-pre']) await bpage.evaluate(new Function(String(A['board-pre'])));
  if (A.unclamp) await bpage.evaluate(nth => { document.querySelectorAll('.sheet')[nth].style.maxWidth = 'none'; }, Number(A.nth));
  const found = await bpage.evaluate(({ nth, title, boardSel }) => {
    const sheets = [...document.querySelectorAll('.sheet')];
    let el = null;
    if (boardSel) el = document.querySelector(boardSel);
    else if (title) el = sheets.find(s => (s.querySelector('.st') || s).textContent.includes(title));
    else el = sheets[Number(nth || 0)];
    if (!el) return null;
    el.setAttribute('data-r2-root', '1');
    const r = el.getBoundingClientRect();
    return { x: r.left + scrollX, y: r.top + scrollY, w: r.width, h: r.height, total: sheets.length };
  }, { nth: A.nth, title: A.title, boardSel: A['board-sel'] || '' });
  if (!found) { console.error('稿里找不到这个弹窗'); process.exit(2); }
  if (A.debug) console.log('DEBUG board rect', JSON.stringify(found));
  const LB = await bpage.evaluate(extractLayout, { root: '[data-r2-root]' });
  await bpage.addStyleTag({ content: '.wall{background:#cddbe7!important;background-image:none!important}.dim{background:rgba(20,30,45,.14)!important}' });
  // 稿里的弹窗落在 y=728.5 这类小数坐标上（前面的弹窗高度带 .5），元素截图的取整与抗锯齿会让整张图相对产品错开 1px，像素差被夸大；
  // 截图前把目标弹窗单独固定到 (40,40)（其它内容隐藏、背景铺平并加与产品相同的 14% 遮罩），与产品侧的做法一致（产品侧是顶对齐 + margin-top:40px）。
  await bpage.addStyleTag({ content: 'html,body{background:#cddbe7!important;background-image:none!important}body::before{content:"";position:fixed;inset:0;background:rgba(20,30,45,.14);visibility:visible!important}body *{visibility:hidden!important}[data-r2-root],[data-r2-root] *{visibility:visible!important}[data-r2-root]{position:fixed!important;top:40px!important;left:40px!important;margin:0!important}' });
  if (A.debug) console.log('DEBUG board rect at screenshot', JSON.stringify(await bpage.evaluate(() => { const r = document.querySelector('[data-r2-root]').getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })));
  await bpage.locator('[data-r2-root]').screenshot({ path: path.join(outDir, 'board.png') });
  const rel = (LX, o) => { LX.tokens.forEach(t => { t.x -= o.x; t.y -= o.y; t.cx -= o.x; t.cy -= o.y; }); LX.boxes.forEach(b => { b.x -= o.x; b.y -= o.y; }); };
  rel(LB, found);
  await bctx.close();

  // ---------- 产品 ----------
  const root = A.root ? path.resolve(A.root) : path.join(L.REPO, 'components/ts_webui/web');
  const fx = await L.startFixture(root, { extra: A.profile ? path.resolve(A.profile) : path.join(L.REPO, 'tests/macos27-r2/profile-board.json') });
  let LP, foundP, errs = [];
  try {
    const { ctx, page } = await L.openPage(browser, fx.origin, A.route || '/', { width: 1440, height: Number(A.height || 1000), lang, state: A.state || 'populated', role: A.role || 'root', wait: Number(A.wait0 || 2600) });
    page.on('pageerror', e => errs.push(String(e)));
    if (A.pre) { await page.evaluate(new Function(String(A.pre))); await page.waitForTimeout(400); }
    if (A.open) { await page.evaluate(async code => { await (0, eval)(code); }, String(A.open)); await page.waitForTimeout(Number(A.wait || 500)); }
    if (A.after) { await page.evaluate(new Function(String(A.after))); await page.waitForTimeout(300); }
    const findRoot = sel => {
      const els = [...document.querySelectorAll(sel)].filter(e => { const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; });
      const el = els[els.length - 1];
      if (!el) return null;
      document.querySelectorAll('[data-r2-root]').forEach(x => x.removeAttribute('data-r2-root'));
      el.setAttribute('data-r2-root', '1');
      const r = el.getBoundingClientRect();
      return { x: r.left + scrollX, y: r.top + scrollY, w: r.width, h: r.height, count: els.length };
    };
    foundP = await page.evaluate(findRoot, A.sel || '.modal .sheet');
    // 半像素对齐（测试工具的假象）：弹窗在视口里居中，高度/宽度奇偶不同就会落在 .5px 上，截图里所有边缘的抗锯齿都会与稿（整数坐标）不同，
    // 像素差会被夸大到几个百分点。把视口宽/高各调 1px，让弹窗落在整数坐标上再截图。
    for (let k = 0; k < 2 && foundP && (foundP.y % 1 !== 0 || foundP.x % 1 !== 0); k++) {
      const vs = page.viewportSize();
      await page.setViewportSize({ width: vs.width + (foundP.x % 1 ? 1 : 0), height: vs.height + (foundP.y % 1 ? 1 : 0) });
      await page.waitForTimeout(250);
      foundP = await page.evaluate(findRoot, A.sel || '.modal .sheet');
    }
    if (!foundP) { console.error('产品里找不到弹窗元素：' + (A.sel || '.modal .sheet') + (errs.length ? '\n页面错误：' + errs.join('\n') : '')); await ctx.close(); process.exit(3); }
    if (A.debug) console.log('DEBUG page rect', JSON.stringify(foundP), JSON.stringify(page.viewportSize()));
    LP = await page.evaluate(extractLayout, { root: '[data-r2-root]' });
    rel(LP, foundP);
    // 截图前才把页面内容隐藏，只留弹窗：背后是纯背景渐变（和稿的 wall 同源）；抽取布局要在隐藏之前做（抽取会跳过祖先不可见的节点）
    await page.evaluate(() => { const a = document.activeElement; if (a && a !== document.body && a.blur) a.blur(); });   // 产品会自动聚焦第一个输入框，稿里没有焦点环
    await page.addStyleTag({ content: 'html,body{background:#cddbe7!important;background-image:none!important}body *{visibility:hidden!important}.modal,.modal *{visibility:visible!important}.modal{background:rgba(20,30,45,.14)!important;align-items:flex-start!important}.modal>.sheet{margin-top:40px!important}' });   // 顶对齐：居中会产生半像素偏移，抗锯齿会让像素对照失真
    if (A.debug) console.log('DEBUG page rect at screenshot', JSON.stringify(await page.evaluate(() => { const r = document.querySelector('[data-r2-root]').getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })));
    await page.locator('[data-r2-root]').screenshot({ path: path.join(outDir, 'page.png') });
    await ctx.close();
  } finally { await fx.stop(); }
  await browser.close();

  // ---------- 对照 ----------
  // 稿里用 button.field.sel 画下拉框，产品用原生 <select>，其中的文字无法从 DOM 抽取：稿侧这类文字不参与对照（下拉框本身的盒子仍参与）
  LB.tokens = LB.tokens.filter(t => !/field\.sel/.test(t.el));
  const { TA, TB, tm, bm, all, tokExact, boxExact, shift, kinds } = compareLayouts(LB, LP);
  const pix = JSON.parse(L.py(path.join(__dirname, 'pixdiff.py'), [path.join(outDir, 'board.png'), path.join(outDir, 'page.png'), outDir]));
  const summary = {
    tag, board, nth: A.nth, title: A.title, route: A.route, lang,
    size: { board: [Math.round(found.w), Math.round(found.h)], page: [Math.round(foundP.w), Math.round(foundP.h)] },
    tokens: { board: TA.length, page: TB.length, matched: tm.matches.length, exact: tokExact, missing: tm.missing.length, extra: tm.extra.length },
    boxes: { board: LB.boxes.length, page: LP.boxes.length, matched: bm.matches.length, exact: boxExact, missing: bm.missing.length, extra: bm.extra.length },
    shift, pixel: pix, kinds, pageErrors: errs,
  };
  fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify({ summary, deviations: all }, null, 1));
  { const pick = b => ({ x: b.x, y: b.y, w: b.w, h: b.h, glyph: b.glyph, label: b.label, fill: b.fill }); const isI = b => b.tag === 'svg' && b.glyph;
    fs.writeFileSync(path.join(outDir, 'icons.json'), JSON.stringify({ shift, board: LB.boxes.filter(isI).map(pick), page: LP.boxes.filter(isI).map(pick) })); }
  const md = [];
  md.push(`# 弹窗对照：${board}#${A.nth ?? A.title} ↔ ${A.sel || '.modal .sheet'}（${lang}）`);
  md.push('');
  md.push(`- 尺寸：稿 ${summary.size.board.join('×')}，页面 ${summary.size.page.join('×')}；整体偏移（中位）dx=${fmt(shift.dx)} dy=${fmt(shift.dy)}`);
  md.push(`- 文本记号：稿 ${TA.length}，页面 ${TB.length}，匹配 ${tm.matches.length}（完全一致 ${tokExact}），缺失 ${tm.missing.length}，多出 ${tm.extra.length}`);
  md.push(`- 绘制盒：稿 ${LB.boxes.length}，页面 ${LP.boxes.length}，匹配 ${bm.matches.length}（完全一致 ${boxExact}），缺失 ${bm.missing.length}，多出 ${bm.extra.length}`);
  md.push(`- 像素：不一致 ${pix.mismatch_pct}%，MAE ${pix.mae}`);
  md.push(`- 偏差类别：${Object.entries(kinds).map(([k, v]) => `${k}×${v}`).join('，') || '无'}`);
  if (errs.length) md.push(`- 页面错误：${errs.join(' | ')}`);
  md.push('');
  md.push('| 类别 | 位置 | 稿 | 实际 |');
  md.push('|---|---|---|---|');
  for (const d of all.slice(0, Number(A.top || 40))) md.push(`| ${d.kind} | ${d.where.replace(/\|/g, '/')} | ${String(d.want).replace(/\|/g, '/')} | ${String(d.got).replace(/\|/g, '/')} |`);
  fs.writeFileSync(path.join(outDir, 'report.md'), md.join('\n') + '\n');
  console.log(md.slice(0, 8).join('\n'));
  const top = all.slice(0, Number(A.print || 12));
  for (const d of top) console.log(`  ${d.kind} | ${d.where.slice(0, 44)} | ${String(d.want).slice(0, 44)} | ${String(d.got).slice(0, 44)}`);
  console.log(`输出：${outDir}`);
}
main().catch(e => { console.error('ERR', e.stack || e.message); process.exit(1); });
