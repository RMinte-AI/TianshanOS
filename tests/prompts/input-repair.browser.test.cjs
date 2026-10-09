const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let server,browser,base;
before(async()=>{
 server=http.createServer((req,res)=>{
  const rel=new URL(req.url,'http://local').pathname,file=path.resolve(root,'.'+(rel==='/'?'/index.html':rel));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css','.woff2':'font/woff2'})[path.extname(file)]||'application/octet-stream');res.end(data);});
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
after(async()=>{await browser?.close();await new Promise(resolve=>server?.close(resolve));});
async function fixture(language='zh-CN'){
 const context=await browser.newContext({viewport:{width:1280,height:900}});
 await context.addInitScript(language=>{localStorage.setItem('ts_language',language);class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;},language);
 const page=await context.newPage(),errors=[];page.setDefaultTimeout(5000);page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
  const u=new URL(route.request().url());if(u.origin!==base){await route.abort();return;}
  if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{}}});return;}
  await route.continue();
 });
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&typeof editAction==='function');
 await page.evaluate(()=>{
  closeLoginModal();window.writes=[];window.stored=null;
  api.call=async(endpoint,params)=>{
   if(endpoint==='automation.actions.get')return {code:0,data:structuredClone(stored)};
   if(['automation.actions.add','automation.actions.update','automation.actions.delete','ui.widgets.set'].includes(endpoint)){
    writes.push({endpoint,params:structuredClone(params)});
    if(window.rejectSave)return {code:500,message:'simulated storage failure'};
    if(endpoint.startsWith('automation.actions.'))stored=structuredClone(params);
   }
   return {code:0,data:{}};
  };
  api.ledList=async()=>({code:0,data:{devices:[{name:'matrix',count:256},{name:'board',count:28}]}});
 });
 return {page,context,errors};
}
for(const language of ['zh-CN','en-US']){
 test(`${language}: widget zero, negative bound, expressions and thresholds survive save/reopen`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(()=>{
    dataWidgets=[{id:'fixture',type:'number',label:'Number',icon:'ri-dashboard-line',decimals:0,min:0,max:0,expression:'Math.max("value", 1)',layout:'auto'}];
    showWidgetEditPanel('fixture');
   });
   assert.equal(await f.page.locator('#edit-decimals').inputValue(),'0');assert.equal(await f.page.locator('#edit-max').inputValue(),'0');
   assert.equal(await f.page.locator('#edit-expression').inputValue(),'Math.max("value", 1)');
   await f.page.evaluate(()=>saveWidgetEdit('fixture'));assert.equal(await f.page.evaluate(()=>writes.length),1);
   await f.page.evaluate(()=>{dataWidgets=loadDataWidgetsFromLocalStorage();showWidgetEditPanel('fixture');});
   assert.equal(await f.page.locator('#edit-max').inputValue(),'0');assert.equal(await f.page.locator('#edit-decimals').inputValue(),'0');
   await f.page.evaluate(()=>{document.getElementById('widget-edit-modal')?.remove();dataWidgets=[{id:'status',type:'status',label:'Status',decimals:0,thresholds:[0,0,0]}];showWidgetEditPanel('status');});
   for(let i=1;i<=3;i++)assert.equal(await f.page.locator('#edit-threshold-'+i).inputValue(),'0');
   await f.page.evaluate(()=>saveWidgetEdit('status'));assert.deepEqual(await f.page.evaluate(()=>loadDataWidgetsFromLocalStorage()[0].thresholds),[0,0,0]);
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(`${language}: invalid widget numbers retain the whole draft without saving`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(()=>{dataWidgets=[{id:'fixture',type:'bar',label:'Original',decimals:1,min:-1,max:0}];showWidgetEditPanel('fixture');});
   await f.page.locator('#edit-label').fill('Draft');await f.page.locator('#edit-decimals').fill('');
   await f.page.evaluate(()=>saveWidgetEdit('fixture'));assert.equal(await f.page.evaluate(()=>writes.length),0);
   assert.equal(await f.page.evaluate(()=>dataWidgets[0].label),'Original');assert.equal(await f.page.locator('#edit-label').inputValue(),'Draft');
   assert.equal(await f.page.locator('#edit-decimals').getAttribute('aria-invalid'),'true');
   await f.page.locator('#edit-decimals').fill('0');await f.page.evaluate(()=>saveWidgetEdit('fixture'));
   assert.equal(await f.page.evaluate(()=>writes.length),1);assert.equal(await f.page.evaluate(()=>writes[0].params.widgets[0].max),0);
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(`${language}: all 14 available filter templates retain parameters across reopen and two updates`,async()=>{
  const f=await fixture(language);try{
   const names=['pulse','breathing','blink','wave','scanline','glitch','rainbow','sparkle','plasma','sepia','posterize','contrast','invert','grayscale'];
   for(const name of names){
    const expected=await f.page.evaluate(async name=>{
     document.getElementById('action-modal')?.remove();
     const params=Object.fromEntries(filterConfig[name].params.map(p=>[p,paramLabels[p].min]));
     stored={id:'fixture',name:'Filter',type:'led',enabled:false,async:true,delay_ms:0,led:{device:'matrix',ctrl_type:'filter',filter:name,filter_params:params}};
     await editAction('fixture');return params;
    },name);
    for(let save=0;save<2;save++){
     for(const [key,value]of Object.entries(expected))assert.equal(await f.page.locator('#action-filter-'+key).inputValue(),String(value));
     await f.page.evaluate(()=>updateAction('fixture'));
     const write=await f.page.evaluate(()=>writes.at(-1));assert.equal(write.endpoint,'automation.actions.update');
     assert.deepEqual(write.params.led.filter_params,expected);assert.equal(write.params.enabled,false);assert.equal(write.params.async,true);
     await f.page.evaluate(async()=>{document.getElementById('action-modal')?.remove();await editAction('fixture');});
    }
   }
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(`${language}: failed or invalid LED edit keeps its template and never deletes it`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(async()=>{stored={id:'fixture',name:'Original',type:'led',enabled:true,async:false,led:{device:'board',ctrl_type:'fill',color:'#ff0000',brightness:0,index:0}};await editAction('fixture');});
   assert.equal(await f.page.locator('#action-led-brightness').inputValue(),'0');assert.equal(await f.page.locator('#action-led-index').inputValue(),'0');
   await f.page.locator('#action-name').fill('Draft');await f.page.locator('#action-led-index').fill('256');
   await f.page.evaluate(()=>updateAction('fixture'));assert.equal(await f.page.evaluate(()=>writes.length),0);
   await f.page.locator('#action-led-index').fill('0');await f.page.evaluate(()=>{rejectSave=true;});
   await f.page.evaluate(()=>updateAction('fixture'));assert.equal(await f.page.locator('#action-name').inputValue(),'Draft');
   assert.equal(await f.page.evaluate(()=>stored.name),'Original');assert.equal(await f.page.evaluate(()=>writes.at(-1).endpoint),'automation.actions.update');
   await f.page.evaluate(()=>{rejectSave=false;});await f.page.evaluate(()=>updateAction('fixture'));
   assert.equal(await f.page.evaluate(()=>stored.led.brightness),0);assert.equal(await f.page.evaluate(()=>stored.led.index),0);
   assert.equal(await f.page.evaluate(()=>writes.some(w=>w.endpoint==='automation.actions.delete')),false);
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(`${language}: editing only a legacy filter label preserves its default execution`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(async()=>{stored={id:'fixture',name:'Original',type:'led',enabled:true,led:{device:'matrix',ctrl_type:'filter',filter:'wave'}};await editAction('fixture');});
   await f.page.locator('#action-name').fill('Renamed');await f.page.evaluate(()=>updateAction('fixture'));
   assert.equal(await f.page.evaluate(()=>Object.hasOwn(stored.led,'filter_params')),false);
   await f.page.evaluate(async()=>{document.getElementById('action-modal')?.remove();await editAction('fixture');});
   await f.page.locator('#action-filter-angle').evaluate(e=>{e.value=0;e.dispatchEvent(new Event('input',{bubbles:true}));});
   await f.page.evaluate(()=>updateAction('fixture'));assert.equal(await f.page.evaluate(()=>stored.led.filter_params.angle),0);
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(`${language}: modal and matrix manual filters convert frequency and retain zero decay`,async()=>{
  const f=await fixture(language);try{
   const requests=await f.page.evaluate(async()=>{
    document.body.insertAdjacentHTML('beforeend','<input id="modal-filter-frequency" value="100"><input id="modal-filter-intensity" value="0"><input id="filter-decay" value="0"><input id="filter-density" value="0"><input id="filter-speed" value="5">');
    const calls=[];api.call=async(name,params)=>{if(name==='led.filter.start')calls.push(params);return {code:0,data:{}};};
    selectedModalFilter='glitch';await applyFilterFromModal();
    selectedFilter='sparkle';await applySelectedFilter();return calls;
   });
   assert.equal(requests[0].frequency,255);assert.equal(requests[0].intensity,0);
   assert.equal(requests[1].decay,0);assert.equal(requests[1].density,0);assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
}
test('files and image picker preserve special paths, plain names and both directory types',async()=>{
 const f=await fixture();try{
  const name='reviewer\'s "a" & <em> 中文.json';
  await f.page.evaluate(async name=>{
   document.body.insertAdjacentHTML('beforeend','<div id="file-list"></div><div id="breadcrumb"></div><div id="batch-toolbar"></div><span id="selected-count"></span><div id="file-picker-list"></div><div id="file-picker-current-path"></div>');
   api.storageStatus=async()=>({code:0,data:{sd:{mounted:true}}});api.storageList=async()=>({code:0,data:{entries:[{name,type:'file',size:1}]}});
   await loadDirectory('/sdcard');
  },name);
  assert.equal(await f.page.locator('.file-row').getAttribute('data-path'),'/sdcard/'+name);
  assert.equal(await f.page.locator('.file-name em').count(),0);await f.page.locator('.file-checkbox').check();
  assert.deepEqual(await f.page.evaluate(()=>[...selectedFiles]),['/sdcard/'+name]);
  await f.page.evaluate(name=>{window.navigated=null;navigateToPath=p=>{navigated=p;};updateBreadcrumb('/sdcard/'+name);},name);
  await f.page.locator('#breadcrumb .current').click();assert.equal(await f.page.evaluate(()=>navigated),'/sdcard/'+name);
  for(const type of ['dir','directory']){
   await f.page.evaluate(async({name,type})=>{window.browsed=[];api.storageList=async path=>{browsed.push(path);return {code:0,data:{entries:path==='/sdcard'? [{name,type}]:[]}};};await loadFilePickerDirectory('/sdcard');},{name,type});
   await f.page.locator('#file-picker-list [data-path]').click();
   assert.equal(await f.page.evaluate(()=>browsed.at(-1)),'/sdcard/'+name);
  }
  assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});
test('JSON field and command-variable buttons pass unmodified quotes and Unicode',async()=>{
 const f=await fixture();try{
  const name='reviewer\'s "value" & 中文';
  await f.page.evaluate(name=>{
   document.body.insertAdjacentHTML('beforeend','<div id="fixture-vars"><div class="var-list"></div></div><input id="fixture-target"><div id="commands-list"></div>');
   renderVarSelector('fixture-vars',{[name]:1},'fixture-target');
   sshCommands={host:[{id:'cmd',hostId:'host',name:'Fixture',command:'true',varName:name}]};selectedHostId='host';
   window.selectedVariable=null;showCommandVariables=value=>{selectedVariable=value;};updateServiceStatusInList=()=>{};refreshCommandsList();
  },name);
  await f.page.locator('#fixture-vars .var-item').click();assert.equal(await f.page.locator('#fixture-target').inputValue(),name);
  await f.page.locator('.cmd-variables').click();assert.equal(await f.page.evaluate(()=>selectedVariable),name);assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});
test('CSR UI uses UTF-8 byte bounds and preserves the organization data',async()=>{
 const f=await fixture();try{
  await f.page.evaluate(()=>{
   document.body.insertAdjacentHTML('beforeend','<input id="csr-device-id"><input id="csr-org"><input id="csr-ou"><div id="csr-gen-result"></div><div id="csr-result-box"></div><button id="csr-gen-btn"></button><textarea id="csr-pem-output"></textarea>');
   api.certGenerateCSR=async params=>{writes.push(params);return {code:0,data:{csr_pem:'synthetic display-only PEM'}};};
  });
  await f.page.locator('#csr-device-id').fill('中'.repeat(22));await f.page.evaluate(()=>generateCSR());assert.equal(await f.page.evaluate(()=>writes.length),0);
  assert.equal(await f.page.locator('#csr-device-id').getAttribute('aria-invalid'),'true');
  await f.page.locator('#csr-device-id').fill('中'.repeat(21));await f.page.locator('#csr-org').fill('ACME, Ltd\\tail');await f.page.evaluate(()=>generateCSR());
  assert.deepEqual(await f.page.evaluate(()=>writes[0]),{device_id:'中'.repeat(21),organization:'ACME, Ltd\\tail'});
  assert.equal(await f.page.locator('#csr-device-id').getAttribute('aria-invalid'),null);assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});
for(const language of ['zh-CN','en-US'])for(const params of [undefined,{angle:0},{speed:40,wavelength:8,amplitude:128,angle:0}]){
 test(`${language}: ${JSON.stringify(params)} preserves field presence through metadata and one-field edits`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(async params=>{
    stored={id:'fixture',name:'Original',type:'led',enabled:true,led:{device:'matrix',ctrl_type:'filter',filter:'wave'}};
    if(params!==null)stored.led.filter_params=params;
    await editAction('fixture');
   },params??null);
   if(!params?.speed)assert.equal(await f.page.locator('#action-filter-speed-val').textContent(),await f.page.evaluate(()=>t('inputRepair.defaultParam')));
   await f.page.locator('#action-name').fill('Renamed');await f.page.evaluate(()=>updateAction('fixture'));
   assert.deepEqual(await f.page.evaluate(()=>stored.led.filter_params),params);
   await f.page.evaluate(async()=>{document.getElementById('action-modal')?.remove();await editAction('fixture');});
   await f.page.locator('#action-filter-speed').evaluate(el=>{el.value=25;el.dispatchEvent(new Event('input',{bubbles:true}));});
   await f.page.locator('#action-led-filter').selectOption('scanline');
   await f.page.locator('#action-led-filter').selectOption('wave');
   assert.equal(await f.page.locator('#action-filter-speed').inputValue(),'25');
   await f.page.evaluate(()=>updateAction('fixture'));
   assert.deepEqual(await f.page.evaluate(()=>stored.led.filter_params),{...params,speed:25});
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
}
