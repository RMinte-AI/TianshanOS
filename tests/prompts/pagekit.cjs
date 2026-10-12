// A real browser page wired to a fake device, shared by the failure-message suites.
//   const kit = require('./pagekit.cjs');  kit.install();   // registers before/after hooks that start and stop the browser
//   await kit.withPage('zh-CN', call => response, async ({page, calls}) => { ... });
// `call` is {endpoint, method, params}. The response is a raw string (sent verbatim), an object (sent as JSON), undefined
// (an empty success) or kit.DROP (the connection is cut: the device never answers).
const {before, after} = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http'), fs = require('node:fs'), path = require('node:path');
const {chromium} = require('playwright');
const {root} = require('./harness.cjs');

const DROP = Symbol('drop');
let browser;
function install() {
    before(async () => { browser = await chromium.launch({channel: 'chrome', headless: true}); });
    after(async () => { await browser?.close(); });
}

async function open(language, respond = () => undefined, {viewport} = {}) {
    const calls = [], errors = [];
    const server = http.createServer((req, res) => {
        const url = new URL(req.url, 'http://local');
        const send = body => { res.statusCode = 200; res.setHeader('Content-Type', 'application/json'); res.end(typeof body === 'string' ? body : JSON.stringify(body)); };
        if (url.pathname.startsWith('/api/v1/')) {
            let raw = '';
            req.setEncoding('utf8');
            req.on('data', part => raw += part);
            req.on('end', () => {
                const call = {endpoint: url.pathname.slice('/api/v1/'.length), method: req.method, params: req.method === 'GET' ? Object.fromEntries(url.searchParams) : (raw && /json/i.test(req.headers['content-type'] || '') ? JSON.parse(raw) : {})};
                calls.push(call);
                const out = respond(call);
                if (out === DROP) { req.socket.destroy(); return; }
                send(out ?? {code: 0, data: {}});
            });
            return;
        }
        const file = path.resolve(root, '.' + (url.pathname === '/' ? '/index.html' : url.pathname));
        if (!file.startsWith(root + '/')) { res.writeHead(403).end(); return; }
        fs.readFile(file, (error, data) => {
            if (error) { res.writeHead(404).end(); return; }
            res.setHeader('Content-Type', ({'.js': 'application/javascript', '.html': 'text/html', '.css': 'text/css', '.woff2': 'font/woff2', '.svg': 'image/svg+xml'})[path.extname(file)] || 'application/octet-stream');
            res.end(data);
        });
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const base = `http://127.0.0.1:${server.address().port}`;
    const context = await browser.newContext({locale: language, ...(viewport ? {viewport} : {})});
    await context.addInitScript(language => {
        localStorage.setItem('ts_language', language);
        class Socket { static OPEN = 1; static CONNECTING = 0; constructor() { this.readyState = 0; } send() {} close() { this.readyState = 3; } }
        window.WebSocket = Socket;
    }, language);
    const page = await context.newPage();
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(base);
    await page.waitForFunction(() => i18n.isReady());
    await page.evaluate(() => closeLoginModal());
    return {page, calls, errors, close: async () => { await context.close(); await new Promise(resolve => server.close(resolve)); }};
}
async function withPage(language, respond, run) {
    const d = await open(language, respond);
    try { await run(d); assert.deepEqual(d.errors, []); } finally { await d.close(); }
}

// failure toasts keep the sentence in .toast-msg; plain toasts keep it directly in #toast
const toastText = page => page.evaluate(() => { const toast = document.getElementById('toast'); return toast?.querySelector('.toast-msg')?.textContent ?? toast?.textContent ?? ''; });
const toastIs = (page, type) => page.waitForFunction(type => document.getElementById('toast')?.classList.contains('toast-' + type) && document.getElementById('toast').classList.contains('show'), type);
const expectToast = async (page, type, text) => {
    await toastIs(page, type);
    await page.waitForFunction(text => { const toast = document.getElementById('toast'); return (toast?.querySelector('.toast-msg')?.textContent ?? toast?.textContent) === text; }, text);
    assert.equal(await toastText(page), text);
};
// the page's own confirmation sheet: answer it the way a user would
const confirmSheet = async (page, yes = true) => {
    const button = page.locator(`.confirm-sheet button[data-r="${yes ? 1 : 0}"]`);
    await button.waitFor();
    await button.click();
};

module.exports = {install, open, withPage, toastText, toastIs, expectToast, confirmSheet, DROP};
