// Local source regression for the task-table scroll wrapper, not device proof.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out=process.argv[2];
assert(out?.startsWith('output/macos27-20260929-native-rework/')&&!fs.existsSync(out));
fs.mkdirSync(out,{recursive:true});
const build=process.argv.find(a=>a.startsWith('--build='))?.slice(8);
assert(!build||/^build-v\d+$/.test(build));
const port=process.argv.find(a=>a.startsWith('--port='))?.slice(7)||'18783';
assert(/^\d{4,5}$/.test(port));
const web=build?'output/macos27-20260929-native-rework/'+build+'/web_optimized':'components/ts_webui/web',origin='http://127.0.0.1:'+port;
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const data=structuredClone(require('./profiles.cjs')['system/memory_detail']);
const taskNames=['fixture-task','fixture-task-with-a-long-descriptive-name','x'.repeat(96)];
data.tasks=[128,640,384].map((hwm,i)=>({...data.tasks[0],name:taskNames[i],stack_hwm:hwm,stack_used:4096-hwm,cpu_percent:10+i}));
data.task_count=3;data.total_stack_allocated=12288;
const result={scope:'Synthetic local data; task table layout, keyboard, sorting, refresh and close only',web,origin,before:identity(),rows:[],errors:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{
 for(const p of ['css/style.css','js/app.js']){const r=await fetch(origin+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
 const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();
 try{
  for(const variant of ['cpu','no-cpu','short','empty'])for(const language of ['zh-CN','en-US'])for(const [width,height]of [[320,600],[390,844],[768,600],[1024,800],[1440,900],[844,390],[390,390]]){
   const fixture=structuredClone(data);
   if(variant==='no-cpu')fixture.tasks.forEach(t=>delete t.cpu_percent);
   if(variant==='short')fixture.tasks.forEach((t,i)=>t.name=['fixture-task','idle-0','worker'][i]);
   if(variant==='empty'){fixture.tasks=[];fixture.task_count=0;fixture.total_stack_allocated=0;}
   const page=await browser.newPage({viewport:{width,height}});
   page.on('pageerror',e=>result.errors.push(String(e)));
   let requests=0;
   await page.route('**/api/v1/system/memory_detail',r=>{requests++;return r.fulfill({json:{code:0,data:fixture}});});
   try{
    await page.goto(origin+'/?state=populated&lang='+language+'#/');
    await page.locator('[onclick="showMemoryDetailModal()"]').click();
    await page.locator('#memory-detail-body .memory-history').waitFor();
    const prefix=variant+'-'+language+'-'+width+'x'+height;
    const table=page.locator('#task-memory-table'),scroll=page.locator('.memory-task-scroll');
    if(variant==='empty'){
     assert.equal(await table.count(),0);assert.equal(await scroll.count(),0);
     await page.locator('#memory-detail-body .memory-history').scrollIntoViewIfNeeded();
     await page.screenshot({path:out+'/'+prefix+'.png'});
     await page.locator('#memory-detail-modal .modal-close').click();await page.locator('#memory-detail-modal').waitFor({state:'hidden'});
     result.rows.push({variant,language,width,height,emptyTableAbsent:'PASS',close:'PASS'});save();continue;
    }
    await table.waitFor();assert.equal(await table.locator('tbody tr').count(),3);
    assert.equal(await table.locator('thead th').count(),variant==='no-cpu'?7:8);
    const readOrder=()=>table.locator('tbody tr td:first-child').allTextContents();
    const names=ids=>ids.map(i=>fixture.tasks[i].name);
    assert.deepEqual(await readOrder(),names([0,2,1]));
    const heading=table.locator('[data-sort="stack_hwm"]');
    await heading.click();assert.deepEqual(await readOrder(),names([1,2,0]));
    await heading.click();assert.deepEqual(await readOrder(),names([0,2,1]));
    await scroll.scrollIntoViewIfNeeded();
    const headers=await table.locator('th').evaluateAll(es=>es.map(e=>{const s=getComputedStyle(e);const r=document.createRange();r.selectNodeContents(e);return{text:e.textContent,whiteSpace:s.whiteSpace,rects:[...r.getClientRects()].map(b=>({y:b.y,height:b.height}))};}));
    assert(headers.every(h=>h.whiteSpace==='nowrap'&&new Set(h.rects.map(r=>Math.round(r.y))).size===1),'Header wrapped');
    const nameLayout=await table.locator('tbody tr td:first-child').evaluateAll(es=>es.map(e=>{
     const n=e.querySelector('code'),range=document.createRange();range.selectNodeContents(n);
     const cell=e.getBoundingClientRect(),rects=[...range.getClientRects()];
     return {name:n.textContent,width:cell.width,lines:new Set(rects.map(r=>Math.round(r.y))).size,
      within:rects.every(r=>r.left>=cell.left-1&&r.right<=cell.right+1)};
    }));
    assert.equal(nameLayout.find(x=>x.name==='fixture-task').lines,1,'Short task name unnecessarily wrapped');
    assert(nameLayout.every(x=>x.within),'Task name escapes its cell');
    assert(nameLayout.every(x=>x.width<=240),'Task-name column consumes excessive horizontal space');
    const geometry=await page.evaluate(()=>{const e=document.querySelector('.memory-task-scroll');return{innerWidth,innerHeight,pageWidth:document.documentElement.scrollWidth,clientWidth:e.clientWidth,scrollWidth:e.scrollWidth};});
    assert(geometry.pageWidth<=width,'Page horizontal overflow');
    await scroll.evaluate(e=>e.scrollLeft=0);
    await scroll.focus();await page.keyboard.press('Tab');await page.keyboard.press('Shift+Tab');
    assert(await scroll.evaluate(e=>document.activeElement===e),'Tab did not return to scroll region');
    const focus=await scroll.evaluate(e=>({outline:getComputedStyle(e).outlineStyle,name:document.getElementById(e.getAttribute('aria-labelledby')).textContent}));
    assert.equal(focus.outline,'solid');assert(focus.name.length>0);
    await page.keyboard.press('ArrowRight');await page.waitForTimeout(250);
    assert(await scroll.evaluate(e=>e.scrollLeft>0),'Keyboard did not scroll');
    await scroll.evaluate(e=>e.scrollLeft=0);
    await page.screenshot({path:out+'/'+prefix+'-left.png'});
    await scroll.evaluate(e=>e.scrollLeft=e.scrollWidth);
    assert(await table.locator('th').last().evaluate(e=>{const r=e.getBoundingClientRect(),p=e.closest('.memory-task-scroll').getBoundingClientRect();return r.right<=p.right+1&&r.left>=p.left-1;}),'Last column inaccessible');
    await page.screenshot({path:out+'/'+prefix+'-right.png'});
    const refresh=page.locator('#memory-detail-modal [onclick="refreshMemoryDetail()"]');
    const previous=requests,oldBody=await table.locator('tbody').elementHandle();
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/system/memory_detail')),refresh.click()]);
    await page.waitForFunction(old=>!old.isConnected,oldBody);await oldBody.dispose();
    await page.waitForFunction(()=>document.querySelectorAll('#task-table-body tr').length===3);
    assert.equal(requests,previous+1);assert.deepEqual(await readOrder(),names([0,2,1]));
    await page.locator('#memory-detail-modal .modal-close').click();await page.locator('#memory-detail-modal').waitFor({state:'hidden'});
    result.rows.push({variant,language,width,height,headers,nameLayout,geometry,focus,keyboardScroll:'PASS',sorting:'PASS',lastColumn:'PASS',refreshRequests:requests,close:'PASS'});save();
   }finally{await page.close();}
  }
  result.after=identity();assert.deepEqual(result.before,result.after);assert.equal(result.errors.length,0);result.completed=true;save();
 }catch(e){result.errors.push(String(e));save();throw e;}finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
