'use strict';
// 压缩器保真检查：用 Chrome 的 CSSOM 解析源码版与压缩版，逐规则比较。
// 选择器（selectorText）精确比较——`.a :hover` 被压成 `.a:hover`、`:not (` 变成非法而整条规则被丢弃，都会暴露；
// 声明块去掉空白后比较——带 var() 的声明 Chrome 会原样保留空白，不算差异。
//   node csscheck.cjs <源 style.css> <压缩后 style.css>
const fs = require('node:fs');
const L = require('./lib.cjs');

(async () => {
  const [srcPath, builtPath] = process.argv.slice(2);
  const src = fs.readFileSync(srcPath, 'utf8');
  const built = fs.readFileSync(builtPath, 'utf8');
  const browser = await L.launch();
  const page = await browser.newPage();
  const res = await page.evaluate(({ src, built }) => {
    const strip = t => t.replace(/\s+/g, '');
    const ser = r => {
      switch (r.constructor.name) {
        case 'CSSStyleRule': return `S|${r.selectorText}|${strip(r.style.cssText)}`;
        case 'CSSMediaRule': return `M|${r.conditionText}|{${[...r.cssRules].map(ser).join(';')}}`;
        case 'CSSSupportsRule': return `U|${r.conditionText}|{${[...r.cssRules].map(ser).join(';')}}`;
        case 'CSSKeyframesRule': return `K|${r.name}|{${[...r.cssRules].map(k => k.keyText + strip(k.style.cssText)).join(';')}}`;
        case 'CSSFontFaceRule': return `F|${strip(r.style.cssText)}`;
        default: return strip(r.cssText);
      }
    };
    const parse = text => { const s = new CSSStyleSheet(); s.replaceSync(text); return [...s.cssRules].map(ser); };
    return { a: parse(src), b: parse(built) };
  }, { src, built });
  await browser.close();
  const count = new Map();
  for (const t of res.a) count.set(t, (count.get(t) || 0) + 1);
  for (const t of res.b) count.set(t, (count.get(t) || 0) - 1);
  const onlySrc = [], onlyBuilt = [];
  for (const [t, n] of count) { if (n > 0) for (let i = 0; i < n; i++) onlySrc.push(t); else if (n < 0) for (let i = 0; i < -n; i++) onlyBuilt.push(t); }
  const out = { rulesSource: res.a.length, rulesBuilt: res.b.length, onlySource: onlySrc.length, onlyBuilt: onlyBuilt.length };
  console.log(JSON.stringify(out));
  for (const t of onlySrc.slice(0, 8)) console.log('  仅源码有：', t.slice(0, 160));
  for (const t of onlyBuilt.slice(0, 8)) console.log('  仅产物有：', t.slice(0, 160));
  process.exitCode = onlySrc.length || onlyBuilt.length ? 1 : 0;
})().catch(e => { console.error('ERR', e.message); process.exit(2); });
