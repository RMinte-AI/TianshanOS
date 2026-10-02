'use strict';
// 对照核心（compare.cjs 与 dialogcmp.cjs 共用）：文本记号/绘制盒匹配 + 偏差归类。
module.exports = function makeCore({ TOL = 1, radius = 60, ignoreRe = [/^\d{1,2}:\d{2}(:\d{2})?$/, /^\d{4}[-/]\d{1,2}[-/]\d{1,2}$/] } = {}) {
const colorDist = (p, q) => Math.abs(p[0] - q[0]) + Math.abs(p[1] - q[1]) + Math.abs(p[2] - q[2]) + 255 * Math.abs(p[3] - q[3]);
  const fmt = n => (Math.round(n * 10) / 10).toString();
  const rgba = c => `rgba(${c[0]},${c[1]},${c[2]},${fmt(c[3])})`;

  function iou(a, b) {
    const x1 = Math.max(a.x, b.x), y1 = Math.max(a.y, b.y), x2 = Math.min(a.x + a.w, b.x + b.w), y2 = Math.min(a.y + a.h, b.y + b.h);
    const i = Math.max(0, x2 - x1) * Math.max(0, y2 - y1);
    const u = a.w * a.h + b.w * b.h - i;
    return u ? i / u : 0;
  }

  function matchTokens(TA, TB) {
    const usedA = new Set(), usedB = new Set(), matches = [];
    for (const rad of [6, radius]) {
      const byS = new Map();
      TB.forEach((t, i) => { if (!usedB.has(i)) { if (!byS.has(t.s)) byS.set(t.s, []); byS.get(t.s).push(i); } });
      const pairs = [];
      TA.forEach((a, ai) => {
        if (usedA.has(ai)) return;
        for (const bi of byS.get(a.s) || []) {
          const d = Math.hypot(a.cx - TB[bi].cx, a.cy - TB[bi].cy);
          if (d <= rad) pairs.push([d, ai, bi]);
        }
      });
      pairs.sort((p, q) => p[0] - q[0]);
      for (const [d, ai, bi] of pairs) {
        if (usedA.has(ai) || usedB.has(bi)) continue;
        usedA.add(ai); usedB.add(bi); matches.push({ a: TA[ai], b: TB[bi] });
      }
    }
    return { matches, missing: TA.filter((_, i) => !usedA.has(i)), extra: TB.filter((_, i) => !usedB.has(i)) };
  }

  function matchBoxes(BA, BB) {
    const pairs = [];
    for (let i = 0; i < BA.length; i++) for (let j = 0; j < BB.length; j++) {
      const v = iou(BA[i], BB[j]);
      if (v > 0.35) pairs.push([v, i, j]);
    }
    pairs.sort((p, q) => q[0] - p[0]);
    const ua = new Set(), ub = new Set(), matches = [];
    for (const [v, i, j] of pairs) {
      if (ua.has(i) || ub.has(j)) continue;
      ua.add(i); ub.add(j); matches.push({ a: BA[i], b: BB[j], iou: v });
    }
    return { matches, missing: BA.filter((_, i) => !ua.has(i)), extra: BB.filter((_, j) => !ub.has(j)) };
  }

  function labelFor(box, tokens) {
    let best = null;
    for (const t of tokens) {
      if (t.cx >= box.x && t.cx <= box.x + box.w && t.cy >= box.y && t.cy <= box.y + box.h) {
        if (!best || t.cy < best.cy - 3 || (Math.abs(t.cy - best.cy) <= 3 && t.cx < best.cx)) best = t;
      }
    }
    return best ? best.s : '';
  }

  const clampR = (r, b) => r.map(v => Math.min(v, Math.min(b.w, b.h) / 2));

  function boxDeviations(m, tokensA) {
    const { a, b } = m; const dev = [];
    const lab = labelFor(a, tokensA);
    const where = `${a.sig}${lab ? ` 〔${lab}〕` : ''}`;
    const dx = b.x - a.x, dy = b.y - a.y, dw = b.w - a.w, dh = b.h - a.h;
    if (Math.abs(dx) > TOL || Math.abs(dy) > TOL || Math.abs(dw) > TOL || Math.abs(dh) > TOL)
      dev.push({ kind: 'box-rect', where, want: `${fmt(a.x)},${fmt(a.y)} ${fmt(a.w)}×${fmt(a.h)}`, got: `${fmt(b.x)},${fmt(b.y)} ${fmt(b.w)}×${fmt(b.h)}`, score: Math.hypot(dx, dy) + Math.abs(dw) + Math.abs(dh) });
    const ra = clampR(a.r, a), rb = clampR(b.r, b);
    const rd = Math.max(...ra.map((v, i) => Math.abs(v - rb[i])));
    if (rd > 1) dev.push({ kind: 'box-radius', where, want: ra.map(fmt).join('/'), got: rb.map(fmt).join('/'), score: rd * 2 });
    const cd = colorDist(a.bg, b.bg);
    if (cd > 8) dev.push({ kind: 'box-bg', where, want: rgba(a.bg), got: rgba(b.bg), score: cd / 8 });
    if (a.bgi !== b.bgi && (a.bgi || b.bgi)) dev.push({ kind: 'box-bgimage', where, want: a.bgi || '—', got: b.bgi || '—', score: 8 });
    const bwa = a.bw[0], bwb = b.bw[0];
    if (Math.abs(bwa - bwb) > 0.5 || (bwa > 0 && colorDist(a.bc[0], b.bc[0]) > 8))
      dev.push({ kind: 'box-border', where, want: `${fmt(bwa)}px ${rgba(a.bc[0])}`, got: `${fmt(bwb)}px ${rgba(b.bc[0])}`, score: 6 });
    const shA = a.sh.replace(/\s+/g, ''), shB = b.sh.replace(/\s+/g, '');
    if (shA !== shB) dev.push({ kind: 'box-shadow', where, want: a.sh || '无', got: b.sh || '无', score: 5 });
    if (a.bf !== b.bf) dev.push({ kind: 'box-backdrop', where, want: a.bf || '无', got: b.bf || '无', score: 5 });
    if ((a.tag === 'svg' || b.tag === 'svg') && a.fill && b.fill && a.fill !== b.fill) dev.push({ kind: 'icon-color', where, want: a.fill, got: b.fill, score: 3 });
    return dev;
  }

  function tokenDeviations(m) {
    const { a, b } = m; const dev = [];
    const where = `“${a.s}” ${a.el}`;
    const dx = b.x - a.x, dy = b.y - a.y, dw = b.w - a.w;
    if (Math.abs(dx) > TOL || Math.abs(dy) > TOL) dev.push({ kind: 'text-pos', where, want: `${fmt(a.x)},${fmt(a.y)}`, got: `${fmt(b.x)},${fmt(b.y)}`, score: Math.hypot(dx, dy) });
    if (Math.abs(dw) > 1.5 && a.s.length > 1) dev.push({ kind: 'text-width', where, want: fmt(a.w), got: fmt(b.w), score: Math.abs(dw) });
    if (Math.abs(a.fs - b.fs) > 0.3) dev.push({ kind: 'text-size', where, want: `${fmt(a.fs)}px`, got: `${fmt(b.fs)}px`, score: 6 + Math.abs(a.fs - b.fs) });
    if (a.fw !== b.fw) dev.push({ kind: 'text-weight', where, want: a.fw, got: b.fw, score: 5 });
    if (colorDist(a.color, b.color) > 10) dev.push({ kind: 'text-color', where, want: rgba(a.color), got: rgba(b.color), score: 4 });
    if (Math.abs(a.ls - b.ls) > 0.15) dev.push({ kind: 'text-spacing', where, want: `${fmt(a.ls)}px`, got: `${fmt(b.ls)}px`, score: 3 });
    if (a.ff !== b.ff) dev.push({ kind: 'text-font', where, want: a.ff, got: b.ff, score: 5 });
    return dev;
  }

  // 一次完成：过滤 → 匹配 → 偏差列表（按得分降序）
  function compareLayouts(LB, LP, { band = null, bandP = null } = {}) {
    bandP = bandP || band;
    const inBand = (y, h, bd) => !bd || (y + h / 2 >= bd[0] && y + h / 2 <= bd[1]);
    if (band) {
      LB.tokens = LB.tokens.filter(t => inBand(t.y, t.h, band)); LB.boxes = LB.boxes.filter(b => inBand(b.y, b.h, band));
      LP.tokens = LP.tokens.filter(t => inBand(t.y, t.h, bandP)); LP.boxes = LP.boxes.filter(b => inBand(b.y, b.h, bandP));
      const shift = band[0] - bandP[0];
      if (shift) { LP.tokens.forEach(t => { t.y += shift; t.cy += shift; }); LP.boxes.forEach(b => { b.y += shift; }); }
    }
    const keepTok = t => !ignoreRe.some(re => re.test(t.s));
    const TA = LB.tokens.filter(keepTok), TB = LP.tokens.filter(keepTok);
    const tm = matchTokens(TA, TB);
    const bm = matchBoxes(LB.boxes, LP.boxes);
    const devs = [];
    for (const m of tm.matches) devs.push(...tokenDeviations(m));
    for (const m of bm.matches) devs.push(...boxDeviations(m, TA));
    const tokExact = tm.matches.filter(m => tokenDeviations(m).length === 0).length;
    const boxExact = bm.matches.filter(m => boxDeviations(m, TA).length === 0).length;
    const med = arr => { if (!arr.length) return 0; const s = [...arr].sort((p, q) => p - q); return s[Math.floor(s.length / 2)]; };
    const shift = { dx: med(tm.matches.map(m => m.b.x - m.a.x)), dy: med(tm.matches.map(m => m.b.y - m.a.y)) };
    const missBoxes = bm.missing.map(b => ({ kind: 'box-missing', where: `${b.sig}${labelFor(b, TA) ? ` 〔${labelFor(b, TA)}〕` : ''}`, want: `${fmt(b.x)},${fmt(b.y)} ${fmt(b.w)}×${fmt(b.h)} bg ${rgba(b.bg)} r${b.r.map(fmt).join('/')}`, got: '页面无对应元素', score: 10 + Math.sqrt(b.w * b.h) / 10 }));
    const extraBoxes = bm.extra.filter(b => b.w * b.h > 400).map(b => ({ kind: 'box-extra', where: `${b.sig}`, want: '稿中无对应元素', got: `${fmt(b.x)},${fmt(b.y)} ${fmt(b.w)}×${fmt(b.h)} bg ${rgba(b.bg)}`, score: 4 + Math.sqrt(b.w * b.h) / 20 }));
    const missTok = tm.missing.map(t => ({ kind: 'text-missing', where: `“${t.s}” ${t.el}`, want: `${fmt(t.x)},${fmt(t.y)}`, got: '页面无该文本', score: 7 }));
    const extraTok = tm.extra.map(t => ({ kind: 'text-extra', where: `“${t.s}” ${t.el}`, want: '稿中无该文本', got: `${fmt(t.x)},${fmt(t.y)}`, score: 2 }));
    const all = [...devs, ...missBoxes, ...missTok, ...extraBoxes, ...extraTok].sort((p, q) => q.score - p.score);
    const kinds = {};
    for (const d of all) kinds[d.kind] = (kinds[d.kind] || 0) + 1;
    return { TA, TB, tm, bm, all, tokExact, boxExact, shift, kinds };
  }
  return { colorDist, fmt, rgba, iou, matchTokens, matchBoxes, labelFor, boxDeviations, tokenDeviations, compareLayouts };
};
