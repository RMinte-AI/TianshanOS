'use strict';
// 函数「逻辑骨架」差分：HEAD 与当前的同一份 js 文件，逐个顶层函数比较。
// 骨架 = 去掉注释、模板字符串里的静态文字、含 HTML 标签的字符串字面量，再归一空白。
// 换句话说：只比较「函数里写了什么逻辑」，忽略「拼出来的标记长什么样」。用来回答「除了标记之外，有没有动到逻辑」。
//   node tests/macos27-r2/fndiff.cjs --head <HEAD 的 web 目录> [--file js/app.js] [--show 函数名[,函数名]] [--all] [--max 40]
//   加 --effects：改比「对外副作用签名」（api 调用及其接口名、fetch、定时器、存储、跳转、确认/提示/toast、await 是否在位等），只列签名有差异的函数。
// 只读；不修改任何文件。
const fs = require('node:fs');
const path = require('node:path');
const acorn = require(path.join(__dirname, '../prompts/node_modules/acorn'));
const L = require('./lib.cjs');

function walk(node, fn) {
  if (!node || typeof node.type !== 'string') return;
  fn(node);
  for (const k of Object.keys(node)) {
    const v = node[k];
    if (Array.isArray(v)) v.forEach(x => x && typeof x.type === 'string' && walk(x, fn));
    else if (v && typeof v.type === 'string') walk(v, fn);
  }
}

function parse(src) {
  const comments = [];
  const ast = acorn.parse(src, { ecmaVersion: 'latest', sourceType: 'script', allowHashBang: true, onComment: (b, t, s, e) => comments.push([s, e]) });
  const cuts = comments.map(([s, e]) => [s, e, '']);
  walk(ast, n => {
    if (n.type === 'TemplateElement') cuts.push([n.start, n.end, '']);
    else if (n.type === 'Literal' && typeof n.value === 'string' && /<\/?[a-zA-Z]/.test(n.value) && n.value.length > 8) cuts.push([n.start, n.end, '"H"']);
  });
  cuts.sort((a, b) => a[0] - b[0]);
  return { ast, cuts };
}

function skel(src, cuts, start, end) {
  let out = '', pos = start;
  for (const [s, e, r] of cuts) {
    if (e <= start || s >= end) continue;
    if (s < pos) continue; // 已被更大的范围覆盖（不应发生）
    out += src.slice(pos, s) + r; pos = e;
  }
  out += src.slice(pos, end);
  return out.replace(/\s+/g, ' ').trim();
}

// 顶层「命名单元」：函数声明；以及 window.x = … / 顶层 const|let|var 声明（按名字）
function units(file) {
  const src = fs.readFileSync(file, 'utf8');
  const { ast, cuts } = parse(src);
  const m = new Map();
  const put = (name, node) => { m.set(name, { sk: skel(src, cuts, node.start, node.end), raw: node.end - node.start, line: src.slice(0, node.start).split('\n').length }); };
  for (const n of ast.body) {
    if (n.type === 'FunctionDeclaration' && n.id) put(n.id.name, n);
    else if (n.type === 'ClassDeclaration' && n.id) { for (const mth of n.body.body) put(n.id.name + '.' + (mth.key && (mth.key.name || mth.key.value)), mth); }
    else if (n.type === 'VariableDeclaration') for (const d of n.declarations) if (d.id && d.id.name) put('var:' + d.id.name, d);
    else if (n.type === 'ExpressionStatement' && n.expression.type === 'AssignmentExpression' && n.expression.left.type === 'MemberExpression') put('assign:' + src.slice(n.expression.left.start, n.expression.left.end), n);
    else if (n.type === 'ExpressionStatement') put('stmt@' + skel(src, cuts, n.start, Math.min(n.end, n.start + 60)), n);
  }
  return m;
}

