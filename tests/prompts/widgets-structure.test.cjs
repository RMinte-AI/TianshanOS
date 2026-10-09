const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {harness} = require('./harness.cjs');
const css = fs.readFileSync(path.join(__dirname, '../../components/ts_webui/web/css/style.css'), 'utf8');

for (const type of ['ring', 'gauge']) {
    test(`${type}: value uses an HTML overlay outside the SVG and retains update IDs`, async () => {
        const h = harness();
        await h.ready();
        h.load();
        const html = h.run(`renderWidgetHtml({id:'sample',type:'${type}',label:'GPU',unit:' MB'})`);
        assert(!html.includes('<text'));
        assert(html.includes('</svg><span class="dw-ring-value" id="dw-sample-value">'));
        assert(html.includes(`id="dw-sample-${type}"`));
        const value = h.el('dw-sample-value');
        const arc = h.el(`dw-sample-${type}`);
        h.run(`updateWidgetValue({id:'sample',type:'${type}',unit:' MB',decimals:1},123456789012.3)`);
        assert.equal(value.textContent, '123456789012.3 MB');
        assert.equal(arc.getAttribute('stroke-dasharray'), type === 'ring' ? '163.4 163.4' : '88.0 87.96');
        h.run(`updateWidgetValue({id:'sample',type:'${type}'},null)`);
        assert.equal(value.textContent, '-');
        assert.equal(arc.getAttribute('stroke-dasharray'), type === 'ring' ? '0.0 163.4' : '0.0 87.96');
    });
}

test('all widget types retain rendering, layout and edit hooks in the new UI', async () => {
    const h = harness();
    await h.ready();
    h.load();
    for (const type of ['ring', 'gauge', 'temp', 'number', 'bar', 'text', 'status', 'icon', 'dual', 'percent', 'log']) {
        const html = h.run(`renderWidgetHtml({id:'sample',type:'${type}',label:'GPU',layout:'large',unit:' MB'})`);
        assert(html.includes('w-tile dw-card dw-layout-large'));
        assert(html.includes(`data-type="${type}"`));
        assert(html.includes("showWidgetManager('sample')"));
        assert(html.includes(type === 'log' ? 'id="dw-sample-log"' : 'id="dw-sample-value"'));
    }
    h.el('dw-sample-value');
    h.el('dw-sample-sub');
    h.run("updateWidgetValue({id:'sample',type:'dual',decimals:1,subValue:65536},32768.8)");
    assert.equal(h.el('dw-sample-value').textContent, '32768.8');
    assert.equal(h.el('dw-sample-sub').textContent, '65536.0');
    const long = 'LongUnbrokenComponentValue'.repeat(12);
    h.ctx.longWidgetValue = long;
    h.run("updateWidgetValue({id:'sample',type:'text'},longWidgetValue)");
    assert.equal(h.el('dw-sample-value').textContent, long);
});

test('stylesheet retains migrated bounds, overlay, wrapping and responsive span contracts', () => {
    for (const rule of [
        '.dw-card{container-type:inline-size;min-width:0;overflow-wrap:anywhere}',
        '.dw-ring>svg,.dw-ring-value{grid-area:1/1}',
        '.dw-card .bigrow{flex-wrap:wrap;max-width:100%}',
        '.dw-card[data-type=dual] .bigrow{flex-direction:column;align-items:center}',
        '.dw-grid>.dw-layout-large{grid-column:span 2}',
        '.dw-grid>.dw-layout-medium,.dw-grid>.dw-layout-large,.dw-grid>.dw-card[data-type=log][data-layout=auto]{grid-column:span 1}',
        '.dw-log-line{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}',
        '.dw-log-tools>.btn,.dw-log-tools>.state{flex-shrink:0;max-width:100%;white-space:normal;overflow-wrap:anywhere}',
        'max(270px,calc((100% - 24px)/3))'
    ]) assert(css.includes(rule), rule);
    assert(css.includes('.dw-grid{grid-template-columns:minmax(0,1fr)}'));
    assert(css.includes('.dw-ring-value{z-index:1;width:100%;'));
});
