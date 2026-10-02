// Run the existing production-UI checks against the revised skin without
// overwriting the rejected revision's evidence. The fixture has no device proxy.
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const built = process.argv.includes('--built');
const out = 'output/macos27-20260929-card-revision' + (built ? '/built' : '');
const names = process.argv.slice(2).filter(x => x !== '--built');
if (!names.length) throw new Error('Specify existing test procedure names');
(async () => {
    fs.mkdirSync(path.join(out, 'after'), { recursive: true });
    const browser = await chromium.launch({ channel: 'chrome', headless: true });
    try {
        for (const name of names) {
            if (!/^[a-z0-9-]+$/.test(name)) throw new Error('Invalid procedure name');
            const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
            try {
                await page.goto('http://127.0.0.1:' + (built ? 18789 : 18783) + '/?state=populated#/');
                const source = fs.readFileSync(path.join(__dirname, name + '.js'), 'utf8')
                    .replaceAll('output/macos27-20260928', out)
                    .replaceAll('18783', built ? '18789' : '18783');
                const result = await eval('(' + source + ')')(page);
                fs.writeFileSync(path.join(out, name + '.json'), JSON.stringify(result, null, 2));
                console.log('DONE', name, new Date().toISOString());
            } finally { await page.close(); }
        }
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
