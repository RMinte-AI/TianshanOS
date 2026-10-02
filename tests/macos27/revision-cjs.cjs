// Reuse audited CommonJS test procedures with an isolated evidence directory.
// Only fixture artifact paths and the B artifact port are substituted.
const fs = require('node:fs');
const path = require('node:path');
const { createRequire } = require('node:module');
const name = process.argv[2];
const allowed = new Set(['rare-surfaces','remaining-surfaces','deep-conditional','ota-conditional','populated-variables','keyboard-auth','lifecycle-input','language-failure','final-browser','target-size','paint-trace','scroll-performance','interaction-timing','asset-transfer','control-contrast','zoom-profile']);
if (!allowed.has(name)) throw new Error('Unknown revision procedure');
const built = process.argv.includes('--built');
const out = 'output/macos27-20260929-card-revision' + (built ? '/built' : '');
for (const side of ['baseline','after']) fs.mkdirSync(path.join(out, side), { recursive: true });
const filename = path.join(__dirname, name + '.cjs');
const source = fs.readFileSync(filename, 'utf8')
    .replaceAll('output/macos27-20260928', out)
    .replaceAll('18785', '18789')
    .replaceAll('18783', built ? '18789' : '18783');
new Function('require', '__dirname', '__filename', source)(createRequire(filename), __dirname, filename);
