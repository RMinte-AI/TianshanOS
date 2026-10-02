// Existing procedures, isolated output, and an immutable source identity per run.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { createRequire } = require('node:module');
const name = process.argv[2];
const allowed = new Set(['rare-surfaces','remaining-surfaces','deep-conditional','ota-conditional','populated-variables','keyboard-auth','lifecycle-input','language-failure','final-browser','target-size','paint-trace','scroll-performance','interaction-timing','asset-transfer','control-contrast','zoom-profile','native-rework-zoom','native-rework-zoom-dialogs']);
if (!allowed.has(name)) throw new Error('Unknown rework procedure');
const built = process.argv.includes('--built');
const out = 'output/macos27-20260929-native-rework' + (built ? '/built' : '');
const webRoot = built ? 'output/macos27-20260929-native-rework/build/web_optimized' : 'components/ts_webui/web';
const identity = () => Object.fromEntries(['index.html','css/style.css','js/app.js'].map(n => [n,crypto.createHash('sha256').update(fs.readFileSync(path.join(webRoot,n))).digest('hex')]));
const before = identity();
const started = new Date().toISOString();
for (const side of ['baseline','after']) fs.mkdirSync(path.join(out,side),{recursive:true});
process.once('exit',code => {
    const after = identity();
    const stable = JSON.stringify(before)===JSON.stringify(after);
    fs.writeFileSync(path.join(out,name+'.identity.json'),JSON.stringify({started,ended:new Date().toISOString(),code,webRoot,before,after,stable,scope:'Artifact identity, not acceptance'},null,2));
    if (!stable) { console.error('Artifacts changed during '+name); process.exitCode=1; }
});
const filename = path.join(__dirname,name+'.cjs');
const source = fs.readFileSync(filename,'utf8')
    .replaceAll('output/macos27-20260928',out)
    .replaceAll('18785','18790')
    .replaceAll('18783',built?'18790':'18783');
new Function('require','__dirname','__filename',source)(createRequire(filename),__dirname,filename);
