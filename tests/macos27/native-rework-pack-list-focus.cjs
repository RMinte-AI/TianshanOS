const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const version=process.argv[3]||'v15';assert(['v15','v16','v17'].includes(version));
const origin='http://127.0.0.1:'+({v15:18807,v16:18808,v17:18809}[version]),web='output/macos27-20260929-native-rework/build-'+version+'/web_optimized';
const out='output/macos27-20260929-native-rework/pack-list-'+version+(process.argv[2]||'');assert(!fs.existsSync(out));fs.mkdirSync(out);
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const result={scope:'Synthetic local config-pack list, keyboard navigation only; import is never activated.',before:identity(),rows:[],errors:[],writes:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{for(const p of ['css/style.css','js/app.js']){const r=await fetch(origin+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
let browser;try{browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();
for(const language of ['en-US','zh-CN'])for(const width of [320,390,768,780,781,820,900,1024,1440]){
 const page=await browser.newPage({viewport:{width,height:600}});page.on('pageerror',e=>result.errors.push(String(e)));
 await page.route('**/api/**',async route=>{const req=route.request(),url=new URL(req.url());assert.equal(url.origin,origin);
 if(url.pathname==='/api/v1/config/pack/list'){assert.equal(req.method(),'GET');return route.fulfill({json:{code:0,data:{files:Array.from({length:5},(_,i)=>({name:'fixture-long-configuration-name-'+i+'.tscfg',size:1024,signer:'fixture-signer-'+i,is_official:i%2===0,valid:i%2===0}))}}});}
 if(req.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(url.pathname)){result.writes.push({url:req.url(),method:req.method()});return route.abort();}return route.continue();});
 try{await page.goto(origin+'/?state=populated&role=root&lang='+language+'#/security');await page.locator('[onclick="showConfigPackListModal()"]').click();await page.locator('#pack-list-table:not(.hidden) tbody button').first().waitFor();await page.evaluate(()=>document.fonts.ready);
 const modal=page.locator('#pack-list-modal'),buttons=modal.locator('tbody button');assert.equal(await buttons.count(),5);const checks=[];
 await modal.locator('[onclick="refreshConfigPackList()"]').focus();
 for(const direction of ['forward','reverse'])for(let j=0;j<5;j++){
 const i=direction==='forward'?j:4-j;
 if(direction==='forward')await page.keyboard.press('Tab');else if(j>0)await page.keyboard.press('Shift+Tab');
 await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
 checks.push(await buttons.nth(i).evaluate((n,args)=>{const b=n.getBoundingClientRect(),o=n.closest('.security-table-scroll').getBoundingClientRect(),m=n.closest('.modal-content').getBoundingClientRect();return{...args,text:n.textContent,handler:n.getAttribute('onclick'),focused:document.activeElement===n,rect:b.toJSON(),owner:o.toJSON(),modal:m.toJSON(),visible:b.left>=Math.max(o.left,m.left,0)&&b.right<=Math.min(o.right,m.right,innerWidth)&&b.top>=Math.max(o.top,m.top,0)&&b.bottom<=Math.min(o.bottom,m.bottom,innerHeight),outline:getComputedStyle(n).outline,scrollLeft:n.closest('.security-table-scroll').scrollLeft};},{direction,index:i}));
 }
 const statuses=await modal.locator('tbody td:nth-child(5) span').evaluateAll(ns=>ns.map(n=>({text:n.textContent,color:getComputedStyle(n).color,opacity:getComputedStyle(n).opacity,rect:n.getBoundingClientRect().toJSON(),cell:n.parentElement.getBoundingClientRect().toJSON()})));
 const screenshot=out+'/'+language+'-'+width+'.png';await page.screenshot({path:screenshot});const viewport=await page.evaluate(()=>({innerWidth,innerHeight,scale:visualViewport.scale,scrollY,overflow:document.documentElement.scrollWidth>innerWidth}));
 await modal.locator('[onclick="hideConfigPackListModal()"]').click();await assert.doesNotReject(()=>modal.waitFor({state:'hidden'}));result.rows.push({language,width,viewport,checks,statuses,screenshot});save();
 }finally{await page.close();}
}
result.after=identity();assert.deepEqual(result.before,result.after);result.completed=true;save();assert.equal(result.rows.length,18);assert.equal(result.errors.length,0);assert.equal(result.writes.length,0);assert(result.rows.every(r=>!r.viewport.overflow&&r.checks.every(c=>c.focused&&c.visible)),'Pack list focused action clipped or focus order changed');
}catch(e){result.errors.push(String(e));throw e;}finally{save();if(browser)await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
