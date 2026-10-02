'use strict';
// 逐个跑 dialogs.json 里的弹窗对照（每个一行摘要）。  node dialogs-all.cjs [名称片段…] [--prefix 标签前缀]
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
const L = require('./lib.cjs');
const A = L.parseArgs();
const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, 'dialogs.json'), 'utf8'));
const want = A._;
const prefix = A.prefix || 'dlg';
const shard = typeof A.shard === 'string' ? A.shard.split('/').map(Number) : null;   // --shard i/n：只跑第 i 组（按顺序轮流分配），便于多进程并行
let idx = 0;
for (const [name, c] of Object.entries(manifest)) {
  if (want.length && !want.some(w => name.includes(w))) continue;
  if (shard && (idx++ % shard[1]) !== shard[0]) continue;
  const args = [path.join(__dirname, 'dialogcmp.cjs'), '--tag', `${prefix}-${name}`];
  for (const [k, v] of Object.entries(c)) args.push('--' + k, String(v));
  const r = spawnSync('node', args, { encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  const pick = re => (out.match(re) || [])[0] || '';
  const size = pick(/尺寸：[^\n]*/).replace('尺寸：', '');
  const tok = pick(/文本记号：[^\n]*/).replace('文本记号：', 'T ');
  const box = pick(/绘制盒：[^\n]*/).replace('绘制盒：', 'B ');
  const pix = pick(/像素：[^\n]*/).replace('像素：不一致 ', 'px ');
  const kinds = pick(/偏差类别：[^\n]*/).replace('偏差类别：', '');
  console.log(`## ${name}  ${r.status ? '✗ ' + out.split('\n').filter(Boolean).slice(-2).join(' ') : ''}\n   ${size} | ${tok} | ${box} | ${pix}\n   ${kinds}`);
}
