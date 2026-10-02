// Capture every vertical portion of each editor without changing production CSS.
const fs = require('node:fs');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {chromium} = require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const origin = 'http://127.0.0.1:18809';
const root = 'output/macos27-20260929-native-rework';
const web = root + '/build-v17/web_optimized';
const out = root + '/widget-sequence-v17';
assert(!fs.existsSync(out));
fs.mkdirSync(out);
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const identities = () => Object.fromEntries(['index.html', 'css/style.css', 'js/app.js'].map(p => [p, hash(fs.readFileSync(web + '/' + p))]));
const report = {before: identities(), rows: [], errors: [], writes: [], completed: false};
(async () => {
    let browser;
    try {
        for (const p of ['css/style.css', 'js/app.js']) {
            assert(Buffer.from(await (await fetch(origin + '/' + p)).arrayBuffer()).equals(fs.readFileSync(web + '/' + p)));
        }
        browser = await chromium.launch({channel: 'chrome', headless: true});
        report.browser = browser.version();
        for (const [width, height, lang] of [[320, 600, 'en-US'], [844, 390, 'zh-CN']]) {
            const context = await browser.newContext({viewport: {width, height}});
            const page = await context.newPage();
            page.on('pageerror', e => report.errors.push(String(e)));
            await page.route('**/api/**', route => {
                const req = route.request(), u = new URL(req.url());
                assert.equal(u.origin, origin);
                if (req.method() !== 'GET' && !['/api/v1/auth/status', '/api/v1/device/ping'].includes(u.pathname)) {
                    report.writes.push({url: req.url(), method: req.method()});
                    return route.abort();
                }
                return route.continue();
            });
            await page.goto(origin + '/?state=populated&lang=' + lang + '#/');
            await page.locator('.dw-card').first().waitFor();
            await page.locator('[onclick="showWidgetManager()"]').click();
            const body = page.locator('.dw-manager-body');
            for (const name of ['add', 'ring', 'gauge', 'temp', 'number', 'bar', 'text', 'status', 'icon', 'dual', 'percent', 'log']) {
                const handler = name === 'add' ? 'showAddWidgetPanel()' : "showWidgetEditPanel('fixture-" + name + "')";
                await page.locator('[onclick=' + JSON.stringify(handler) + ']').filter({visible: true}).first().click();
                const limits = await body.evaluate(el => {
                    const pane = document.getElementById('dw-manager-main');
                    const start = Math.max(0, pane.getBoundingClientRect().top - el.getBoundingClientRect().top + el.scrollTop);
                    return {start: Math.min(start, el.scrollHeight - el.clientHeight), end: el.scrollHeight - el.clientHeight, step: el.clientHeight - 32};
                });
                assert(limits.step > 0);
                const stops = [limits.start];
                while (stops.at(-1) < limits.end) stops.push(Math.min(limits.end, stops.at(-1) + limits.step));
                const row = {name, lang, width, height, limits, frames: []};
                report.rows.push(row);
                for (const [index, target] of stops.entries()) {
                    await body.evaluate((el, y) => { el.scrollTop = y; }, target);
                    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
                    const geometry = await body.evaluate(el => {
                        const pane = document.getElementById('dw-manager-main');
                        const b = el.getBoundingClientRect();
                        const controls = [...pane.querySelectorAll('input:not([type="hidden"]),select,textarea,button')].filter(e => e.getClientRects().length).map(e => {
                            const r = e.getBoundingClientRect();
                            return {id: e.id, text: (e.textContent || '').trim().slice(0, 60), rect: r.toJSON(), visible: r.top >= b.top - 1 && r.bottom <= b.bottom + 1};
                        });
                        return {innerWidth, innerHeight, devicePixelRatio, visualScale: visualViewport.scale, scrollTop: el.scrollTop, scrollHeight: el.scrollHeight, clientHeight: el.clientHeight, body: b.toJSON(), pane: pane.getBoundingClientRect().toJSON(), pageOverflow: document.documentElement.scrollWidth > innerWidth, controls};
                    });
                    const path = `${out}/${name}-${lang}-${width}x${height}-${index}.png`;
                    await page.screenshot({path});
                    row.frames.push({path, sha256: hash(fs.readFileSync(path)), geometry});
                    assert(!geometry.pageOverflow);
                    assert(geometry.controls.every(c => c.rect.left >= geometry.body.left - 1 && c.rect.right <= geometry.body.right + 1));
                }
                // Continuous strips overlap by 32px; every normal-height control must
                // appear whole in at least one strip, rather than trusting a reused box.
                const controls = row.frames[0].geometry.controls;
                for (let i = 0; i < controls.length; i++) {
                    assert(row.frames.some(f => f.geometry.controls[i].visible), `${name}: control not wholly captured ${i}`);
                }
                const last = row.frames.at(-1).geometry;
                assert(Math.abs(last.scrollTop - (last.scrollHeight - last.clientHeight)) <= 1);
            }
            await page.locator('.dw-manager-modal .modal-close').click();
            await page.locator('.dw-manager-modal').waitFor({state: 'detached'});
            await context.close();
        }
        report.after = identities();
        assert.deepEqual(report.before, report.after);
        assert.equal(report.errors.length, 0);
        assert.equal(report.writes.length, 0);
        report.completed = true;
    } catch (e) {
        report.errors.push(String(e));
        throw e;
    } finally {
        fs.writeFileSync(out + '/raw.json', JSON.stringify(report, null, 2));
        if (browser) await browser.close();
    }
})().catch(e => { console.error(e); process.exitCode = 1; });
