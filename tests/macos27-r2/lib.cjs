'use strict';
// macOS 27 WebUI 第二轮：验证公共库。只连本机替身（127.0.0.1），不碰真实设备。
const path = require('node:path');
const fs = require('node:fs');
const net = require('node:net');
const { spawn, execFileSync } = require('node:child_process');

const REPO = path.resolve(__dirname, '..', '..');
const PW = '/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright';
const CPY = '/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3'; // Pillow + numpy
const BOARDS = path.join(REPO, 'docs/macos27/design-system/boards');
const QUANTIFY_CSS = path.join(REPO, 'docs/macos27/design-system/demo/assets/quantify.css');
const OUT = path.join(REPO, 'output/macos27-r2');

function parseArgs(argv = process.argv.slice(2)) {
  const o = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const k = a.slice(2);
      const v = argv[i + 1] !== undefined && !argv[i + 1].startsWith('--') ? argv[++i] : true;
      o[k] = v;
    } else o._.push(a);
  }
  return o;
}

function freePort() {
  return new Promise((res, rej) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
    s.on('error', rej);
  });
}

// 启动上一轮遗留的本机替身（只读使用，不修改）。root 可以是源码目录或构建产物目录。
async function startFixture(root, { log = '', extra = '' } = {}) {
  const port = await freePort();
  const args = [path.join(REPO, 'tests/macos27/fixture-server.cjs'), path.resolve(root), String(port), log];
  if (extra) args.push(extra);
  const p = spawn('node', args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let err = '';
  p.stderr.on('data', d => { err += d; });
  await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('fixture start timeout: ' + err)), 10000);
    p.stdout.on('data', d => { if (String(d).includes('Fixture only')) { clearTimeout(t); res(); } });
    p.on('exit', c => { clearTimeout(t); rej(new Error('fixture exited ' + c + ' ' + err)); });
  });
  return {
    port,
    origin: `http://127.0.0.1:${port}`,
    stop: () => new Promise(r => { p.removeAllListeners('exit'); p.on('exit', r); p.kill(); setTimeout(r, 1500); }),
  };
}

async function launch() {
  const { chromium } = require(PW);
  return chromium.launch({ channel: 'chrome', headless: true });
}

function pageUrl(origin, route = '/', { state = 'populated', role = 'root', lang = 'zh-CN', extra = '' } = {}) {
  return `${origin}/?state=${state}&role=${role}&lang=${lang}${extra}#${route}`;
}

// 关动画/过渡、固定光标，避免截图抖动
const FREEZE_CSS = '*,*::before,*::after{animation:none!important;transition:none!important;caret-color:transparent!important}';

async function openPage(browser, origin, route, { width = 1440, height = 1000, lang = 'zh-CN', state = 'populated', role = 'root', extra = '', wait = 2600, dpr = 1 } = {}) {
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: dpr });
  const page = await ctx.newPage();
  await page.goto(pageUrl(origin, route, { state, role, lang, extra }));
  await page.waitForTimeout(wait);
  await page.addStyleTag({ content: FREEZE_CSS });
  return { ctx, page };
}

function py(script, args = []) {
  return execFileSync(CPY, [script, ...args], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
}

function mkdirp(p) { fs.mkdirSync(p, { recursive: true }); return p; }

module.exports = { REPO, PW, CPY, BOARDS, QUANTIFY_CSS, OUT, parseArgs, freePort, startFixture, launch, pageUrl, openPage, FREEZE_CSS, py, mkdirp };
