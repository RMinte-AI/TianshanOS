// Exercise the shared rich toast in the rendered page, including the action-result API path.
const {test} = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
if (process.env.TS_TOAST_WEB) require('./harness.cjs').root = path.resolve(process.env.TS_TOAST_WEB);
const kit = require('./pagekit.cjs');
const {fixture} = require('./configuration-fixture.cjs');
kit.install();

async function bounded(page, details = true) {
    const boxes = await page.locator('#toast').evaluate((toast, details) => {
        const box = e => {
            const r = e.getBoundingClientRect();
            return {top: r.top, bottom: r.bottom, left: r.left, right: r.right, height: r.height};
        };
        return {
            width: innerWidth, height: innerHeight, toast: box(toast),
            children: [...toast.querySelectorAll(details ? '.toast-msg,button,.toast-detail' : '.toast-msg,button')].map(box)
        };
    }, details);
    assert(boxes.toast.top >= 15, JSON.stringify(boxes));
    assert(boxes.toast.bottom <= boxes.height && boxes.toast.left >= 0 && boxes.toast.right <= boxes.width, JSON.stringify(boxes));
    for (const child of boxes.children) {
        assert(child.height > 0 && child.top >= boxes.toast.top && child.bottom <= boxes.toast.bottom + 1, JSON.stringify(boxes));
        assert(child.left >= boxes.toast.left && child.right <= boxes.toast.right + 1, JSON.stringify(boxes));
    }
}

async function readToEnd(page, selector) {
    const area = page.locator(selector);
    assert(await area.evaluate(e => e.scrollHeight > e.clientHeight), 'content must actually need scrolling');
    await area.focus();
    await page.keyboard.press('End');
    await page.waitForFunction(selector => {
        const e = document.querySelector(selector);
        return e.scrollTop + e.clientHeight >= e.scrollHeight - 1;
    }, selector, {timeout: 5000});
}

for (const language of ['zh-CN', 'en-US']) {
    for (const viewport of [{width: 1024, height: 768}, {width: 390, height: 768}, {width: 667, height: 375}]) {
        test(`${language}: action's 200-byte multiline output remains readable at ${viewport.width}x${viewport.height}`, async () => {
            const device = fixture(), output = '123\n'.repeat(49) + 'END\n';
            assert.equal(Buffer.byteLength(output), 200); // Fits the production 256-byte result buffer.
            const p = await kit.open(language, call => call.endpoint === 'automation/actions/execute'
                ? {code: 0, data: {output}} : device.respond(call), {viewport});
            try {
                const {page} = p;
                await page.evaluate(async () => { await loadAutomationPage(); await testAction('act'); });
                const toast = page.locator('#toast'), detail = toast.locator('.toast-detail');
                assert.equal(await detail.isHidden(), true);
                await bounded(page, false);
                await toast.locator('.toast-more').click();
                await bounded(page);
                assert((await detail.textContent()).endsWith(output), 'complete raw output is preserved');
                assert.equal(await page.evaluate(() => toastDeadline === Infinity), true);
                await readToEnd(page, '#toast .toast-detail');
                await bounded(page);
                // Collapse and close remain keyboard-operable after reading the end.
                await toast.locator('.toast-more').focus();
                await page.keyboard.press('Enter');
                assert.equal(await detail.isHidden(), true);
                await page.keyboard.press('Tab');
                assert.equal(await toast.locator('.toast-close').evaluate(e => e === document.activeElement), true);
                await page.keyboard.press('Enter');
                assert.equal(await toast.evaluate(e => e.classList.contains('show')), false);
                assert.deepEqual(p.errors, []);
            } finally { await p.close(); }
        });
    }

    test(`${language}: long summary and unbroken details survive resizing with controls outside scrolling content`, async () => {
        const p = await kit.open(language, undefined, {viewport: {width: 1024, height: 768}});
        try {
            const {page} = p, message = 'Summary <literal>\n'.repeat(80), details = 'AGX_LPMU_<script>'.repeat(200);
            await page.evaluate(({message, details}) => showFailureToast(message, details), {message, details});
            await page.locator('#toast .toast-more').click();
            await page.setViewportSize({width: 320, height: 320});
            await bounded(page);
            assert.equal(await page.locator('#toast .toast-msg').textContent(), message);
            assert.equal(await page.locator('#toast .toast-detail').textContent(), details);
            assert.equal(await page.locator('#toast script').count(), 0);
            await readToEnd(page, '#toast .toast-msg');
            await readToEnd(page, '#toast .toast-detail');
            await bounded(page);
            await page.setViewportSize({width: 390, height: 768});
            await bounded(page);
            await page.locator('#toast .toast-more').click();
            await bounded(page, false);
            await page.locator('#toast .toast-close').click();
            // A long message without details shares the same bound and remains dismissible.
            await page.evaluate(message => showFailureToast(message), message);
            await bounded(page, false);
            await readToEnd(page, '#toast .toast-msg');
            assert.equal(await page.locator('#toast .toast-more').count(), 0);
            await page.locator('#toast .toast-close').click();
            assert.equal(await page.locator('#toast').evaluate(e => e.classList.contains('show')), false);
            assert.deepEqual(p.errors, []);
        } finally { await p.close(); }
    });
}
