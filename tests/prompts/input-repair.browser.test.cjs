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
 await context.addInitScript(language=>{localStorage.setItem('ts_language',language);const random=Math.random;Math.random=()=>{window.formulaRandomCalls=(window.formulaRandomCalls||0)+1;return window.formulaRandomValue??random();};class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;},language);
 const page=await context.newPage(),errors=[];page.setDefaultTimeout(5000);page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
  const u=new URL(route.request().url());if(u.origin!==base){await route.abort();return;}
  if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:u.pathname.endsWith('/ui/widgets/get')?{widgets:[],source:'nvs'}:{}}});return;}
  await route.continue();
 });
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&typeof editAction==='function');
 await page.evaluate(async()=>{
  closeLoginModal();window.writes=[];window.stored=null;window.fixtureRevision=1;
  const configuration=()=>({active_version:fixtureRevision.toString(16).padStart(32,'0'),source:'sd',loaded:true,configured:true,enabled:true,sd_available:true});
  api.call=async(endpoint,params)=>{
   if(endpoint==='ui.widgets.get')return {code:0,data:{widgets:[],source:'nvs'}};
   if(endpoint==='automation.config.status')return {code:0,data:configuration()};
   if(endpoint==='automation.actions.get')return {code:0,data:{...structuredClone(stored),active_version:configuration().active_version}};
   if(['automation.actions.add','automation.actions.update','automation.actions.delete','ui.widgets.set'].includes(endpoint)){
    writes.push({endpoint,params:structuredClone(params)});
    if(window.rejectSave)return {code:1,message:'storage_write_failed',data:{...configuration(),saved:false}};
    if(endpoint.startsWith('automation.actions.')){stored=structuredClone(params);++fixtureRevision;return {code:0,data:{...configuration(),saved:true}};}
    return {code:0,data:{nvs_saved:true}};
   }
   return {code:0,data:{}};
  };
  api.ledList=async()=>({code:0,data:{devices:[{name:'matrix',count:256},{name:'board',count:28}]}});
  await loadDataWidgets();
 });
 return {page,context,errors};
}
for(const language of ['zh-CN','en-US']){
 test(`${language}: stopped initialized protection displays disabled and can enable again`,async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(async()=>{
    window.protectionRunning=false;window.protectionWrites=[];
    api.powerProtectionStatus=async()=>({code:0,data:{initialized:true,running:protectionRunning}});
    api.powerProtectionSet=async params=>{protectionWrites.push(params.enable);protectionRunning=params.enable;return {code:0,data:{initialized:true,running:protectionRunning}};};
    await loadSystemPage();await refreshSystemPageOnce();
   });
   const toggle=f.page.locator('#protection-toggle');
   assert.equal(new URL(f.page.url()).origin,base);assert.match(await f.page.title(),/TianShanOS/i);
   assert.equal(await f.page.locator('.page-system').count(),1);
   assert.equal(await toggle.getAttribute('aria-checked'),'false');
   assert.equal(await f.page.locator('#protection-status').innerText(),language==='zh-CN'?'已禁用':'Disabled');
   if(process.env.TS_TEST_EVIDENCE_DIR){await toggle.scrollIntoViewIfNeeded();await f.page.screenshot({path:path.join(process.env.TS_TEST_EVIDENCE_DIR,`a06-${language}-disabled.png`)});}
   await toggle.click();await f.page.waitForFunction(()=>protectionWrites.length===1);
   assert.equal(await toggle.getAttribute('aria-checked'),'true');
   await toggle.click();await f.page.waitForFunction(()=>protectionWrites.length===2);
   assert.equal(await toggle.getAttribute('aria-checked'),'false');
   await toggle.click();await f.page.waitForFunction(()=>protectionWrites.length===3);
   assert.deepEqual(await f.page.evaluate(()=>protectionWrites),[true,false,true]);
   if(process.env.TS_TEST_EVIDENCE_DIR)await f.page.screenshot({path:path.join(process.env.TS_TEST_EVIDENCE_DIR,`a06-${language}-enabled.png`)});
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
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
 test(`${language}: modal manual filters convert frequency and retain zero intensity, decay and density`,async()=>{
  const f=await fixture(language);try{
   const requests=await f.page.evaluate(async()=>{
    document.body.insertAdjacentHTML('beforeend','<input id="modal-filter-frequency" value="100"><input id="modal-filter-intensity" value="0"><input id="modal-filter-decay" value="0"><input id="modal-filter-density" value="0"><input id="modal-filter-speed" value="5">');
    const calls=[];api.call=async(name,params)=>{if(name==='led.filter.start')calls.push(params);return {code:0,data:{}};};
    selectedModalFilter='glitch';await applyFilterFromModal();
    selectedModalFilter='sparkle';await applyFilterFromModal();return calls;
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

for(const language of ['zh-CN','en-US'])test(`${language}: late font metadata preserves the saved font and the user's newer selection`,async()=>{
 const f=await fixture(language);try{
  await f.page.evaluate(async()=>{
   window.fontReads=0;
   api.storageList=async()=>{++fontReads;await new Promise(resolve=>window.finishFonts=resolve);return {code:0,data:{entries:[]}};};
   stored={id:'fixture',name:'Text',type:'led',enabled:true,led:{device:'matrix',ctrl_type:'text',text:'Hello',font:'missing-font'}};
   await editAction('fixture');
  });
  assert.equal(await f.page.locator('#action-led-font').inputValue(),'missing-font');
  await f.page.evaluate(()=>finishFonts());
  await f.page.waitForFunction(()=>document.querySelector('#action-led-font option:checked').textContent===configText('fontUnavailable',{name:'missing-font'}));
  await f.page.evaluate(async()=>{document.getElementById('action-modal')?.remove();await editAction('fixture');});
  await f.page.locator('#action-led-font').selectOption('');
  await f.page.evaluate(()=>finishFonts());
  await f.page.waitForFunction(()=>document.getElementById('action-led-font').options.length===1);
  assert.equal(await f.page.locator('#action-led-font').inputValue(),'');
  assert.equal(await f.page.evaluate(()=>fontReads),2);
  assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});

for (const lang of ['zh-CN', 'en-US']) {
    test(`${lang}: power telemetry valid UART, zero readings, invalid UART and independent ADC`, async () => {
        const f=await fixture(lang);
        try { const {page}=f;
            await page.evaluate(() => loadSystemPage());
            const readings = await page.evaluate(() => {
                const snapshot = data => {
                    updatePowerInfo(data);
                    return ['voltage', 'current', 'power-watts', 'internal-voltage'].map(id => document.getElementById(id).textContent);
                };
                return {
                    valid: snapshot({power_chip:{valid:true,crc_valid:true,voltage_v:24,current_a:2,power_w:48},voltage:{supply_v:13.6}}),
                    zero: snapshot({power_chip:{valid:true,crc_valid:true,voltage_v:24,current_a:0,power_w:0},current:{value_a:99},power:{value_w:99},voltage:{supply_v:13.6}}),
                    crcBad: snapshot({power_chip:{valid:false,crc_valid:false,voltage_v:75,current_a:19,power_w:1425},current:{value_a:99},power:{value_w:99},voltage:{supply_v:13.6}}),
                    crcOnly: snapshot({power_chip:{valid:true,crc_valid:false,voltage_v:75,current_a:19,power_w:1425},voltage:{supply_v:13.6}}),
                    invalid: snapshot({power_chip:{valid:false,crc_valid:true,voltage_v:75,current_a:19,power_w:1425},voltage:{supply_v:13.6}}),
                    noChip: snapshot({current:{value_a:2},power:{value_w:48},voltage:{supply_v:13.6}})
                };
            });
            // Assert the validity defect first; keep zero/fallback expectations alongside it.
            assert.deepEqual(readings.crcBad, ['-', '-', '-', '12.60 V']);
            assert.deepEqual(readings.crcOnly, ['-', '-', '-', '12.60 V']);
            assert.deepEqual(readings.invalid, ['-', '-', '-', '12.60 V']);
            assert.deepEqual(readings.valid, ['24.0 V', '2.00 A', '48.0 W', '12.60 V']);
            assert.deepEqual(readings.zero, ['24.0 V', '0.00 A', '0.0 W', '12.60 V']);
            assert.deepEqual(readings.noChip, ['-', '2.00 A', '48.0 W', '12.60 V']);
            assert.deepEqual(f.errors, []);
        } finally { await f.context.close(); }
    });
}

async function prepareFanCurveFixture(page, failedEndpoint = null) {
    await page.evaluate(async failed => {
        window.fanWrites = []; window.fanMode = 'auto';
        window.savedFan = {curve:[{temp:30,duty:30},{temp:50,duty:60},{temp:70,duty:100}],hysteresis:3,min_interval:2000,min_duty:20,max_duty:100};
        const original = api.call;
        api.call = async (endpoint, params) => {
            if (endpoint === 'fan.config') return {code:0,data:structuredClone(savedFan)};
            if (['fan.limits','fan.curve','fan.mode'].includes(endpoint)) {
                fanWrites.push({endpoint,params:structuredClone(params)});
                if (endpoint === 'fan.mode') fanMode = params.mode;
                else Object.assign(savedFan, structuredClone(params));
                if (endpoint === failed) return {code:9,message:'fan_config_save_failed',data:{applied:true,persisted:false}};
                return {code:0,data:{...params,applied:true,persisted:true}};
            }
            return original(endpoint, params);
        };
        await showFanCurveModal(0);
    }, failedEndpoint);
}
for (const language of ['zh-CN', 'en-US']) {
    test(`${language}: fan curve row identity, deletion and successful mode switch`, async () => {
        const f = await fixture(language);
        try {
            const {page} = f; await prepareFanCurveFixture(page);
            const rows = page.locator('#fan-curve-points .row');
            await rows.nth(0).locator('input').nth(0).fill('35');
            await rows.nth(0).locator('input').nth(0).press('Tab');
            await page.evaluate(() => closeFanCurveModal());
            assert.deepEqual(await page.evaluate(() => [fanWrites.length,fanMode]), [0,'auto']);
            await page.evaluate(() => showFanCurveModal(0));
            await rows.nth(0).locator('input').nth(0).fill('60');
            await rows.nth(0).locator('input').nth(0).press('Tab');
            await rows.nth(0).locator('input').nth(1).fill('80');
            await rows.nth(0).locator('input').nth(1).press('Tab');
            assert.deepEqual(await page.evaluate(() => fanCurveConfig.curve), [{temp:60,duty:80},{temp:50,duty:60},{temp:70,duty:100}]);
            await page.evaluate(() => applyFanCurve());
            assert.equal(await page.locator('#fan-curve-modal').count(), 0);
            assert.equal(await page.evaluate(() => fanMode), 'curve');
            const first = await page.evaluate(() => fanWrites);
            assert.deepEqual(first.map(write => write.endpoint), ['fan.limits','fan.curve','fan.mode']);
            assert.deepEqual(first[1].params.curve, [{temp:50,duty:60},{temp:60,duty:80},{temp:70,duty:100}]);
            await page.evaluate(() => showFanCurveModal(0));
            await rows.nth(1).locator('button').click();
            assert.equal(await rows.count(), 2);
            await rows.nth(0).locator('input').nth(1).fill('65');
            await rows.nth(0).locator('input').nth(1).press('Tab');
            assert.deepEqual(await page.evaluate(() => fanCurveConfig.curve), [{temp:50,duty:65},{temp:70,duty:100}]);
            await page.evaluate(() => applyFanCurve());
            assert.deepEqual(await page.evaluate(() => savedFan.curve), [{temp:50,duty:65},{temp:70,duty:100}]);
            assert.equal(await page.evaluate(() => fanMode), 'curve');
            assert.deepEqual(f.errors, []);
        } finally { await f.context.close(); }
    });
    for (const failed of ['fan.limits','fan.curve','fan.mode']) {
        test(`${language}: fan curve ${failed} persistence failure remains applied and reports failure`, async () => {
            const f = await fixture(language);
            try {
                await prepareFanCurveFixture(f.page, failed);
                await f.page.evaluate(() => applyFanCurve());
                assert.equal(await f.page.locator('#fan-curve-modal').count(), 1);
                assert.equal(await f.page.locator('#toast.toast-error.show').count(), 1);
                const why = await f.page.evaluate(() => t('failure.reasons.fan_config_save_failed.why'));
                assert((await f.page.locator('#toast .toast-msg').innerText()).includes(why));
                const writes = await f.page.evaluate(() => fanWrites.map(write => write.endpoint));
                assert.equal(writes.at(-1), failed);
                assert.equal(await f.page.evaluate(() => fanMode), failed === 'fan.mode' ? 'curve' : 'auto');
                assert.deepEqual(f.errors, []);
            } finally { await f.context.close(); }
        });
    }
}

const formulaContract=JSON.parse(fs.readFileSync(path.join(__dirname,'../../docs/repair/whole-product-20261031/review-evidence/v3-formula-contract-cases.json'),'utf8'));
test('A03: actual formula evaluator retains all 24 approved math/text cases and the fixed native function contract',async()=>{
 const f=await fixture();try{
  for(const c of formulaContract.positive_cases){
   assert.deepEqual(await f.page.evaluate(c=>evaluateExpression(c.expression,c.variables),c),c.expected,c.id);
  }
  const cases=[
   ['1 + 2 * 3 ** 2',19],['(1 + 2) * 3',9],['false ? 1 : true ? 2 : 3',2],
   ['1 | 2 ^ 3 & 1',3],['128 >>> 2',32],['1 << 3 + 1',16],['~1',-2],
   ['1 == "1"',true],['1 !== "1"',true],['2 <= 2 && 3 > 2',true],
   ['0x10 + 0b10 + 0o10 + .5e1',31],['0.5 + 2.5e-1',.75],
   ['null ?? 4',4],['(null ?? false) || 3',3],['null ?? (false || 3)',3],
   ['!false && +2 + -3 === -1',true],
   ['"中文\\n\\t\\x41\\u4e2d\\u{1f600}"','中文\n\tA中😀'],
   ["'a\\'b\\\\c'", "a'b\\c"],
   [String.fromCharCode(96)+'中文\n$'+'{temp}'+String.fromCharCode(96),'中文\n42.36'],
   [String.fromCharCode(96)+'a\r\nb'+String.fromCharCode(96),'a\nb'],
   ['parseInt("ff", 16)',255],['Boolean(0)',false],['String(12) + Number("3")','123'],
   ['isNaN("no")',true],['isFinite("3")',true],
   ['(123.45).toPrecision(4)','123.5'],['(2).toFixed(100)',(2).toFixed(100)],
   ['(2).toExponential(100)',(2).toExponential(100)],['(2).toPrecision(100)',(2).toPrecision(100)],
   ['(2).toFixed()','2'],['Math.max(1, 3,)',3],
   ['Math.max()',-Infinity],['Infinity / 2',Infinity]
  ];
  for(const [expression,expected]of cases){
   assert.deepEqual(await f.page.evaluate(expression=>evaluateExpression(expression,{temp:42.36}),expression),expected,expression);
  }
  const mathArgs={abs:[-2],acos:[.5],acosh:[2],asin:[.5],asinh:[2],atan:[2],atan2:[2,3],atanh:[.5],cbrt:[-8],
   ceil:[1.2],clz32:[1],cos:[1],cosh:[1],exp:[1],expm1:[1],floor:[1.8],fround:[1.2345],hypot:[3,4],imul:[3,4],
   log:[2],log10:[100],log1p:[1],log2:[8],max:[-1,2,3],min:[-1,2,3],pow:[2,3],round:[1.8],sign:[-2],
   sin:[1],sinh:[1],sqrt:[4],tan:[1],tanh:[1],trunc:[-1.8]};
  for(const [name,args]of Object.entries(mathArgs)){
   const expression='Math.'+name+'('+args.join(',')+')';
   const pair=await f.page.evaluate(({expression,name,args})=>[evaluateExpression(expression,{}),Math[name](...args)],{expression,name,args});
   assert.equal(pair[0],pair[1],name);
  }
  for(const name of 'E LN10 LN2 LOG10E LOG2E PI SQRT1_2 SQRT2'.split(' ')){
   assert.equal(await f.page.evaluate(name=>evaluateExpression('Math.'+name,{}),name),Math[name],name);
  }
  assert.equal(await f.page.evaluate(()=>Number.isNaN(evaluateExpression('0 / 0',{}))),true);
  assert.equal(await f.page.evaluate(()=>Number.isNaN(evaluateExpression('NaN',{}))),true);
  assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});

test('A03: data bindings stay values, all structure is checked before native calls, and selected branches alone execute',async()=>{
 const f=await fixture();try{
  const result=await f.page.evaluate(blocked=>{
   const ref=name=>'$'+'{'+name+'}';
   const text='中文 "quote" \\ slash\n<em>plain</em> $'+'{nested} null';
   const variables={name:text,temp:12,nil:null};
   const own=Object.assign(Object.create({inherited:9}),{constructor:7,'a.b 中文':8});
   const data=[evaluateExpression(ref('name'),variables),evaluateExpression('"'+ref('name')+'"',variables),
    evaluateExpression(ref('name')+' + " end"',variables),evaluateExpression('"'+ref('name')+' '+ref('temp')+'"',variables),
    evaluateExpression(ref(' a.b 中文 '),own),evaluateExpression(ref('constructor')+' + 1',own),
    evaluateExpression(ref('inherited'),own),evaluateExpression(ref('nil'),variables),
    evaluateExpression(ref('missing')+' + 1',variables),evaluateExpression('true ? 1 : '+ref('nil'),variables),
    evaluateExpression('"null '+ref('temp')+'"',variables)];
   window.formulaRandomValue=.25;window.formulaRandomCalls=0;
   const lazy=['true ? 4 : Math.random()','false && Math.random()','true || Math.random()','1 ?? Math.random()',
    'true ? 4 : (0).toFixed(101)'].map(expression=>evaluateExpression(expression,{}));
   const before=formulaRandomCalls;
   const invalid=blocked.concat(['-2 ** 2','null ?? true || false','true && null ?? 1','++2','--2',
    'Math.random() +','true ? 1 : Math.noSuch(2)','Math.random() + true.noSuch()',
    'Math.random() + 1 2','"unfinished','1e','0x','1;','(1,2)','[1]','"a".toFixed(1)'])
    .map(expression=>evaluateExpression(expression,{}));
   const after=formulaRandomCalls;
   const called=evaluateExpression('Math.random() + (false ? Math.random() : 1)',{});
   return {text,data,lazy,before,invalid,after,called,calls:formulaRandomCalls};
  },formulaContract.blocked_expressions_not_executed_on_baseline);
  assert.deepEqual(result.data,[result.text,result.text,result.text+' end',result.text+' 12',8,8,undefined,null,null,null,'null 12']);
  assert.deepEqual(result.lazy,[4,false,true,1,4]);assert.equal(result.before,0);
  assert(result.invalid.every(value=>value===null));assert.equal(result.after,0);
  assert.equal(result.called,1.25);assert.equal(result.calls,1);assert.deepEqual(f.errors,[]);
 }finally{await f.context.close();}
});

for(const language of ['zh-CN','en-US']){
 test(language+': A03 Admin widget edit preserves the existing preview, then saves and refreshes each original formula',async()=>{
  const f=await fixture(language);try{
   await f.page.evaluate(async()=>{
    api.user={username:'admin',role:'admin'};window.widgetSaved={widgets:[],source:'nvs'};
    const original=api.call;
    api.call=async(endpoint,params)=>{
     if(endpoint==='ui.widgets.set'){widgetSaved=structuredClone(params);writes.push({endpoint,params:structuredClone(params)});return {code:0,data:{nvs_saved:true}};}
     if(endpoint==='ui.widgets.get')return {code:0,data:{...structuredClone(widgetSaved),source:'nvs'}};
     if(endpoint==='automation.variables.list')return {code:0,data:{variables:[
      {name:'temp',value:42.36},{name:'used',value:0},{name:'name',value:'GPU'},{name:'enabled',value:true}]}};
     return original(endpoint,params);
    };
    await loadSystemPage();
   });
   for(const c of formulaContract.positive_cases.slice(0,11)){
    await f.page.evaluate(()=>{
     stopDataWidgetsAutoRefresh();
     dataWidgets=[{id:'formula',type:'text',label:'Formula',icon:'ri-dashboard-line',layout:'large',unit:'°C',decimals:1,expression:'$'+'{temp}'}];
     renderDataWidgets();showWidgetEditPanel('formula');
    });
    assert.equal(await f.page.locator('#dw-preview-card .dw-card').count(),1);
    assert.equal(await f.page.locator('#dw-preview-card svg.i').count()>0,true);
    await f.page.locator('#edit-expression').fill(c.expression);
    await f.page.locator('#widget-edit-modal .sf .primary').click();
    await f.page.waitForFunction(()=>!document.getElementById('widget-edit-modal'));
    await f.page.waitForFunction(expected=>document.getElementById('dw-formula-value')?.textContent===String(expected),c.expected);
    assert.equal(await f.page.evaluate(()=>widgetSaved.widgets[0].expression),c.expression,c.id);
    await f.page.evaluate(async()=>{dataWidgets=[];await loadDataWidgets();renderDataWidgets();await refreshDataWidgets();});
    assert.equal(await f.page.locator('#dw-formula-value').textContent(),String(c.expected),c.id);
    assert.equal(await f.page.locator('#data-widgets-grid [data-widget-id="formula"]').getAttribute('data-layout'),'large');
    assert.equal(await f.page.evaluate(()=>dataWidgets[0].unit),'°C');
   }
   assert.equal(await f.page.evaluate(()=>writes.filter(w=>w.endpoint==='ui.widgets.set').length),11);
   await f.page.evaluate(async()=>{
    const original=api.call;
    api.call=async(endpoint,params)=>endpoint==='automation.variables.list'?{code:0,data:{variables:[
     {name:'__proto__',value:2},{name:'constructor',value:3},{name:'a.b 中文',value:4},{name:'external',value:'中文 "quote" \\ slash\n<em>plain</em> & null'}]}}:original(endpoint,params);
    dataWidgets[0].expression='$'+'{__proto__} + $'+'{constructor} + $'+'{a.b 中文}';
    await refreshDataWidgets();
   });
   assert.equal(await f.page.locator('#dw-formula-value').textContent(),'9');
   await f.page.evaluate(async()=>{
    dataWidgets[0].expression='"$'+'{external}"';await refreshDataWidgets();
   });
   assert.equal(await f.page.locator('#dw-formula-value').textContent(),'中文 "quote" \\ slash\n<em>plain</em> & null');
   assert.equal(await f.page.locator('#dw-formula-value em').count(),0);
   await f.page.evaluate(async()=>{dataWidgets[0].expression='$'+'{missing} + 1';await refreshDataWidgets();});
   assert.equal(await f.page.locator('#dw-formula-value').textContent(),'-');

   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
}
