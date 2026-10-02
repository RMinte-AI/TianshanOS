// 在页面里运行的抽取函数（board 与 app 共用），返回可序列化的布局快照。
// 用法：page.evaluate(extractLayout, {root: '.dc' | null})
function extractLayout(opts) {
  const rootSel = opts && opts.root;
  const rootEl = rootSel ? document.querySelector(rootSel) : document.body;
  const sx = window.scrollX, sy = window.scrollY;
  const px = v => parseFloat(v) || 0;
  const parseColor = c => {
    if (!c) return [0, 0, 0, 0];
    let m = c.match(/rgba?\(([^)]+)\)/);
    if (m) { const p = m[1].split(/[ ,\/]+/).filter(Boolean).map(Number); return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1]; }
    m = c.match(/color\(srgb ([^)]+)\)/);
    if (m) { const p = m[1].split(/[ \/]+/).filter(Boolean).map(Number); return [Math.round(p[0] * 255), Math.round(p[1] * 255), Math.round(p[2] * 255), p.length > 3 ? p[3] : 1]; }
    return [0, 0, 0, 0];
  };
  const skipClosest = 'script,style,noscript,template,#fixture-metrics,#fixture-observation,[data-r2-ignore]';
  const visible = (el, cs) => {
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0 && cs.visibility !== 'hidden' && cs.display !== 'none' && parseFloat(cs.opacity) > 0.01;
  };
  // 祖先里有 opacity:0 / display:none 的一律排除
  const chainVisible = el => {
    for (let e = el; e && e !== document.documentElement; e = e.parentElement) {
      const cs = getComputedStyle(e);
      if (cs.display === 'none' || cs.visibility === 'hidden' || parseFloat(cs.opacity) < 0.01) return false;
    }
    return true;
  };
  const sig = el => {
    let s = el.tagName.toLowerCase();
    if (el.id) s += '#' + el.id;
    const c = typeof el.className === 'string' ? el.className.trim().split(/\s+/).slice(0, 3).join('.') : '';
    if (c) s += '.' + c;
    return s;
  };
  const csCache = new Map();
  const styleOf = el => {
    let v = csCache.get(el);
    if (!v) {
      const cs = getComputedStyle(el);
      v = { fs: px(cs.fontSize), fw: cs.fontWeight, color: parseColor(cs.color), ls: cs.letterSpacing === 'normal' ? 0 : px(cs.letterSpacing), ff: cs.fontFamily.split(',')[0].replace(/["']/g, '').trim(), lh: cs.lineHeight, ta: cs.textAlign, fvn: cs.fontVariantNumeric, tt: cs.textTransform };
      csCache.set(el, v);
    }
    return v;
  };

  // ---- 文本记号：按 CJK 单字 / 拉丁词 / 数字串 / 标点 切分，每个记号取自己的矩形 ----
  const TOK = /[⺀-鿿＀-￯]|[A-Za-z]+|\d+(?:[.,:]\d+)*|[^\sA-Za-z\d⺀-鿿＀-￯]/g;
  const tokens = [];
  const walker = document.createTreeWalker(rootEl, NodeFilter.SHOW_TEXT);
  let n;
  while ((n = walker.nextNode())) {
    const data = n.data;
    if (!data.trim()) continue;
    const el = n.parentElement;
    if (!el || el.closest(skipClosest)) continue;
    if (el.tagName === 'I' && /(^|\s)ri-/.test(el.className)) continue;   // 图标字体的私有区字符，不算文本
    if (!chainVisible(el)) continue;
    const st = styleOf(el);
    let m;
    TOK.lastIndex = 0;
    const range = document.createRange();
    while ((m = TOK.exec(data))) {
      range.setStart(n, m.index);
      range.setEnd(n, m.index + m[0].length);
      const rs = range.getClientRects();
      if (!rs.length) continue;
      const r = rs[0];
      if (r.width === 0 || r.height === 0) continue;
      tokens.push({
        s: m[0], x: r.left + sx, y: r.top + sy, w: r.width, h: r.height,
        cx: r.left + sx + r.width / 2, cy: r.top + sy + r.height / 2,
        fs: st.fs, fw: st.fw, color: st.color, ls: st.ls, ff: st.ff, ta: st.ta, el: sig(el),
      });
    }
  }

  // ---- 绘制盒：有底色/底图/边框/阴影的元素，以及最外层 svg/img/canvas ----
  const boxes = [];
  const all = rootEl.querySelectorAll('*');
  for (const el of all) {
    if (el.closest(skipClosest)) continue;
    const tag = el.tagName.toLowerCase();
    if (el.closest('svg') && tag !== 'svg') continue;           // svg 内部图元不单独记
    if (el.closest('svg') !== null && tag === 'svg' && el.parentElement && el.parentElement.closest('svg')) continue; // 嵌套 svg
    let cs = getComputedStyle(el);
    if (cs.display === 'none' || cs.display === 'contents') continue;
    const r = el.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) continue;
    // 底衬伪元素（z-index:-1、绝对定位、铺满整个边框盒，如 .m-surface::before）：材质画在它上面，取它的样式当作元素自己的绘制盒
    { const pb = getComputedStyle(el, '::before'); if (pb.content !== 'none' && pb.position === 'absolute' && pb.zIndex === '-1' && pb.display !== 'none') cs = pb; }
    if (cs.visibility === 'hidden' || parseFloat(cs.opacity) < 0.01) continue;
    const bg = parseColor(cs.backgroundColor);
    const bgi = cs.backgroundImage !== 'none';
    const bw = [px(cs.borderTopWidth), px(cs.borderRightWidth), px(cs.borderBottomWidth), px(cs.borderLeftWidth)];
    const bcs = [cs.borderTopColor, cs.borderRightColor, cs.borderBottomColor, cs.borderLeftColor].map(parseColor);
    const hasBorder = bw.some((w, i) => w > 0 && bcs[i][3] > 0.02 && ['solid', 'dashed', 'dotted', 'double'].includes([cs.borderTopStyle, cs.borderRightStyle, cs.borderBottomStyle, cs.borderLeftStyle][i]));
    const sh = cs.boxShadow !== 'none' ? cs.boxShadow : '';
    const isIconFont = tag === 'i' && /(^|\s)ri-/.test(el.className);
    const isMedia = tag === 'svg' || tag === 'img' || tag === 'canvas' || isIconFont;
    if (!(bg[3] > 0.02 || bgi || hasBorder || sh || isMedia)) continue;
    if (!chainVisible(el)) continue;
    const rad = [px(cs.borderTopLeftRadius), px(cs.borderTopRightRadius), px(cs.borderBottomRightRadius), px(cs.borderBottomLeftRadius)];
    boxes.push({
      tag: isIconFont ? 'svg' : tag, sig: sig(el), x: r.left + sx, y: r.top + sy, w: r.width, h: r.height,
      r: rad, bg, bgi: bgi ? cs.backgroundImage.slice(0, 60) : '', bw, bc: bcs, sh,
      bf: (cs.backdropFilter && cs.backdropFilter !== 'none') ? cs.backdropFilter : '',
      op: parseFloat(cs.opacity),
      fill: isMedia ? cs.color : '',
      icon: isIconFont ? String(el.className).split(/\s+/).find(c => c.startsWith('ri-')) || '' : '',
      // 图标身份：产品 = <use href>；稿 = 内联图形；label = 紧邻的文字/标题（用来把稿里的图形对应到产品里的图标名）
      glyph: tag === 'svg' ? ((el.querySelector('use') && el.querySelector('use').getAttribute('href')) || ('inner:' + el.innerHTML.replace(/\s+/g, ' ').trim().slice(0, 700))) : '',
      label: tag === 'svg' && el.parentElement ? (el.parentElement.getAttribute('title') || el.parentElement.getAttribute('aria-label') || (el.parentElement.textContent || '').trim().slice(0, 24)) : '',
    });
  }
  // ---- 控件：按钮/输入/下拉/文本域（不含复选、单选、滑块、隐藏、文件） ----
  const controls = [];
  for (const el of rootEl.querySelectorAll('button,input,select,textarea,[role=button],a.btn,.btn,.field')) {
    if (el.closest(skipClosest)) continue;
    const tag = el.tagName.toLowerCase();
    const type = (el.getAttribute('type') || '').toLowerCase();
    if (tag === 'input' && ['checkbox', 'radio', 'range', 'hidden', 'file', 'color'].includes(type)) continue;
    const cs = getComputedStyle(el);
    if (!visible(el, cs) || !chainVisible(el)) continue;
    const r = el.getBoundingClientRect();
    controls.push({ tag, type, sig: sig(el), h: r.height, w: r.width, x: r.left + sx, y: r.top + sy, fs: px(cs.fontSize), fw: cs.fontWeight, ff: cs.fontFamily.split(',')[0].replace(/["']/g, '').trim(), cur: cs.cursor });
  }
  const de = document.documentElement;
  return { size: { w: de.scrollWidth, h: de.scrollHeight }, tokens, boxes, controls };
}
if (typeof module !== 'undefined') module.exports = { extractLayout };
