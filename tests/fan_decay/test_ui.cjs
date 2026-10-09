const assert = require('node:assert/strict');
const {harness} = require('../prompts/harness.cjs');

(async () => {
    for (const language of ['zh-CN', 'en-US']) {
        const h = harness(language);
        await h.ready();
        h.load();
        h.el('fans-grid');
        h.el('fan-global-temp');
        h.el('fan-global-duty');
        for (const mode of ['auto', 'curve', 'manual', 'off']) {
            for (const [duty, target] of [[87, 85], [37, 70], [0, 0]]) {
                h.ctx.fixture = {fans: [{id: 0, mode, duty, target_duty: target, fault: duty !== target,
                    enabled: true, predicted_temperature: 38.3, slope_c_per_min: -14,
                    guard_temperature: 48.8, auto_state: 'active'}], temperature: 48.8, temp_valid: true};
                h.run('updateFanInfo(fixture)');
                const html = h.el('fans-grid').innerHTML;
                assert(html.includes(`fan-speed-num">${duty}</span>`), mode);
                assert(html.includes(`fan-slider-value">${duty}%</span>`), mode);
                assert.equal(h.el('fan-global-duty').textContent, `${duty}%`);
                assert(!html.includes('fanPage.'), language);
            }
        }
        h.ctx.fixture = {fans: [{id: 0, mode: 'auto', target_duty: 87}]};
        h.run('updateFanInfo(fixture)');
        assert(h.el('fans-grid').innerHTML.includes('fan-speed-num">--</span>'));
        assert.equal(h.el('fan-global-duty').textContent, '--%');
        console.log(`PASS ${language}: production renderer shows applied PWM, not requested PWM; missing applied value stays unknown`);
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
