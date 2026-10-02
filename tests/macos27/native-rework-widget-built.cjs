// Replay the reviewed widget entry procedure against the current release files.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const origin='http://127.0.0.1:18809',web='output/macos27-20260929-native-rework/build-v17/web_optimized';
const out='output/macos27-20260929-native-rework/widget-built-v17';assert(!fs.existsSync(out));fs.mkdirSync(out);
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const report={before:identity(),errors:[],writes:[],completed:false};
(async()=>{let browser;try{
for(const p of ['css/style.css','js/app.js'])assert(Buffer.from(await(await fetch(origin+'/'+p)).arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));
browser=await chromium.launch({channel:'chrome',headless:true});report.browser=browser.version();const page=await browser.newPage();
await page.route('**/api/**',route=>{const req=route.request(),u=new URL(req.url());assert.equal(u.origin,origin);if(req.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(u.pathname)){report.writes.push({url:req.url(),method:req.method()});return route.abort();}return route.continue();});
await page.goto(origin+'/?state=populated&lang=en-US#/');
const source=fs.readFileSync('tests/macos27/native-rework-widget-layout.js','utf8').replaceAll('http://127.0.0.1:18783',origin).replaceAll('output/macos27-20260928/after',out);
report.procedureSha256=crypto.createHash('sha256').update(source).digest('hex');
report.result=await eval('('+source+')')(page);report.after=identity();assert.deepEqual(report.before,report.after);assert.equal(report.writes.length,0);report.completed=true;
}catch(e){report.errors.push(String(e));throw e;}finally{fs.writeFileSync(out+'/raw.json',JSON.stringify(report,null,2));if(browser)await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