const EFFECT_RE = /(await\s+)?\b(api\.call\(\s*['"`][^'"`]+['"`]|api\.[A-Za-z_$][\w$.]*\(|fetch\(|XMLHttpRequest|new WebSocket|EventSource|setTimeout\(|setInterval\(|clearInterval\(|clearTimeout\(|localStorage\.\w+\(\s*['"`]?[\w.$-]*|sessionStorage\.\w+\(|location\.[\w]+|window\.open\(|navigate\(|history\.\w+\(|confirmAction\(|confirmSheet\(|alert\(|confirm\(|prompt\(|showToast\(|closeModal\(|fieldError\(|document\.cookie|\.submit\(|\.click\(\)|FormData|URL\.createObjectURL|navigator\.clipboard\.\w+)/g;
function effects(sk) { const out = []; let m; EFFECT_RE.lastIndex = 0; while ((m = EFFECT_RE.exec(sk))) out.push(((m[1] ? 'await ' : '') + m[2]).replace(/\s+/g, ' ')); return out.sort(); }
function msDiff(a, b) { const c = new Map(); for (const x of a) c.set(x, (c.get(x) || 0) + 1); for (const x of b) c.set(x, (c.get(x) || 0) - 1); const r = []; for (const [k, v] of c) if (v) r.push((v > 0 ? '- ' : '+ ') + k + (Math.abs(v) > 1 ? ' ×' + Math.abs(v) : '')); return r; }

function tokens(s) { return s.split(/(?<=[;{}])\s*/).filter(Boolean); }
function diff(a, b) {
  const n = a.length, m = b.length;
  if (n * m > 4e6) return [['~', `（太长，未做行级差分：${n} vs ${m} 段）`]];
  const dp = Array.from({ length: n + 1 }, () => new Uint16Array(m + 1));
  for (let i = n - 1; i >= 0; i--) for (let j = m - 1; j >= 0; j--) dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
  const out = []; let i = 0, j = 0;
  while (i < n && j < m) { if (a[i] === b[j]) { i++; j++; } else if (dp[i + 1][j] >= dp[i][j + 1]) out.push(['-', a[i++]]); else out.push(['+', b[j++]]); }
  while (i < n) out.push(['-', a[i++]]); while (j < m) out.push(['+', b[j++]]);
  return out;
}

(async () => {
  const A = L.parseArgs();
  if (!A.head) { console.error('缺 --head <HEAD 的 web 目录>'); process.exit(2); }
  const rel = typeof A.file === 'string' ? A.file : 'js/app.js';
  const H = units(path.join(path.resolve(A.head), rel)), C = units(path.join(L.REPO, 'components/ts_webui/web', rel));
  const show = typeof A.show === 'string' ? new Set(A.show.split(',')) : null;
  const same = [], changed = [], onlyH = [], onlyC = [];
  for (const [k, v] of H) { if (!C.has(k)) onlyH.push(k); else if (C.get(k).sk === v.sk) same.push(k); else changed.push(k); }
  for (const k of C.keys()) if (!H.has(k)) onlyC.push(k);
  console.log(`${rel}：HEAD 单元 ${H.size}，当前 ${C.size}；骨架相同 ${same.length}，骨架有差异 ${changed.length}，只在 HEAD ${onlyH.length}，只在当前 ${onlyC.length}`);
  if (!A.quiet) {
    console.log('\n只在 HEAD：' + (onlyH.filter(k => !k.startsWith('stmt@')).join(', ') || '无'));
    console.log('只在当前：' + (onlyC.filter(k => !k.startsWith('stmt@')).join(', ') || '无'));
  }
  if (A.effects) {
    let n = 0;
    for (const k of changed) { const d = msDiff(effects(H.get(k).sk), effects(C.get(k).sk)); if (d.length) { n++; console.log(`  ${k}（当前 ${C.get(k).line}）: ${d.join('；')}`); } }
    for (const k of onlyC) if (!k.startsWith('stmt@')) { const e = effects(C.get(k).sk); if (e.length) console.log(`  [新增] ${k}: ${[...new Set(e)].join('，')}`); }
    for (const k of onlyH) if (!k.startsWith('stmt@')) { const e = effects(H.get(k).sk); if (e.length) console.log(`  [删除] ${k}: ${[...new Set(e)].join('，')}`); }
    console.log(`\n对外副作用签名有差异的函数：${n} / ${changed.length}`);
    return;
  }
  const max = Number(A.max || 40);
  const targets = show ? changed.filter(k => show.has(k)) : (A.all ? changed : []);
  if (!targets.length) {
    console.log('\n骨架有差异的单元（名字 · HEAD 行 → 当前行 · 骨架长度 HEAD/当前）：');
    for (const k of changed) console.log(`  ${k}  ${H.get(k).line}→${C.get(k).line}  ${H.get(k).sk.length}/${C.get(k).sk.length}`);
  }
  for (const k of targets) {
    console.log(`\n=== ${k}（HEAD ${H.get(k).line} → 当前 ${C.get(k).line}）`);
    const d = diff(tokens(H.get(k).sk), tokens(C.get(k).sk)); let shown = 0;
    for (const [t, s] of d) { if (shown++ >= max) { console.log('  …（其余省略，用 --max 调大）'); break; } console.log(`  ${t} ${s.slice(0, 220)}`); }
  }
  if (show) for (const k of show) if (!changed.includes(k)) console.log(`\n（${k}：${same.includes(k) ? '骨架相同' : '不在两边的单元里'}）`);
})().catch(e => { console.error(e); process.exit(2); });
