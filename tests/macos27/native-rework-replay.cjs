// Reuse reviewed local UI procedures without overwriting prior evidence.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const [phase, port, ...requested] = process.argv.slice(2);
const runArgs = requested.filter(name => name.startsWith('--run='));
if (runArgs.length > 1 || (runArgs.length && !/^--run=[a-z0-9-]+$/.test(runArgs[0]))) throw Error('Invalid evidence run suffix');
const runSuffix = runArgs.length ? '-' + runArgs[0].slice(6) : '';
const names = requested.filter(name => !name.startsWith('--run='));
if (!['before', 'after', 'built'].includes(phase) || !['18783', '18789', '18790', '18791', '18793', '18795', '18796', '18798'].includes(port) || !names.length) {
    throw new Error('Usage: native-rework-replay.cjs before|after|built 18783|18789|18790|18791|18793|18795|18796|18798 procedure...');
}
if (runSuffix && phase !== 'built') throw Error('Run suffix is only supported for built evidence');
const evidenceRoot = 'output/macos27-20260929-native-rework' + (phase === 'built' ? (port === '18798' ? '/built-v7' : port === '18796' ? '/built-v6' : port === '18795' ? '/built-v5' : port === '18793' ? '/built-v3' : port === '18791' ? '/built-v2' : '/built') + runSuffix : '');
const out = phase === 'built' ? evidenceRoot : evidenceRoot + '/' + phase;
const webRoot = {'18783':'components/ts_webui/web', '18789':'output/macos27-20260929-card-revision/after/web_optimized', '18790':'output/macos27-20260929-native-rework/build/web_optimized', '18791':'output/macos27-20260929-native-rework/build-v2/web_optimized', '18793':'output/macos27-20260929-native-rework/build-v3/web_optimized', '18795':'output/macos27-20260929-native-rework/build-v5/web_optimized', '18796':'output/macos27-20260929-native-rework/build-v6/web_optimized', '18798':'output/macos27-20260929-native-rework/build-v7/web_optimized'}[port];
const identity = () => Object.fromEntries(['index.html','css/style.css','js/app.js'].map(name => [name,crypto.createHash('sha256').update(fs.readFileSync(path.join(webRoot,name))).digest('hex')]));
(async () => {
    fs.mkdirSync(out, { recursive: true });
    if (phase === 'built') for (const side of ['after','baseline','before']) fs.mkdirSync(path.join(evidenceRoot,side),{recursive:true});
    const browser = await chromium.launch({ channel: 'chrome', headless: true });
    try {
        for (const name of names) {
            if (!/^[a-z0-9-]+$/.test(name)) throw new Error('Invalid procedure name');
            if(fs.existsSync(path.join(out,name+'.json')))throw Error('Preserve existing procedure evidence: '+name);
            const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
            const started = new Date().toISOString();
            const before = identity();
            try {
                // Local file hashes alone cannot establish what a stale server serves.
                for (const asset of ['css/style.css', 'js/app.js']) {
                    const response = await fetch('http://127.0.0.1:' + port + '/' + asset);
                    if (!response.ok || !Buffer.from(await response.arrayBuffer()).equals(fs.readFileSync(path.join(webRoot, asset)))) {
                        throw new Error('Served artifact mismatch: ' + asset);
                    }
                }
                await page.goto('http://127.0.0.1:' + port + '/?state=populated#/');
                const source = fs.readFileSync(path.join(__dirname, name + '.js'), 'utf8')
                    .replaceAll('output/macos27-20260929-native-rework', evidenceRoot)
                    .replaceAll('output/macos27-20260928', evidenceRoot)
                    .replace("?'after':'baseline'", "?'" + (phase === 'built' ? 'after' : phase) + "':'" + (phase === 'built' ? 'after' : phase) + "'")
                    .replaceAll('18783', port);
                const result = await eval('(' + source + ')')(page);
                fs.writeFileSync(path.join(out, name + '.json'), JSON.stringify(result, null, 2));
                const after = identity();
                const stable = JSON.stringify(before) === JSON.stringify(after);
                fs.writeFileSync(path.join(out, name + '.identity.json'), JSON.stringify({started,ended:new Date().toISOString(),browser:browser.version(),origin:'http://127.0.0.1:'+port,webRoot,before,after,stable,servedAssetsCompared:['css/style.css','js/app.js'],scope:'Artifact identity only; fixture-injected HTML is not byte-identical; screenshot capture is not visual acceptance'},null,2));
                if (!stable) throw new Error('Source changed during procedure: '+name);
                console.log('Recorded', phase, name);
            } finally {
                await page.close();
            }
        }
    } finally {
        await browser.close();
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
