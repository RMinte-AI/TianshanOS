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
   if(endpoint==='automation/config/status')data={active_version:'1'.padStart(32,'0'),configured:true,loaded:true,enabled:true,source:'sd',sd_available:true};
   if(endpoint==='ssh/hosts/list')data={hosts:deployed?[host]:[],active_version:'1'.padStart(32,'0')};
   if(endpoint==='hosts/list')data={hosts:[]};
   if(endpoint==='key/list')data={keys:[{id:'fixture-key',type:'rsa2048',bits:2048}]};
   if(endpoint==='ssh/copyid'){
    const params=req.postDataJSON();copies.push({method:req.method(),params});
    if(changed){code=1001;data={status:'mismatch',stored_fingerprint:'old',current_fingerprint:'changed'};}
    else if(params.trust_new!==true){code=1002;data={status:'new_host',fingerprint:'first'};}
    else{deployed=true;data={deployed:true,verified:true,registered:true,host_id:host.id};}
   }
   await route.fulfill({json:{code,data,message:code===1001?'ssh_host_key_changed':code===1002?'ssh_host_key_unknown':''}});
  });
  try {
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{closeLoginModal();await loadSecurityPage();showDeployKeyModal('fixture-key');});
   await page.locator('#deploy-host').fill(host.host);await page.locator('#deploy-user').fill(host.username);
   await page.locator('#deploy-password').fill('fixture-password');
   await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('success'));
   assert.equal(copies.length,1);assert.equal(copies[0].method,'POST');
   assert.deepEqual(copies[0].params,{host:host.host,user:host.username,password:'fixture-password',keyid:host.keyid,port:22,verify:true,trust_new:true,accept_changed:false,active_version:'1'.padStart(32,'0')});
   assert.equal(await page.locator('.confirm-sheet').count(),0);
   assert((await page.locator('#ssh-hosts-table-body').textContent()).includes(host.id));
   assert.equal(await page.locator('#deploy-btn').isEnabled(),false);
   const reopen=async()=>{
    await page.evaluate(()=>{closeModal();showDeployKeyModal('fixture-key');});
    await page.locator('#deploy-host').fill(host.host);await page.locator('#deploy-user').fill(host.username);
    await page.locator('#deploy-password').fill('fixture-password');
   };
   // A new explicit deployment uses the stored fingerprint, with fresh credentials.
   await reopen();await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('success'));
   assert.equal(copies.length,2);
   changed=true;await reopen();await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('error'));
   assert.equal(copies.length,3);assert.equal(copies[2].params.accept_changed,false);
   assert.equal(await page.locator('.confirm-sheet').count(),0);
   assert((await page.locator('#deploy-result').textContent()).includes(language==='zh-CN'?'指纹和之前登记的不一样':'differs from the one registered before'));
   assert((await page.locator('#ssh-hosts-table-body').textContent()).includes(host.id));
   assert.deepEqual(errors,[]);
  } finally {await context.close();}
 });
}
for(const language of ['zh-CN','en-US']){
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

// Independent background start, verify, stop and late-reply ownership are
// exercised through production run IDs in service-runs.browser.test.cjs.
