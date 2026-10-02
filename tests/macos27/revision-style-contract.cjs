const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out='output/macos27-20260929-card-revision';
(async()=>{const browser=await chromium.launch({channel:'chrome',headless:true});const results=[];try{
for(const width of [390,1440]){
 const page=await browser.newPage({viewport:{width,height:900}});
 let release;const blocked=new Promise(r=>release=r);
 await page.route('**/css/style.css*',async route=>{await blocked;await route.continue();});
 await page.goto('http://127.0.0.1:18783/?state=populated#/automation',{waitUntil:'domcontentloaded'});
 const read=()=>page.evaluate(()=>Object.fromEntries(['.header','.nav','.logo','.main','#page-content','.header .btn'].map(sel=>{const s=getComputedStyle(document.querySelector(sel));return[sel,Object.fromEntries(['fontFamily','fontSize','fontWeight','borderRadius','backgroundColor','position','top','left','right','marginTop','paddingTop','paddingRight','paddingBottom','paddingLeft'].map(k=>[k,s[k]]))]})));
 const critical=await read();release();await page.waitForFunction(()=>[...document.styleSheets].some(s=>s.href?.includes('/css/style.css')));await page.waitForTimeout(300);const full=await read();
 // Header compact layout and active page content are owned by the same rules.
 for(const sel of ['.header','.nav','.logo','.main','#page-content'])assert.deepEqual(critical[sel],full[sel],sel+' first-paint drift at '+width);
 results.push({width,critical,full,status:'PASS'});await page.close();
}
const page=await browser.newPage({viewport:{width:1440,height:1000}});
for(const mode of ['off','manual','auto','curve']){
 await page.route('**/api/v1/fan/status*',async route=>{const response=await route.fetch();const json=await response.json();for(const fan of json.data.fans)fan.mode=mode;await route.fulfill({json});});
 await page.goto('http://127.0.0.1:18783/?state=populated&fanTest='+mode+'#/');await page.locator('.fan-mode-tab.active.'+mode).first().waitFor();
 const style=await page.locator('.fan-mode-tab.active.'+mode).first().evaluate(e=>{const s=getComputedStyle(e);return{color:s.color,background:s.backgroundColor,font:s.fontFamily,size:s.fontSize}});
 const rgb=s=>s.match(/[\d.]+/g).slice(0,3).map(Number);const lum=s=>rgb(s).map(v=>v/255).map(v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4).reduce((sum,v,i)=>sum+v*[.2126,.7152,.0722][i],0);const a=lum(style.color),b=lum(style.background);const contrast=(Math.max(a,b)+.05)/(Math.min(a,b)+.05);assert(contrast>=4.5,mode+' contrast '+contrast);
 results.push({mode,...style,contrast,status:'PASS'});await page.unroute('**/api/v1/fan/status*');
}
await page.goto('http://127.0.0.1:18783/?state=populated#/files');await page.locator('.tab-btn.active').waitFor();const tabs=await page.locator('.storage-tabs .tab-btn').evaluateAll(es=>es.map(e=>({active:e.classList.contains('active'),background:getComputedStyle(e).backgroundColor,color:getComputedStyle(e).color})));assert.notEqual(tabs[0].background,tabs[1].background);results.push({storageTabs:tabs,status:'PASS'});
}finally{fs.writeFileSync(out+'/style-contract.json',JSON.stringify(results,null,2));await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1});
