// Compare exact original and candidate JS under the same current presentation shell.
// This isolates an existing handler/duplicate-ID issue; it is not baseline visual QA.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto'),cp=require('node:child_process');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const origin='http://127.0.0.1:18809',out='output/macos27-20260929-native-rework/widget-preview-diagnostic-v17-run4';assert(!fs.existsSync(out));fs.mkdirSync(out);
const original=cp.execFileSync('git',['show','HEAD:components/ts_webui/web/js/app.js'],{encoding:'utf8',maxBuffer:8*1024*1024});
const candidate=fs.readFileSync('output/macos27-20260929-native-rework/build-v17/web_optimized/js/app.js','utf8');
const hash=s=>crypto.createHash('sha256').update(s).digest('hex');
const report={scope:'Original JS vs candidate JS, identical current HTML/CSS and synthetic fixture; diagnostic only',rows:[],errors:[],completed:false};
(async()=>{let browser;try{browser=await chromium.launch({channel:'chrome',headless:true});report.browser=browser.version();
for(const [side,source] of [['original',original],['candidate',candidate]]){
const page=await browser.newPage({viewport:{width:390,height:700}}),writes=[],errors=[];page.on('pageerror',e=>errors.push(String(e)));
await page.route('**/js/app.js?*',r=>r.fulfill({body:source,contentType:'application/javascript'}));
await page.route('**/api/**',r=>{const q=r.request(),u=new URL(q.url());assert.equal(u.origin,origin);if(q.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(u.pathname)){writes.push({url:q.url(),method:q.method(),body:q.postData()});return r.fulfill({status:409,json:{code:-1,message:'Diagnostic blocks persistence'}});}return r.continue();});
await page.goto(origin+'/?state=populated&lang=en-US#/');await page.locator('.dw-card').first().waitFor();await page.locator('[onclick="showWidgetManager()"]').click();await page.locator('[onclick="showWidgetEditPanel(\'fixture-log\')"]').first().click();
const capture=()=>page.evaluate(()=>({containers:[...document.querySelectorAll('[id="dw-fixture-log-log"]')].map(e=>({preview:!!e.closest('#dw-preview-card'),class:e.className})),buttons:[...document.querySelectorAll('[id="dw-fixture-log-collapse"]')].map(e=>({preview:!!e.closest('#dw-preview-card'),title:e.title,html:e.innerHTML})),stored:localStorage.getItem('data_widgets_v2')}));
const before=await capture();await page.locator('#dw-preview-card .dw-log-collapse-btn').click();await page.waitForTimeout(100);const firstClick=await capture();await page.locator('#dw-preview-card .dw-log-collapse-btn').click();await page.waitForTimeout(100);const after=await capture();report.rows.push({side,jsSha256:hash(source),before,firstClick,after,writes,errors});
assert.equal(before.containers.length,2);assert.equal(after.containers.length,2);assert.notEqual(before.containers[0].class,after.containers[0].class);assert.equal(before.containers[1].class,after.containers[1].class);assert.equal(before.buttons[1].html,after.buttons[1].html);assert.equal(writes.length,2);assert.equal(errors.length,0);
await page.close();}
report.completed=true;
}catch(e){report.errors.push(String(e));throw e;}finally{fs.writeFileSync(out+'/raw.json',JSON.stringify(report,null,2));if(browser)await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
