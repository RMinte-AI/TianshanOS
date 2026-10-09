const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let server,browser,base;
before(async()=>{
 server=http.createServer((req,res)=>{
  const rel=decodeURIComponent(new URL(req.url,'http://local').pathname),file=path.resolve(root,'.'+(rel==='/'?'/index.html':rel));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}const ext=path.extname(file);res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css'})[ext]||'application/octet-stream');res.end(data);});
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
for (const language of ['zh-CN', 'en-US']) {
 test(`${language}: new confirmation sheet cancels writes and guards dangerous keyboard actions`, async()=>{
  const {page,context,errors}=await pageFor(language);
  try {
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(()=>{
    closeLoginModal();window.deleteCalls=0;
    api.storageDelete=async()=>{deleteCalls++;return {code:0};};
    refreshFilesPage=async()=>{};
    window.pendingDelete=deleteFile('/sdcard/fixture.txt');
   });
   const sheet=page.locator('.confirm-sheet');
   await sheet.waitFor();
   assert.equal(await page.evaluate(()=>document.activeElement.dataset.r),'0');
   await page.evaluate(()=>document.querySelector('.confirm-sheet').dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true})));
   assert.equal(await page.evaluate(()=>deleteCalls),0);
   assert.equal(await sheet.count(),1);
   await page.keyboard.press('Escape');await page.evaluate(()=>pendingDelete);
   assert.equal(await page.evaluate(()=>deleteCalls),0);
   assert.equal(await sheet.count(),0);
   await page.evaluate(()=>{window.pendingDelete=deleteFile('/sdcard/fixture.txt');});
   await sheet.locator('button[data-r="1"]').click();await page.evaluate(()=>pendingDelete);
   assert.equal(await page.evaluate(()=>deleteCalls),1);
   assert.equal(await sheet.count(),0);
   assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}
after(async()=>{await browser?.close();await new Promise(resolve=>server?.close(resolve));});
async function pageFor(language='en-US',handler){
 const context=await browser.newContext({locale:language,viewport:{width:1280,height:900}});
 await context.addInitScript(({language})=>{
   localStorage.setItem('ts_language',language);
   class Socket {static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}
   window.WebSocket=Socket;
 },{language});
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
   const u=new URL(route.request().url());
   if(u.origin!==base){await route.abort();return;}
   if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{}}});return;}
   if(handler&&await handler(route,u))return;
   await route.continue();
 });
 return {page,context,errors};
}
for(const language of ['zh-CN','en-US']) {
 test(`${language}: deploy button uses first trust directly, renders registered host and rejects changed keys`,async()=>{
  const {page,context,errors}=await pageFor(language);
  const copies=[];let deployed=false,changed=false;
  const host={id:'fixture@192.0.2.99',host:'192.0.2.99',port:22,username:'fixture',keyid:'fixture-key'};
  await page.route('**/api/v1/**',async route=>{
   const req=route.request(),endpoint=new URL(req.url()).pathname.slice('/api/v1/'.length);
   let code=0,data={};
   if(endpoint==='ssh/hosts/list')data={hosts:deployed?[host]:[]};
   if(endpoint==='hosts/list')data={hosts:[]};
   if(endpoint==='key/list')data={keys:[{id:'fixture-key',type:'rsa2048',bits:2048}]};
   if(endpoint==='ssh/copyid'){
    const params=req.postDataJSON();copies.push({method:req.method(),params});
    if(changed){code=1001;data={status:'mismatch',stored_fingerprint:'old',current_fingerprint:'changed'};}
    else if(params.trust_new!==true){code=1002;data={status:'new_host',fingerprint:'first'};}
    else{deployed=true;data={deployed:true,verified:true,registered:true,host_id:host.id};}
   }
   await route.fulfill({json:{code,data,message:code===1001?'Host key mismatch - possible MITM attack':code===1002?'New host requires confirmation':''}});
  });
  try {
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{closeLoginModal();await loadSecurityPage();showDeployKeyModal('fixture-key');});
   await page.locator('#deploy-host').fill(host.host);await page.locator('#deploy-user').fill(host.username);
   await page.locator('#deploy-password').fill('fixture-password');
   await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('success'));
   assert.equal(copies.length,1);assert.equal(copies[0].method,'POST');
   assert.deepEqual(copies[0].params,{host:host.host,user:host.username,password:'fixture-password',keyid:host.keyid,port:22,verify:true,trust_new:true,accept_changed:false});
   assert.equal(await page.locator('.confirm-sheet').count(),0);
   assert((await page.locator('#ssh-hosts-table-body').textContent()).includes(host.id));
   assert.equal(await page.locator('#deploy-btn').isEnabled(),true);
   // Reusing the stored fingerprint remains a one-request operation.
   await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>!document.getElementById('deploy-btn').disabled);
   assert.equal(copies.length,2);
   changed=true;await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('error'));
   assert.equal(copies.length,3);assert.equal(copies[2].params.accept_changed,false);
   assert.equal(await page.locator('.confirm-sheet').count(),0);
   assert((await page.locator('#deploy-result').textContent()).includes('Host key mismatch'));
   assert((await page.locator('#ssh-hosts-table-body').textContent()).includes(host.id));
   assert.deepEqual(errors,[]);
  } finally {await context.close();}
 });
}
for(const language of ['zh-CN','en-US']){
 test(`${language}: dashboard verifies unknown service with JSON before triggering`,async()=>{
  const {page,context,errors}=await pageFor(language);
  const calls=[];let verifyOperation=0,verifiedState='unknown',verifyGate,releaseVerify,verifyEntered,triggerGate,releaseTrigger,triggerEntered;
  const rule={id:'model-rule',name:'Model',enabled:true,manual_trigger:true,allow_manual_trigger:true,show_on_dashboard:true,actions_count:1};
  await page.route('**/api/v1/**',async route=>{
   const request=route.request(),url=new URL(request.url()),endpoint=url.pathname.slice('/api/v1/'.length);
   const params=request.method()==='GET'?Object.fromEntries(url.searchParams):request.postDataJSON();
   calls.push({endpoint,method:request.method(),params});
   let data={};
   if(endpoint==='ssh/hosts/list')data={hosts:[{id:'host',host:'192.0.2.1',username:'fixture'}]};
   if(endpoint==='ssh/commands/list')data={commands:[{id:'model-command',host_id:'host',name:'Model',command:'true',nohup:true,serviceMode:true}]};
   if(endpoint==='automation/rules/list')data={loaded:true,rules:[rule]};
   if(endpoint==='automation/rules/get')data={actions:[{template_id:'model-action'}]};
   if(endpoint==='automation/actions/get')data={type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model-command'}};
   // Like the device, a URL string "true" is not a JSON boolean true.
   if(endpoint==='automation/services/status'){
    if(params?.verify===true){verifyOperation++;verifyEntered?.();data={operation_id:verifyOperation};}
    else {
     if(verifyOperation&&verifyGate)await verifyGate;
     data={state:verifyOperation?verifiedState:'unknown',operation_id:verifyOperation,operation_phase:'succeeded',operation_kind:'verify'};
    }
   }
   if(endpoint==='automation/rules/trigger'&&triggerGate){triggerEntered();await triggerGate;}
   await route.fulfill({json:{code:0,data}});
  });
  try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{closeLoginModal();await loadSystemPage();await refreshQuickActions();stopServiceStatusRefresh();if(quickActionsTimeoutId){clearTimeout(quickActionsTimeoutId);quickActionsTimeoutId=null;}});
   const card=page.locator('#quick-action-model-rule');await card.waitFor();
   assert.equal(await card.getAttribute('data-allowed'),'true');
   await page.evaluate(()=>updateQuickActionServiceStatus());
   assert(calls.filter(c=>c.endpoint==='automation/services/status').every(c=>c.method==='GET'&&!c.params.verify));
   for(const state of ['unknown','running','stopped']){
    verifiedState=state;
    await card.evaluate(el=>{el.dataset.state='unknown';});
    verifyGate=new Promise(resolve=>releaseVerify=resolve);
    const verifying=new Promise(resolve=>verifyEntered=resolve);
    triggerGate=new Promise(resolve=>releaseTrigger=resolve);
    const triggering=new Promise(resolve=>triggerEntered=resolve);
    const beforeVerify=calls.filter(c=>c.endpoint==='automation/services/status'&&c.params.verify).length;
    await card.locator('.quick-action-name').click();await verifying;
    assert.equal(await card.evaluate(el=>el.classList.contains('triggering')&&el.style.pointerEvents==='none'),true);
    // Exercise the handler guard too, bypassing CSS pointer-event suppression.
    await page.evaluate(()=>triggerQuickAction('model-rule'));
    assert.equal(calls.filter(c=>c.endpoint==='automation/services/status'&&c.params.verify).length,beforeVerify+1);
    assert.equal(await page.locator('#toast').textContent(),await page.evaluate(()=>t('toast.processing')));
    releaseVerify();
    if(state==='stopped'){
     await triggering;
     await page.evaluate(()=>triggerQuickAction('model-rule'));
     assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,1);
     assert.equal(await page.locator('#toast').textContent(),await page.evaluate(()=>t('toast.processing')));
    }
    releaseTrigger();
    await page.waitForFunction(()=>!document.getElementById('quick-action-model-rule').classList.contains('triggering'));
    assert.equal(await card.evaluate(el=>el.style.pointerEvents),'');
    assert.equal(await card.getAttribute('data-state'),state==='stopped'?'starting':state);
    if(state!=='stopped'){
     await page.waitForFunction(()=>document.getElementById('toast')?.textContent===runtimeText('startBlocked'));
     assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
     assert.equal(await page.locator('#toast').textContent(),await page.evaluate(()=>runtimeText('startBlocked')));
    }
   }
   const verify=calls.filter(c=>c.endpoint==='automation/services/status'&&c.params.verify);
   assert.equal(verify.length,3);assert(verify.every(c=>c.method==='POST'&&c.params.verify===true&&c.params.command_id==='model-command'));
   await page.waitForFunction(()=>document.getElementById('quick-action-model-rule').classList.contains('is-running'));
   const triggers=calls.filter(c=>c.endpoint==='automation/rules/trigger');assert.equal(triggers.length,1);assert.equal(triggers[0].params.id,'model-rule');
   assert.deepEqual(errors,[]);
  }finally{releaseVerify?.();releaseTrigger?.();await context.close();}
 });
 test(`${language}: real index cold start, safe toast/file text, current websocket state`,async()=>{
   const {page,context,errors}=await pageFor(language);try{
    const fetched=[];page.on('request',r=>{if(r.url().includes('/js/lang/'))fetched.push(r.url());});
    await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
    assert.equal(await page.evaluate(()=>getLanguage()),language);assert.equal(fetched.length,1);
    assert.equal(await page.evaluate(()=>document.getElementById('app').inert),false);
    assert(!await page.locator('#login-modal').innerText().then(t=>t.includes('login.')));
    await page.evaluate(()=>{renderWsStatus(false);showToast('<img src=x onerror="window.injected=true">\nSecond line','error',10000);});
    assert.equal(await page.locator('#toast img').count(),0);assert((await page.locator('#toast').textContent()).includes('<img'));
    assert.equal(await page.evaluate(()=>window.injected),undefined);
    assert.equal(await page.locator('#ws-status').getAttribute('title'),await page.evaluate(()=>t('network.disconnected')));
    await page.evaluate(()=>{const list=document.createElement('div');list.id='upload-list';document.body.append(list);filesToUpload=[];handleFileSelect({target:{files:[{name:'<img src=x onerror="window.injected=true">',size:1}]}});});
    assert.equal(await page.locator('#upload-list img').count(),0);
    assert.deepEqual(errors,[]);
    fs.mkdirSync('output/playwright',{recursive:true});await page.screenshot({path:`output/playwright/prompt-repair-${language}.png`});
   }finally{await context.close();}
 });
 test(`${language}: initial language failure blocks controls and retry recovers`,async()=>{
   let fail=true;const {page,context,errors}=await pageFor(language,async(route,u)=>{if(u.pathname.includes('/js/lang/')&&fail){await route.abort();return true;}return false;});
   try{await page.goto(base);await page.waitForSelector('#language-status button');
    assert.equal(await page.evaluate(()=>i18n.isReady()),false);assert.equal(await page.evaluate(()=>document.getElementById('app').inert),true);
    assert.equal(await page.evaluate(()=>{let n=0;window.confirm=()=>{n++;return true;};confirmAction(t('ui.confirmRollback'));return n;}),0);
    fail=false;await page.locator('#language-status button').click();await page.waitForFunction(()=>i18n.isReady());
    assert.equal(await page.evaluate(()=>document.getElementById('app').inert),false);assert.deepEqual(errors,[]);
   }finally{await context.close();}
 });
}
test('delayed language package and rapid switches keep the latest requested language',async()=>{
 let release,hold=true;const gate=new Promise(r=>release=r);
 const {page,context}=await pageFor('en-US',async(route,u)=>{if(u.pathname.endsWith('/zh-CN.js')&&hold){await gate;await route.continue();return true;}return false;});
 try{await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  await page.evaluate(()=>{window.firstSwitch=selectLanguage('zh-CN');});
  await page.evaluate(()=>selectLanguage('en-US'));release();hold=false;
  await page.evaluate(()=>window.firstSwitch);assert.equal(await page.evaluate(()=>getLanguage()),'en-US');
  await page.evaluate(()=>selectLanguage('zh-CN'));assert.equal(await page.evaluate(()=>getLanguage()),'zh-CN');
  assert.equal(await page.locator('#ws-status').getAttribute('aria-label'),await page.evaluate(()=>t('network.disconnected')));
 }finally{release();await context.close();}
});
test('terminal resource failure presents retry; a rejected cached promise is not reused',async()=>{
 const {page,context,errors}=await pageFor('en-US',async(route,u)=>{if(u.pathname.startsWith('/vendor/')){await route.abort();return true;}return false;});let attempts=0;
 try{await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  page.on('request',r=>{if(r.url().includes('xterm.css'))attempts++;});
  await page.evaluate(()=>{closeLoginModal();return loadTerminalPage();});
  assert((await page.locator('#terminal-container').textContent()).includes('could not be loaded'));
  await page.locator('#terminal-container button').click();await page.waitForFunction(()=>document.querySelector('#terminal-container button'));
  assert.equal(attempts,2);assert.deepEqual(errors,[]);
 }finally{await context.close();}
});
for(const language of ['zh-CN','en-US']) {
 test(`${language}: package confirmation treats names as text and applies only on explicit verified action`,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const path="/sdcard/a');window.injected=true;//<img>.tscfg";
   await page.evaluate(path=>{closeLoginModal();window.fixturePath=path;window.calls=[];api.call=async(name,args)=>{calls.push({name,args});return {code:0,data:{success:false,result_message:'<img src=x onerror=alert(1)>'}};};showConfigPackApplyConfirm(path,{valid:false});},path);
   assert.equal(await page.locator('#config-pack-apply-confirm').count(),0);
   await page.evaluate(()=>showConfigPackApplyConfirm(fixturePath,{valid:true,signature:{signer_cn:'<img src=x onerror="window.injected=true">'}}));
   assert.equal(await page.locator('#config-pack-apply-confirm img').count(),0);
   await page.locator('#config-pack-apply-button').click();
   await page.waitForFunction(()=>calls.length===1&&!document.getElementById('config-pack-apply-button').disabled);
   assert.equal(await page.evaluate(()=>calls[0].args.path),path);
   assert.equal(await page.locator('#config-pack-apply-confirm').count(),1);
   assert.equal(await page.locator('#toast img').count(),0);
   await page.evaluate(()=>{toastDeadline=0;api.call=async()=>({code:0,data:{success:true,applied_modules:['fixture']}});});
   await page.locator('#config-pack-apply-button').click();await page.waitForFunction(()=>!document.getElementById('config-pack-apply-confirm'));
   assert.equal(await page.evaluate(()=>window.injected),undefined);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}
test('initial delayed language keeps controls inert until the real package arrives',async()=>{
 let release;const gate=new Promise(r=>release=r);
 const {page,context,errors}=await pageFor('en-US',async(route,u)=>{if(u.pathname.endsWith('/en-US.js')){await gate;await route.continue();return true;}return false;});
 try{await page.goto(base,{waitUntil:'domcontentloaded'});
  assert.equal(await page.evaluate(()=>i18n.isReady()),false);assert.equal(await page.evaluate(()=>document.getElementById('app').inert),true);
  release();await page.waitForFunction(()=>i18n.isReady());assert.deepEqual(errors,[]);
 }finally{release();await context.close();}
});
test('failed language switch preserves the loaded language; retry clears failure and stale toast',async()=>{
 let fail=true;const {page,context,errors}=await pageFor('en-US',async(route,u)=>{if(u.pathname.endsWith('/zh-CN.js')&&fail){await route.abort();return true;}return false;});
 try{await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  await page.evaluate(()=>{showToast('old language','info',10000);return selectLanguage('zh-CN');});
  assert.equal(await page.evaluate(()=>getLanguage()),'en-US');assert.equal(await page.evaluate(()=>document.getElementById('app').inert),false);
  fail=false;await page.locator('#language-status button').click();await page.waitForFunction(()=>getLanguage()==='zh-CN');
  assert.equal(await page.locator('#toast').evaluate(e=>e.classList.contains('show')),false);assert.deepEqual(errors,[]);
 }finally{await context.close();}
});
test('terminal can initialize after failed resource load using newly served resources',async()=>{
 let fail=true;
 const {page,context,errors}=await pageFor('en-US',async(route,u)=>{if(fail&&u.pathname.startsWith('/vendor/')){await route.abort();return true;}return false;});
 try{await page.goto(base);await page.waitForFunction(()=>i18n.isReady());await page.evaluate(()=>{closeLoginModal();return loadTerminalPage();});
  assert.equal(await page.locator('#terminal-container button').count(),1);
  fail=false;await page.locator('#terminal-container button').click();await page.waitForSelector('#terminal-container .xterm');
  assert(await page.evaluate(()=>webTerminal.terminal instanceof Terminal&&!!webTerminal.fitAddon));
  assert.equal(await page.locator('#terminal-container button').count(),0);assert.deepEqual(errors,[]);
 }finally{await context.close();}
});

for(const language of ['zh-CN','en-US']) test(`${language}: managed commands share service state, start, verification and stop`,async()=>{
 const {page,context,errors}=await pageFor(language);
 const calls=[];let state='unknown',phase='',operationId=41,release,entered,statusGate,statusEntered,releaseStatus;
 const gate=new Promise(r=>release=r),started=new Promise(r=>entered=r);
 const model={id:'model',host_id:'host',name:'Model',command:'printf model',nohup:true,serviceMode:true,readyPattern:'ready',varName:'model'};
 await page.route('**/api/v1/**',async route=>{
  const req=route.request(),endpoint=new URL(req.url()).pathname.slice('/api/v1/'.length);
  const params=req.method()==='GET'?Object.fromEntries(new URL(req.url()).searchParams):req.postDataJSON();calls.push({endpoint,params,method:req.method()});
  let data={};
  if(endpoint==='ssh/hosts/list')data={hosts:[{id:'host',host:'192.0.2.1',username:'fixture',port:22}]};
  if(endpoint==='ssh/commands/list')data={commands:[model,{id:'plain',host_id:'host',name:'Plain',command:'printf plain'}]};
  if(endpoint==='automation/services/status'){
   data={state,source:'log',confirmed_ms:1000,operation_id:phase?operationId:0,operation_phase:phase,operation_error:phase==='failed'?259:0};
   if(params.verify){operationId++;phase='succeeded';data={operation_id:operationId};}
   if(statusGate&&!params.verify){const held=statusGate;statusGate=null;statusEntered();await held;}
  }
  if(endpoint==='ssh/services/start'){entered();await gate;phase='queued';data={operation_id:operationId};}
  if(endpoint==='automation/services/stop'){state='stopped';operationId++;phase='succeeded';data={operation_id:operationId};}
  if(endpoint==='ssh/exec_stream')data={session_id:42};
  await route.fulfill({json:{code:0,data}});
 });
 try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  await page.evaluate(async()=>{closeLoginModal();await loadCommandsPage();selectHost('host');stopServiceStatusRefresh();window.confirmAction=()=>true;});
  await page.waitForFunction(()=>!serviceStatusInFlight);
  await page.evaluate(()=>{window.launching=executeCommand(0);});await started;
  await page.evaluate(()=>executeCommand(0));
  assert.equal(calls.filter(c=>c.endpoint==='ssh/services/start').length,1);
  assert.equal(calls.filter(c=>c.endpoint==='ssh/exec_stream').length,0);
  assert.deepEqual(calls.find(c=>c.endpoint==='ssh/services/start').params,{command_id:'model'});
  release();await page.evaluate(()=>window.launching);
  assert.match(await page.locator('#service-status-model').getAttribute('class'),/status-starting/);
  assert((await page.locator('#exec-result').textContent()).includes(await page.evaluate(()=>runtimeText('launch_accepted'))));
  phase='executing';await page.evaluate(()=>updateQuickActionServiceStatus());
  assert((await page.locator('#exec-result').textContent()).includes(await page.evaluate(()=>runtimeText('launch_executing'))));
  phase='succeeded';
  const colors={};
  // Same response drives the real command label and dashboard indicator.
  await page.evaluate(()=>{const card=document.createElement('div');card.className='quick-action-card';card.innerHTML='<div class="quick-action-service-status" data-command="model"><span class="service-value"></span></div>';document.body.append(card);});
  for(const next of ['ready','unknown','running','failed','timeout','stopped']){
   state=next;
   await page.evaluate(async()=>{stopServiceStatusRefresh();await updateQuickActionServiceStatus();});
   const command=page.locator('#service-status-model'),dashboard=page.locator('.quick-action-service-status');
   assert.match(await command.getAttribute('class'),new RegExp('status-'+next));
   assert.equal(await command.textContent(),await dashboard.locator('.service-value').textContent());
   colors[next]=await command.evaluate(el=>getComputedStyle(el).color);
   const dotColor=await page.evaluate(()=>{const el=document.createElement('span');el.style.color='var(--ok-dot)';document.body.appendChild(el);const color=getComputedStyle(el).color;el.remove();return color;});
   const actualDot=await dashboard.evaluate(el=>getComputedStyle(el,'::before').backgroundColor);
   if(next==='ready')assert.equal(actualDot,dotColor);else assert.notEqual(actualDot,dotColor);
  }
  phase='failed';state='unknown';await page.evaluate(()=>updateQuickActionServiceStatus());
  assert((await page.locator('#exec-result').textContent()).includes(await page.evaluate(()=>runtimeText('launch_failed'))));
  operationId++;phase='succeeded';await page.evaluate(()=>updateQuickActionServiceStatus());
  assert((await page.locator('#exec-result').textContent()).includes(await page.evaluate(()=>runtimeText('launch_operationReplaced'))));
  assert.notEqual(colors.ready,colors.unknown);assert.notEqual(colors.ready,colors.running);assert.notEqual(colors.failed,colors.ready);
  await page.evaluate(()=>nohupCheckProcess());
  const verified=calls.filter(c=>c.endpoint==='automation/services/status'&&c.params.verify);assert.equal(verified.length,1);assert.equal(verified[0].method,'POST');assert.equal(verified[0].params.verify,true);
  state='ready';await page.evaluate(()=>nohupStopProcess());
  assert.equal(calls.filter(c=>c.endpoint==='automation/services/stop').length,1);
  await page.evaluate(()=>stopServiceProcess(0));assert.equal(calls.filter(c=>c.endpoint==='automation/services/stop').length,2);
  // A cached READY reply begun before stopping must not repaint the stopped UI.
  state='ready';statusGate=new Promise(r=>releaseStatus=r);
  const statusPending=new Promise(r=>statusEntered=r);
  await page.evaluate(()=>{stopServiceStatusRefresh();window.oldStatus=updateQuickActionServiceStatus();});await statusPending;
  await page.evaluate(()=>stopServiceProcess(0));
  releaseStatus();statusGate=null;await page.evaluate(()=>window.oldStatus);
  assert.match(await page.locator('#service-status-model').getAttribute('class'),/status-stopped/);
  assert.equal(calls.filter(c=>c.endpoint==='automation/variables/set'||c.endpoint==='ssh/exec').length,0);
  await page.evaluate(()=>executeCommand(1));
  assert.equal(calls.filter(c=>c.endpoint==='ssh/exec_stream').length,1);
  assert.equal(calls.find(c=>c.endpoint==='ssh/exec_stream').params.command,'printf plain');
  assert.deepEqual(errors,[]);
 }finally{release();releaseStatus?.();await context.close();}
});
