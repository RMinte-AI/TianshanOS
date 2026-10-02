const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
(async()=>{
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'macos27-dialog-zoom-'));
 const context=await chromium.launchPersistentContext(dir,{channel:'chrome',headless:true,viewport:null,args:['--window-size=1440,1000']});
 const results=[],errors=[];
 try {
  const page=context.pages()[0],cdp=await context.newCDPSession(page);page.on('pageerror',e=>errors.push(e.message));
  await page.goto('chrome://settings/appearance');await page.locator('#zoomLevel').selectOption('2');
  await page.route('http://127.0.0.1:18783/api/v1/automation/variables/list*',r=>r.fulfill({json:{code:0,data:{variables:Array.from({length:20},(_,i)=>({name:'fixture.'+'long_variable_'.repeat(4)+i,source_id:'fixture-source',type:'number',value:i}))}}}));
  const click=handler=>page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
  for(const lang of ['en-US','zh-CN'])for(const entry of ['timezone','memory','led','command','variable']) {
   await page.goto('http://127.0.0.1:18783/?state=populated&lang='+lang+'#'+(entry==='command'?'/commands':'/'));await page.reload();
   let selector,primary;
   if(entry==='command') {await page.locator('[data-host-id="fixture-host"]').click();await click('showAddCommandModal()');await page.locator('.advanced-options summary').click();await page.locator('#cmd-nohup').check();await page.locator('#cmd-service-mode').check();selector='#command-modal';primary='[onclick="saveCommand()"]';}
   if(entry==='timezone'){await click('showTimezoneModal()');selector='#timezone-modal';primary='[onclick="applyTimezone()"]';}
   if(entry==='memory'){await click('showMemoryDetailModal()');selector='#memory-detail-modal';}
   if(entry==='led'){await click("openLedModal('matrix', 'colorcorrection')");selector='#led-modal';primary='[onclick="applyColorCorrection()"]';}
   if(entry==='variable'){await click('showWidgetManager()');await click("showWidgetEditPanel('fixture-ring')");await click('selectVariableForWidget()');selector='#variable-select-modal';await page.locator(selector+' .var-group-header').first().waitFor();await page.waitForFunction(()=>document.activeElement?.id==='var-search');await page.locator(selector+' .var-group-header').first().click();}
   const modal=page.locator(selector);await page.evaluate(()=>document.fonts.ready);
   for(const [position,fraction] of [['top',0],['middle',0.5],['bottom',1]]) {
    const scroll=await modal.evaluate((e,f)=>[...e.querySelectorAll('*')].filter(n=>['auto','scroll'].includes(getComputedStyle(n).overflowY)&&n.scrollHeight>n.clientHeight+1).map(n=>{n.scrollTop=(n.scrollHeight-n.clientHeight)*f;return {class:n.className,top:n.scrollTop,height:n.clientHeight,total:n.scrollHeight};}),fraction);
    const state=await modal.evaluate(e=>{
     const shell=e.querySelector('.modal-content'),r=shell.getBoundingClientRect();
     return {innerWidth,innerHeight,dpr:devicePixelRatio,overflow:document.documentElement.scrollWidth>innerWidth,rect:r.toJSON(),clipped:[...e.querySelectorAll('input,select,button')].filter(n=>n.getClientRects().length).filter(n=>{const b=n.getBoundingClientRect();return b.left<r.left-1||b.right>r.right+1;}).map(n=>({id:n.id,text:n.textContent})),close:[...e.querySelectorAll('.modal-close')].map(n=>n.getBoundingClientRect().toJSON())};
    });
    if(state.dpr!==2||state.innerWidth!==720||state.overflow||state.clipped.length||state.rect.y<0||state.rect.bottom>state.innerHeight+1||state.close.some(r=>r.y<0||r.bottom>state.innerHeight))throw new Error(JSON.stringify({entry,lang,position,...state}));
    if(position==='bottom'&&primary){const r=await modal.locator(primary).boundingBox();if(!r||r.y<0||r.y+r.height>state.innerHeight+1)throw new Error('Primary action unreachable: '+entry);}
    const shot=await cdp.send('Page.captureScreenshot',{format:'png',fromSurface:true}),bytes=Buffer.from(shot.data,'base64');
    const screenshot='output/macos27-20260928/after/zoom-dialog-'+entry+'-'+lang+'-'+position+'.png';fs.writeFileSync(screenshot,bytes);if(bytes.readUInt32BE(16)!==1440)throw new Error('Wrong screenshot capture width');
    results.push({entry,lang,position,screenshot,scroll,...state});
   }
   if(entry==='timezone')await modal.locator('[onclick="hideTimezoneModal()"]').click();else await modal.locator('.modal-close').click();
   await modal.waitFor({state:'hidden'});
  }
  if(errors.length)throw new Error(JSON.stringify(errors));
 } finally {fs.writeFileSync('output/macos27-20260928/after/native-rework-zoom-dialogs.json',JSON.stringify({scope:'Actual 200% zoom in disposable profile; local synthetic data; no save/device operations. Geometry, primary reachability and close only; visual inspection separate.',results,errors},null,2));await context.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
