const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const [port,out]=process.argv.slice(2);
assert(['18783','18791','18793','18795','18813'].includes(port)&&out&&!fs.existsSync(out));fs.mkdirSync(out,{recursive:true});
const web=port==='18783'?'components/ts_webui/web':`output/macos27-20260929-native-rework/build-v${{'18791':2,'18793':3,'18795':5,'18813':19}[port]}/web_optimized`;
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const result={before:identity(),port,scope:'Local synthetic log entries rendered by actual production renderer; no real logs/device writes',rows:[],errors:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{
 for(const p of ['css/style.css','js/app.js']){
  const response=await fetch('http://127.0.0.1:'+port+'/'+p);assert(response.ok);
  assert(Buffer.from(await response.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)),'Served artifact mismatch: '+p);
 }
 const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
 for(const lang of ['zh-CN','en-US'])for(const [width,height]of [[320,900],[390,900],[768,900],[1440,900],[844,390],[390,390]]){
  const page=await browser.newPage({viewport:{width,height}});page.on('pageerror',e=>result.errors.push(String(e)));
  try{
   await page.route('**/*',r=>{const q=r.request(),u=new URL(q.url());if(u.origin!=='http://127.0.0.1:'+port||(q.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(u.pathname))){result.errors.push('Blocked unexpected request '+q.method()+' '+q.url());return r.abort();}return r.continue();});
   await page.goto('http://127.0.0.1:'+port+'/?state=populated&lang='+lang+(port==='18813'?'&logStress=1':'')+'#/logs');
   await page.locator('#terminal-logs-modal').waitFor();await page.locator('.log-entry').first().waitFor();
   await page.locator('#modal-log-level-filter').selectOption('5');
   if(port!=='18813')await page.evaluate(()=>{
    modalLogEntries.splice(0,modalLogEntries.length,...Array.from({length:20},(_,i)=>({timestamp:i*1000,level:i%5+1,levelName:['ERROR','WARN','INFO','DEBUG','VERBOSE'][i%5],tag:'fixture-tag-'+('long-'.repeat(12)),task:'fixture-task-'+('long-'.repeat(20)),message:'message '+i+' · 可读日志内容 '.repeat(10)+' https://example.invalid/'+('path'.repeat(20))})));
    renderModalLogs();
   });
   assert.equal(await page.locator('.log-entry').count(),20);
   const geometry=await page.locator('.log-entry').evaluateAll(es=>es.map(e=>{
    const b=e.getBoundingClientRect();return{width:b.width,children:[...e.children].map(c=>{const r=c.getBoundingClientRect();return{class:c.className,width:r.width,within:r.left>=b.left-1&&r.right<=b.right+1,text:c.textContent};})};
   }));
   for(const row of geometry){assert(row.children.every(x=>x.within),'Log child horizontal overflow');assert(row.children.find(x=>x.class==='log-message').width>=Math.min(180,row.width-22),'Message squeezed');assert.deepEqual(row.children.map(x=>x.class),['log-time','log-level','log-tag','log-message','log-task']);}
   const viewer=page.locator('#modal-log-container');
   assert(await viewer.evaluate(e=>e.getBoundingClientRect().height<=e.parentElement.clientHeight+1),'Viewer clipped by parent');
   assert(await viewer.evaluate(e=>Math.abs(e.scrollHeight-e.clientHeight-e.scrollTop)<2),'Auto-scroll did not reach bottom');
   await page.locator('#modal-log-auto-scroll').uncheck();
   for(const position of [0,.5,1]){
    await viewer.evaluate((e,p)=>e.scrollTop=p*(e.scrollHeight-e.clientHeight),position);
    await viewer.scrollIntoViewIfNeeded();
    await page.screenshot({path:out+'/'+lang+'-'+width+'x'+height+'-'+position+'.png'});
   }
   await page.locator('#modal-log-tag-filter').fill('absent-tag');await page.locator('#modal-log-tag-filter').press('End');await page.waitForTimeout(350);assert.equal(await page.locator('.log-entry').count(),0);
   await page.locator('#modal-log-tag-filter').fill('fixture-tag');await page.locator('#modal-log-tag-filter').press('End');await page.waitForTimeout(350);assert.equal(await page.locator('.log-entry').count(),20);
   await page.locator('#modal-log-level-filter').selectOption('1');assert.equal(await page.locator('.log-entry').count(),4);
   await page.locator('#modal-log-keyword-filter').fill('message 0');await page.locator('#modal-log-keyword-filter').press('End');await page.waitForTimeout(350);assert.equal(await page.locator('.log-entry').count(),1);
   await page.locator('#terminal-logs-modal .modal-close').click();await page.locator('#terminal-logs-modal').waitFor({state:'hidden'});
   result.rows.push({lang,width,height,geometry,filters:'PASS',autoScroll:'PASS',close:'PASS'});save();
  }finally{await page.close();}
 }
 result.after=identity();assert.deepEqual(result.after,result.before);assert.equal(result.errors.length,0,'Browser runtime errors');result.completed=true;save();
}catch(e){result.errors.push(String(e));save();throw e;}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1});
