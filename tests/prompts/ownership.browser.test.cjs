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
after(async()=>{await browser?.close();await new Promise(resolve=>server?.close(resolve));});
async function pageFor(language='en-US',handler){
 const context=await browser.newContext({locale:language,viewport:{width:1280,height:900}});
 await context.addInitScript(({language})=>{
   localStorage.setItem('ts_language',language);
   window.__sockets=[];
   class Socket {
     static OPEN=1;static CONNECTING=0;static CLOSING=2;static CLOSED=3;
     constructor(){this.readyState=0;this.sent=[];window.__sockets.push(this);}
     send(text){this.sent.push(JSON.parse(text));}
     open(){this.readyState=1;this.onopen?.();}
     message(msg){this.onmessage?.({data:JSON.stringify(msg)});}
     close(){this.readyState=3;queueMicrotask(()=>this.onclose?.({code:1000}));}
     emitClose(code=1006){this.readyState=3;this.onclose?.({code});}
   }
   window.WebSocket=Socket;
 },{language});
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
   const u=new URL(route.request().url());
   if(u.origin!==base){await route.abort();return;}
   if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{}}});return;}
   if(handler&&await handler(route,u))return;
   if (process.env.PROMPT_TEST_BASELINE && /^\/js\/(app|api|router|lang\/(en-US|zh-CN))\.js$/.test(u.pathname)) {
     const name=u.pathname.split('/').pop();await route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(process.env.PROMPT_TEST_BASELINE,name),'utf8')});return;
   }
   await route.continue();
 });
 return {page,context,errors};
}

// F09 uses the production login form and api.js. Only HTTP responses are controlled.
async function sessionPage(language) {
 const fixture=await pageFor(language),{page}=fixture,requests=[];
 page.setDefaultTimeout(5000);
 let sequence=0;
 await page.route('**/api/**',async route=>{
  const request=route.request(),url=new URL(request.url()),params=request.postDataJSON();
  if(url.pathname.endsWith('/auth/login')) {
   requests.push({endpoint:'login',params});
   await route.fulfill({json:{code:0,data:{token:`${params.username}-session-${++sequence}`,username:params.username,level:params.username,expires_in:3600,password_changed:true}}});
  } else if(url.pathname.endsWith('/auth/status')) {
   requests.push({endpoint:'status',params,authorization:request.headers().authorization});
   await route.fulfill({json:{code:0,data:{valid:true}}});
  } else if(url.pathname.endsWith('/auth/logout')) {
   requests.push({endpoint:'logout',params});await route.fulfill({json:{success:true}});
  } else if(url.pathname.endsWith('/system/reboot')) {
   requests.push({endpoint:'reboot',params});await route.fulfill({json:{code:0,data:{status:'rebooting',delay_ms:500}}});
  } else await route.fulfill({json:{code:0,data:{}}});
 });
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&router.appReady);
 return {...fixture,requests};
}
async function formLogin(page,username) {
 await page.evaluate(()=>showLoginModal());
 await page.waitForTimeout(150); // showLoginModal focuses the username after 100ms.
 assert.equal(await page.locator('#username').evaluate(input=>document.activeElement===input),true);
 await page.locator('#username').fill(username);await page.locator('#password').fill('local-test-password');
 await page.locator('#login-form button[type="submit"]').click();
 try {await page.waitForFunction(username=>api.username===username&&document.getElementById('login-modal').classList.contains('hidden'),username);}
 catch(error){console.log('F09 login fixture failure',await page.evaluate(()=>({token:api.token,username:api.username,error:document.getElementById('login-error').textContent,field:document.getElementById('username').value,invalid:[...document.querySelectorAll('#login-form :invalid')].map(e=>e.id)})));throw error;}
}
async function sessionView(page) {
 return page.evaluate(()=>({token:api.token,username:api.username,level:api.level,loggedIn:api.isLoggedIn(),
  stored:['ts_token','ts_username','ts_level','ts_expires'].map(k=>localStorage.getItem(k)),
  name:document.getElementById('user-name').textContent,title:document.getElementById('user-name').title,
  loginHidden:document.getElementById('login-modal').classList.contains('hidden'),
  navigation:[...document.querySelectorAll('.nav-link')].map(e=>[e.getAttribute('href'),e.style.display])}));
}
async function holdSessionStatus(page) {
 let held,arrive;const arrived=new Promise(resolve=>arrive=resolve);
 await page.route('**/api/v1/auth/status',route=>{held=route;arrive();});
 await page.evaluate(()=>{window.f09ValidationDone=false;void validateStoredSession().then(()=>f09ValidationDone=true);});
 await arrived;
 return {request:held.request().postDataJSON(),release:async valid=>{
  if(valid==='network')await held.abort('failed');else await held.fulfill({json:{code:0,data:{valid}}});
  await page.waitForFunction(()=>f09ValidationDone);
 }};
}
for(const language of ['zh-CN','en-US']) {
 test(`F09 normal: login refresh logout and relogin ${language}`,async()=>{
  const {page,context,errors,requests}=await sessionPage(language);try{
   await formLogin(page,'admin');const first=await sessionView(page);
   await page.reload();await page.waitForFunction(()=>i18n.isReady()&&document.getElementById('user-name').textContent==='admin');
   await page.evaluate(()=>validateStoredSession());assert.deepEqual(await sessionView(page),first);
   assert(requests.some(r=>r.endpoint==='status'&&r.params.token===first.token&&r.authorization===`Bearer ${first.token}`));
   await page.locator('#login-btn').click();await page.waitForFunction(()=>api.token===null);
   await formLogin(page,'root');const next=await sessionView(page);
   assert.equal(next.username,'root');assert.equal(next.level,'root');assert.equal(next.stored[0],next.token);
   assert.equal(next.stored[2],'root');assert(next.loggedIn&&next.loginHidden);assert.notEqual(next.token,first.token);
   console.log(JSON.stringify({case:'F09 login-refresh-logout-relogin',language,first,next}));assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test(`F09 defect: old invalid status preserves newer login ${language}`,async()=>{
  const {page,context,errors}=await sessionPage(language);try{
   await formLogin(page,'admin');const old=await sessionView(page),pending=await holdSessionStatus(page);
   assert.equal(pending.request.token,old.token);
   const signOut=page.waitForResponse('**/api/v1/auth/logout');
   await page.locator('#login-btn').click();const response=await signOut;
   assert.deepEqual(response.request().postDataJSON(),{token:old.token});assert.deepEqual(await response.json(),{success:true});
   await page.waitForFunction(()=>api.token===null&&document.getElementById('login-btn').onclick===showLoginModal);
   const loggedOut=await sessionView(page);
   assert.equal(loggedOut.username,null);assert.equal(loggedOut.level,null);assert.equal(loggedOut.loggedIn,false);
   assert.deepEqual(loggedOut.stored,[null,null,null,null]);assert.equal(loggedOut.loginHidden,false);
   assert.equal(await page.evaluate(()=>f09ValidationDone),false);
   await formLogin(page,'root');const before=await sessionView(page);assert.notEqual(before.token,old.token);
   await pending.release(false);const after=await sessionView(page);
   console.log(JSON.stringify({case:'F09 stale-invalid',language,old,loggedOut,before,after}));
   assert.deepEqual(after,before);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test(`F09 normal: late logout preserves form-based newer login ${language}`,async()=>{
  const {page,context,errors}=await sessionPage(language);try{
   await formLogin(page,'admin');let held,arrive;const arrived=new Promise(resolve=>arrive=resolve);
   await page.route('**/api/v1/auth/logout',route=>{held=route;arrive();});
   await page.evaluate(()=>{window.f09LogoutDone=false;void logout().then(()=>f09LogoutDone=true);});await arrived;
   assert.equal(await page.evaluate(()=>api.token),null);
   await formLogin(page,'root');const before=await sessionView(page);
   await held.fulfill({json:{success:true}});await page.waitForFunction(()=>f09LogoutDone);
   assert.deepEqual(await sessionView(page),before);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 for(const mode of ['current-invalid','logout-old-valid','network'])test(`F09 normal: ${mode} ${language}`,async()=>{
  const {page,context,errors}=await sessionPage(language);try{
   await formLogin(page,'admin');const before=await sessionView(page),pending=await holdSessionStatus(page);
   if(mode==='logout-old-valid') {await page.locator('#login-btn').click();await page.waitForFunction(()=>api.token===null);}
   await pending.release(mode==='current-invalid'?false:mode==='network'?'network':true);
   const after=await sessionView(page);console.log(JSON.stringify({case:`F09 ${mode}`,language,before,after}));
   if(mode==='network')assert.deepEqual(after,before);
   else {assert.equal(after.token,null);assert.equal(after.loggedIn,false);assert.deepEqual(after.stored,[null,null,null,null]);}
   if(mode==='current-invalid')assert.equal(after.loginHidden,false);
   assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}
test('F60 normal: real webpage confirmation retains its 500ms API request',async()=>{
 const {page,context,errors,requests}=await sessionPage('en-US');try{
  await formLogin(page,'admin');await page.evaluate(()=>{void confirmReboot();});
  const response=page.waitForResponse('**/api/v1/system/reboot');
  await page.locator('.confirm-sheet button[data-r="1"]').click();await response;
  assert.deepEqual(requests.filter(r=>r.endpoint==='reboot'),[{endpoint:'reboot',params:{delay:500}}]);assert.deepEqual(errors,[]);
 }finally{await context.close();}
});
for(const language of ['zh-CN','en-US'])test(`F71 normal: settings form saves nondefault values and reads them back ${language}`,async()=>{
 const {page,context,errors}=await sessionPage(language);let config={low_voltage_threshold:12.6,recovery_voltage_threshold:18,shutdown_delay_sec:60,recovery_hold_sec:5,fan_stop_delay_sec:60};const writes=[];
 try{
  await formLogin(page,'admin');
  await page.route('**/api/v1/power/protection/config',route=>route.fulfill({json:{code:0,data:config}}));
  await page.route('**/api/v1/power/protection/set',route=>{
   const params=route.request().postDataJSON();writes.push(params);
   config={low_voltage_threshold:params.low_threshold,recovery_voltage_threshold:params.recovery_threshold,shutdown_delay_sec:params.shutdown_delay,recovery_hold_sec:params.recovery_hold,fan_stop_delay_sec:params.fan_stop_delay};
   return route.fulfill({json:{code:0,data:{persisted:true}}});
  });
  await page.evaluate(()=>showShutdownSettingsModal());
  const fields={'ss-low-voltage':'13.25','ss-recovery-voltage':'19.5','ss-shutdown-delay':'120','ss-recovery-hold':'17','ss-fan-stop-delay':'23'};
  for(const [id,value] of Object.entries(fields))await page.locator('#'+id).fill(value);
  await page.locator('#shutdown-settings-modal button[onclick="saveShutdownSettings()"]').click();
  await page.waitForFunction(()=>document.getElementById('shutdown-settings-modal').classList.contains('hidden'));
  assert.deepEqual(writes,[{low_threshold:13.25,recovery_threshold:19.5,shutdown_delay:120,recovery_hold:17,fan_stop_delay:23,persist:true}]);
  await page.evaluate(()=>showShutdownSettingsModal());
  for(const [id,value] of Object.entries(fields))assert.equal(await page.locator('#'+id).inputValue(),value);
  assert.equal(writes.length,1);console.log(JSON.stringify({case:'F71 real form request and config read',language,request:writes[0],config}));assert.deepEqual(errors,[]);
 }finally{await context.close();}
});


for(const language of ['zh-CN','en-US']) {
 test('review2: successful retry retains old failure '+language,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const result=await page.evaluate(async()=>{
    closeLoginModal();showConfigPackApplyConfirm('/sdcard/a.tscfg',{valid:true});
    api.call=async()=>({code:7,message:'fixture failure'});
    await applyConfigPackFromPath('/sdcard/a.tscfg');
    const failure=document.getElementById('toast').textContent;
    api.call=async()=>({code:0,data:{success:true}});
    await applyConfigPackFromPath('/sdcard/a.tscfg');
    return {failure,afterSuccess:document.getElementById('toast').textContent,dialogClosed:!document.getElementById('config-pack-apply-confirm')};
   });
   console.log(JSON.stringify({case:'successful retry',language,...result,errors}));
   assert.notEqual(result.afterSuccess,result.failure);assert.equal(result.dialogClosed,true);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test('review2: prior completion closes new package confirmation '+language,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const result=await page.evaluate(async()=>{
    closeLoginModal();showConfigPackApplyConfirm('/sdcard/a.tscfg',{valid:true});
    let resolve;api.call=()=>new Promise(r=>resolve=r);
    const pending=applyConfigPackFromPath('/sdcard/a.tscfg');
    closeConfigPackApplyConfirm();showConfigPackApplyConfirm('/sdcard/b.tscfg',{valid:true});
    const before=document.getElementById('config-pack-apply-confirm').textContent;
    resolve({code:0,data:{success:true}});await pending;
    return {secondShown:before.includes('b.tscfg'),secondRemoved:!document.getElementById('config-pack-apply-confirm'),toast:document.getElementById('toast').textContent};
   });
   console.log(JSON.stringify({case:'late completion',language,...result,errors}));
   assert.equal(result.secondShown,true);assert.equal(result.secondRemoved,false);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test('review2: uploaded file missing after dialog close '+language,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const result=await page.evaluate(async()=>{
    closeLoginModal();let entries=[],listCalls=0;
    api.storageStatus=async()=>({code:0,data:{sd:{mounted:true}}});
    api.storageList=async()=>{listCalls++;return {code:0,data:{entries:[...entries]}};};
    await loadFilesPage();showUploadDialog();
    api.fileUpload=async()=>{entries=[{name:'new-file.txt',type:'file',size:3}];return {status:'uploaded'};};
    handleFileSelect({target:{files:[new File(['abc'],'new-file.txt')]}});
    const callsBefore=listCalls;await uploadFiles();closeUploadDialog();
    const visibleAfterUpload=document.getElementById('file-list').textContent.includes('new-file.txt');
    const callsAfter=listCalls;await refreshFilesPage();
    return {callsBefore,callsAfter,visibleAfterUpload,visibleAfterManualRefresh:document.getElementById('file-list').textContent.includes('new-file.txt')};
   });
   console.log(JSON.stringify({case:'stale file list',language,...result,errors}));
   assert.equal(result.visibleAfterUpload,true);assert.equal(result.visibleAfterManualRefresh,true);assert(result.callsAfter>result.callsBefore);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}

for (const language of ['zh-CN','en-US']) {
 test(`R1: endpoint-specific logout and HTTP errors ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    const response=(ok,status,body)=>({ok,status,text:async()=>JSON.stringify(body)});
    window.fetch=async()=>response(false,401,{error:'Invalid credentials'});
    let login;try{await api.login('bad','bad');}catch(e){login={raw:e.rawMessage,uncertain:!!e.uncertain,status:e.httpStatus,message:e.message};}
    window.fetch=async()=>response(true,200,{success:true});api.token='fixture';const confirmed=await api.logout();
    let otherRejected=false;try{await api.request('/storage/delete','POST');}catch(e){otherRejected=true;}
    window.fetch=async()=>response(true,200,{success:false});api.token='fixture';const failed=await api.logout();
    window.fetch=async()=>{throw new Error('offline');};api.token='fixture';const offline=await api.logout();
    return {login,confirmed,failed:failed.serverConfirmed,offline:offline.serverConfirmed,cleared:api.token===null,otherRejected};
   });
   assert.equal(r.login.raw,'Invalid credentials');assert.equal(r.login.uncertain,false);assert.equal(r.login.status,401);
   assert.equal(r.confirmed.serverConfirmed,true);assert.equal(r.failed,false);assert.equal(r.offline,false);assert(r.cleared&&r.otherRejected);
  }finally{await context.close();}
 });
 test(`R3: late validation cannot revive an old apply entry ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    closeLoginModal();const list=document.createElement('div');list.id='upload-list';document.body.appendChild(list);
    filesToUpload=[];currentFilePath='/sdcard';handleFileSelect({target:{files:[new File(['x'],'same.tscfg')]}});
    let resolve;api.fileUpload=()=>new Promise(r=>resolve=r);const old=uploadFiles();
    api.fileUpload=async()=>({status:'uploaded',config_pack:{valid:false,result_message:'bad signature'}});await uploadFiles();
    const before=list.textContent;resolve({status:'uploaded',config_pack:{valid:true,signature:{signer_cn:'old'}}});await old;
    return {before,after:list.textContent,buttons:list.querySelectorAll('button').length};
   });
   assert.equal(r.before,r.after);assert.equal(r.buttons,1);
  }finally{await context.close();}
 });
 test(`R3: previously opened confirmation loses validity on reupload ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    closeLoginModal();const list=document.createElement('div');list.id='upload-list';document.body.appendChild(list);
    filesToUpload=[];currentFilePath='/sdcard';handleFileSelect({target:{files:[new File(['x'],'same.tscfg')]}});
    api.fileUpload=async()=>({status:'uploaded',config_pack:{valid:true}});await uploadFiles();list.querySelector('.upload-item').lastChild.click();
    api.fileUpload=async()=>({status:'uploaded',config_pack:{valid:false}});await uploadFiles();
    let calls=0;api.call=async()=>{calls++;return {code:0,data:{success:true}};};await applyConfigPackFromPath('/sdcard/same.tscfg');
    return {calls};
   });assert.equal(r.calls,0);
  }finally{await context.close();}
 });
 test(`R4/R5: obsolete errors and finally cannot update newer attempts ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    closeLoginModal();let rejectOld,resolveNew;
    showConfigPackApplyConfirm('/sdcard/same.tscfg',{valid:true});api.call=()=>new Promise((_,r)=>rejectOld=r);const old=applyConfigPackFromPath('/sdcard/same.tscfg');
    closeConfigPackApplyConfirm();showConfigPackApplyConfirm('/sdcard/same.tscfg',{valid:true});api.call=()=>new Promise(r=>resolveNew=r);const newer=applyConfigPackFromPath('/sdcard/same.tscfg');
    const message=document.getElementById('toast').textContent;rejectOld(new Error('older failure'));await old;
    const preserved=message===document.getElementById('toast').textContent;
    const disabled=document.getElementById('config-pack-apply-button').disabled;
    resolveNew({code:0,data:{success:true}});await newer;
    return {preserved,disabled,history:document.getElementById('config-pack-task-results')?.textContent,said:describeFailure({op:'pack.apply',error:new Error('older failure'),endpoint:'config.pack.apply',write:true}).message};
   });assert(r.preserved&&r.disabled);assert(r.history.includes(r.said),r.history);
  }finally{await context.close();}
 });
 test(`R6: fixed upload destination and separate refresh failure ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    closeLoginModal();api.storageStatus=async()=>({code:0,data:{sd:{mounted:true}}});api.storageList=async()=>({code:0,data:{entries:[]}});
    await loadFilesPage();currentFilePath='/sdcard/a';showUploadDialog();handleFileSelect({target:{files:[new File(['a'],'a.txt'),new File(['b'],'b.txt')]}});
    let resolve;const paths=[];api.fileUpload=async path=>{paths.push(path);if(paths.length===1)await new Promise(r=>resolve=r);return {status:'uploaded'};};
    const pending=uploadFiles();currentFilePath='/sdcard/b';resolve();await pending;
    showUploadDialog();handleFileSelect({target:{files:[new File(['c'],'c.txt')]}});api.storageList=async()=>({code:7,error:'read failed'});const result=await uploadFiles();
    return {paths,result,list:document.getElementById('file-list').textContent,summary:document.getElementById('operation-summary').textContent,said:describeFailure({op:'file.list',error:new ApiOperationError({code:7,error:'read failed'},'storage.list'),endpoint:'storage.list',write:false}).message,note:t('promptRepair.listRefreshFailed')};
   });assert.deepEqual(r.paths.slice(0,2),['/sdcard/a/a.txt','/sdcard/a/b.txt']);assert.equal(r.result.success,1);assert.equal(r.result.failures.length,0);assert(r.list.includes(r.said),r.list);assert(r.summary.includes(r.note),r.summary);
  }finally{await context.close();}
 });
 test(`R6: out-of-order directory success and error do not replace latest list ${language}`,async()=>{
  const {page,context}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const r=await page.evaluate(async()=>{
    closeLoginModal();api.storageStatus=async()=>({code:0,data:{sd:{mounted:true}}});api.storageList=async()=>({code:0,data:{entries:[]}});await loadFilesPage();
    let release;api.storageList=()=>new Promise(r=>release=r);const old=loadDirectory('/spiffs/old');
    api.storageList=async()=>({code:0,data:{entries:[{name:'latest.txt',type:'file',size:1}]}});await loadDirectory('/spiffs/new');
    release({code:0,data:{entries:[{name:'obsolete.txt',type:'file',size:1}]}});await old;
    const first=document.getElementById('file-list').textContent;
    api.storageList=()=>new Promise(r=>release=r);const oldFailure=loadDirectory('/spiffs/new');
    api.storageList=async()=>({code:0,data:{entries:[{name:'newest.txt',type:'file',size:1}]}});await loadDirectory('/spiffs/new');release({code:7,error:'old error'});await oldFailure;
    return {first,last:document.getElementById('file-list').textContent,path:currentFilePath};
   });assert(r.first.includes('latest.txt')&&!r.first.includes('obsolete.txt'));assert(r.last.includes('newest.txt')&&!r.last.includes('old error'));assert.equal(r.path,'/spiffs/new');
  }finally{await context.close();}
 });
}

test('R2: authenticated startup initializes once',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await context.addInitScript(()=>{
   localStorage.setItem('ts_token','fixture');localStorage.setItem('ts_username','root');localStorage.setItem('ts_level','root');localStorage.setItem('ts_expires',String(Date.now()+3600000));window.loads=0;
   document.addEventListener('DOMContentLoaded',()=>{const load=window.loadSystemPage;window.loadSystemPage=async function(){loads++;return load();};},{once:true});
  });
  await page.route('**/api/**',async route=>{if(route.request().url().includes('system/info'))await new Promise(r=>setTimeout(r,100));await route.fulfill({json:{code:0,data:{valid:true}}});});
  await page.goto(base);await page.waitForFunction(()=>window.subscriptionManager?.subscriptions.get('system.dashboard')?.size);
  assert.equal(await page.evaluate(()=>loads),1);assert.equal(await page.evaluate(()=>subscriptionManager.subscriptions.get('system.dashboard').size),1);
 }finally{await context.close();}
});
test('R2: obsolete system loader cannot update new page; failed navigation retries',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  const r=await page.evaluate(async()=>{
   closeLoginModal();api.isLoggedIn=()=>true;api.isRoot=()=>true;
   let release;api.getSystemInfo=()=>new Promise(r=>release=r);let oldUpdates=0;updateSystemInfo=()=>oldUpdates++;
   const old=router.navigate();
   router.routes['/']=async()=>{document.getElementById('page-content').textContent='new page';};await router.navigate();
   release({code:0,data:{hostname:'obsolete'}});await old;
   const preserved=document.getElementById('page-content').textContent==='new page';
   let calls=0;router.routes['/']=async()=>{calls++;if(calls===1)throw new Error('fixture load failure');document.getElementById('page-content').textContent='recovered';};
   await router.navigate();const retry=document.getElementById('page-content').querySelector('button');
   const retryPresent=!!retry;if(retry)await retry.onclick();
   return {oldUpdates,preserved,retryPresent,recovered:document.getElementById('page-content').textContent==='recovered'};
  });assert.equal(r.oldUpdates,0);assert(r.preserved&&r.retryPresent&&r.recovered);
 }finally{await context.close();}
});

test('R4/R5: newest result wins over late success and late failure',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  for(const oldFails of [false,true]){
   const r=await page.evaluate(async oldFails=>{
    closeLoginModal();let finishOld;
    showConfigPackApplyConfirm('/sdcard/retry.tscfg',{valid:true});api.call=()=>new Promise((resolve,reject)=>finishOld=()=>oldFails?reject(new Error('obsolete error')):resolve({code:0,data:{success:true}}));const old=applyConfigPackFromPath('/sdcard/retry.tscfg');
    closeConfigPackApplyConfirm();showConfigPackApplyConfirm('/sdcard/retry.tscfg',{valid:true});api.call=async()=>oldFails?{code:0,data:{success:true}}:{code:7,error:'current error'};await applyConfigPackFromPath('/sdcard/retry.tscfg');
    const before=document.getElementById('toast').textContent;const modal=document.getElementById('config-pack-apply-confirm');finishOld();await old;
    return {before,after:document.getElementById('toast').textContent,sameModal:document.getElementById('config-pack-apply-confirm')===modal};
   },oldFails);assert.equal(r.before,r.after);assert(r.sameModal);
  }
 }finally{await context.close();}
});
test('R3: overlapping writes to one path cannot claim current verified content',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  const r=await page.evaluate(async()=>{
   closeLoginModal();const list=document.createElement('div');list.id='upload-list';document.body.appendChild(list);filesToUpload=[];currentFilePath='/sdcard';handleFileSelect({target:{files:[new File(['x'],'same.tscfg')]}});
   let finishOld;api.fileUpload=()=>new Promise(r=>finishOld=r);const old=uploadFiles();api.fileUpload=async()=>({status:'uploaded',config_pack:{valid:true}});await uploadFiles();
   const before=list.textContent;finishOld({status:'uploaded',config_pack:{valid:true}});await old;
   return {buttons:list.querySelectorAll('button').length,before,after:list.textContent};
  });assert.equal(r.buttons,1);assert.equal(r.before,r.after);
 }finally{await context.close();}
});
test('R2: disposed page only removes its dashboard callback',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  const r=await page.evaluate(async()=>{
   closeLoginModal();api.isLoggedIn=()=>true;api.isRoot=()=>true;
   const foreign=()=>{};subscriptionManager.subscribe('system.dashboard',foreign);await router.navigate();
   const before=subscriptionManager.subscriptions.get('system.dashboard').size;
   router.routes['/']=()=>{document.getElementById('page-content').textContent='other';};await router.navigate();
   const callbacks=subscriptionManager.subscriptions.get('system.dashboard');return {before,after:callbacks?.size,foreign:callbacks?.has(foreign)};
  });assert.equal(r.before,2);assert.equal(r.after,1);assert(r.foreign);
 }finally{await context.close();}
});

test('R2: stale finally cannot unlock the new page read',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  const r=await page.evaluate(async()=>{
   closeLoginModal();api.isLoggedIn=()=>true;api.isRoot=()=>true;dataWidgets=[{id:'fixture',type:'value',expression:'${fixture}'}];updateWidgetValue=()=>{};
   let finishOld,finishNew;api.call=()=>new Promise(r=>finishOld=r);const old=refreshDataWidgets();
   router.routes['/']=()=>{};await router.navigate();api.call=()=>new Promise(r=>finishNew=r);const newer=refreshDataWidgets();
   const newStarted=typeof finishNew==='function';finishOld({code:0,data:{variables:[]}});await old;
   const stillLocked=dataWidgetsRefreshing;if(finishNew)finishNew({code:0,data:{variables:[]}});await newer;
   return {newStarted,stillLocked,released:!dataWidgetsRefreshing};
  });assert(r.newStarted&&r.stillLocked&&r.released);
 }finally{await context.close();}
});

test('R1: delayed sign-out does not clear a newer sign-in',async()=>{
 const {page,context}=await pageFor('en-US');try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
  const r=await page.evaluate(async()=>{
   let release;api.token='old';window.fetch=()=>new Promise(r=>release=r);const pending=api.logout();const clearedImmediately=api.token===null;
   api.token='new';release({ok:true,status:200,text:async()=>JSON.stringify({success:true})});const result=await pending;
   return {clearedImmediately,token:api.token,confirmed:result.serverConfirmed};
  });assert(r.clearedImmediately&&r.confirmed);assert.equal(r.token,'new');
 }finally{await context.close();}
});

for (const language of ['zh-CN','en-US']) {
 test(`WS protocol: actual page subscription class, independent intervals and acknowledgements ${language}`,async()=>{
  const first=await pageFor(language),second=await pageFor(language);
  try {
   await Promise.all([first.page.goto(base),second.page.goto(base)]);
   await Promise.all([first.page.waitForFunction(()=>i18n.isReady()),second.page.waitForFunction(()=>i18n.isReady())]);
   async function exercise(page,interval) {
    return page.evaluate(interval=>{
     const sent=[],received=[];
     const mgr=new SubscriptionManager({send:message=>sent.push(message)});
     const callback=message=>received.push(message.data);
     mgr.subscribe('system.cpu',callback,{interval});
     mgr.handleMessage({type:'subscribed',topic:'system.cpu',success:true});
     mgr.handleMessage({type:'data',topic:'system.cpu',data:{usage:12},timestamp:1});
     mgr.unsubscribe('system.cpu',callback);
     mgr.handleMessage({type:'unsubscribed',topic:'system.cpu',success:true});
     mgr.handleMessage({type:'data',topic:'system.cpu',data:{usage:99},timestamp:2});
     return {sent,received,active:mgr.activeSubs.size};
    },interval);
   }
   const [a,b]=await Promise.all([exercise(first.page,1000),exercise(second.page,10000)]);
   assert.equal(a.sent[0].params.interval,1000);assert.equal(b.sent[0].params.interval,10000);
   for(const result of [a,b]) {
    assert.deepEqual(result.received,[{usage:12}]);assert.equal(result.active,0);
    assert.deepEqual(result.sent[1],{type:'unsubscribe',topic:'system.cpu'});
   }
  }finally{await first.context.close();await second.context.close();}
 });
}

for(const language of ['zh-CN','en-US']) {
 test(`A02: login/logout replaces only current main and terminal sockets ${language}`,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&window.ws);
   const result=await page.evaluate(async()=>{
    closeLoginModal();
    api.call=async(name,params)=>name==='auth.login'?{code:0,data:{token:params.username+'-token',username:params.username,level:params.username,expires_in:3600}}:{code:0,data:{}};
    const old=ws.ws,events=[];window.addEventListener('authChanged',()=>events.push(api.token));
    const set=window.setTimeout,clear=window.clearTimeout;let staleReconnect,clearedReconnect=false;
    window.setTimeout=(fn,delay,...args)=>{if(delay===1000){staleReconnect=fn;return 271828;}return set(fn,delay,...args);};
    window.clearTimeout=id=>{if(id===271828){clearedReconnect=true;return;}return clear(id);};
    old.emitClose();
    subscriptionManager.subscribe('system.cpu',()=>{},{interval:2345});
    await api.login('admin','fixture');const admin=ws.ws;admin.open();
    const order=admin.sent.map(m=>m.type),restored=admin.sent.find(m=>m.topic==='system.cpu');
    staleReconnect();window.setTimeout=set;window.clearTimeout=clear;
    old.onmessage({data:JSON.stringify({type:'auth_error',reason:'session_expired'})});old.onclose({code:1006});
    const oldIgnored=api.token==='admin-token'&&ws.ws===admin&&clearedReconnect;
    const term=new WebTerminal('unused');term.connect();term.ws.open();const firstTerminal=term.ws.sent[0];
    const oldTerminal=term.ws;
    await api.login('root','fixture');const root=ws.ws;root.open();const rootTerminal=term.ws;rootTerminal.open();rootTerminal.message({type:'connected'});
    term.startSshShell({host:'fixture.invalid',port:22,user:'u',password:'p'});
    const ssh=rootTerminal.sent.find(m=>m.type==='ssh_connect');
    oldTerminal.onmessage({data:JSON.stringify({type:'auth_error',reason:'session_expired'})});oldTerminal.onclose({code:1006});
    const terminalIgnored=term.ws===rootTerminal&&api.token==='root-token';
    root.message({type:'auth_ok'});const prepared=await ws.prepareOutputIdentity();
    root.message({type:'auth_error',reason:'session_expired'});
    const expired=api.token===null&&ws.ws!==root;
    const anonymous=ws.ws;anonymous.open();anonymous.message({type:'auth_error',message:'Unauthorized'});
    const publicOnly=anonymous.sent[0].type==='auth'&&anonymous.sent[0].token===null&&anonymous.sent[1].type==='subscribe'&&!ws.authenticated&&api.token===null;
    term.destroy();const count=__sockets.length;api.clearAuth();const destroyedListenerRemoved=__sockets.length===count+1;
    return {order,restored,oldIgnored,firstTerminal,ssh,terminalIgnored,prepared,expired,publicOnly,destroyedListenerRemoved,events};
   });
   assert.equal(result.order[0],'auth');assert.equal(result.order[1],'subscribe');assert.deepEqual(result.restored.params,{interval:2345});
   assert(result.oldIgnored&&result.terminalIgnored&&result.prepared&&result.expired&&result.publicOnly&&result.destroyedListenerRemoved);
   assert.equal(result.firstTerminal.token,'admin-token');assert.equal(result.ssh.token,'root-token');assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test(`A02: upload paths retain their original body/result and send current token ${language}`,async()=>{
  const {page,context,errors}=await pageFor(language);try{
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const result=await page.evaluate(async()=>{
    closeLoginModal();api.token='admin-token';const calls=[];
    window.fetch=async(url,options)=>{calls.push({url,headers:options.headers,body:options.body});return {ok:true,status:200,json:async()=>({status:String(url).includes('/file/upload')?'uploaded':'success'})};};
    const file=await api.fileUpload('/sdcard/ordinary file.txt','ordinary');
    const firmware=await uploadImageToDevice('/api/v1/ota/firmware?auto_reboot=false','firmware','ota.firmware');
    const www=await uploadImageToDevice('/api/v1/ota/www','www','ota.www');
    return {calls,file,firmware,www};
   });
   assert.equal(result.file.status,'uploaded');assert(result.firmware.success&&result.www.success);
   assert.deepEqual(result.calls.map(c=>c.headers.Authorization),['Bearer admin-token','Bearer admin-token','Bearer admin-token']);
   assert.deepEqual(result.calls.map(c=>c.body),['ordinary','firmware','www']);assert.equal(result.calls[1].headers['Content-Type'],'application/octet-stream');assert.match(result.calls[1].url,/auto_reboot=false$/);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 for(const mode of ['early','late','auth-error','timeout','closed','http-error','logout','same-token-auth-change','cancel','page-replaced']) {
  test(`A02: immediate foreground execution ${mode} ${language}`,async()=>{
   const {page,context,errors}=await pageFor(language);try{
    await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&window.ws);
    const result=await page.evaluate(async mode=>{
     closeLoginModal();window.submitted=[];
     api.call=async(name,params)=>{
      if(name==='auth.login')return {code:0,data:{token:'admin-token',username:'admin',level:'admin',expires_in:3600}};
      if(name==='ssh.exec_stream'){submitted.push(structuredClone(params));return mode==='http-error'?{code:4,message:'ssh_exec_busy'}:{code:0,data:{session_id:25}};}
      return {code:0,data:{hosts:name==='ssh.hosts.list'?[{id:'local',username:'ordinary',host:'fixture.invalid',port:22,keyid:'default'}]:[],commands:[]}};
     };
     await api.login('admin','fixture');updateAuthUI();await loadCommandsPage();
     window._cmdHostsList=[{id:'local',username:'ordinary',host:'fixture.invalid',port:22,keyid:'default'}];
     sshCommands={local:[{id:'foreground',name:'Normal command',command:'printf ordinary',enabled:true}]};selectHost('local');
     const socket=ws.ws;socket.open();
     if(mode==='early')socket.message({type:'auth_ok'});
     if(mode==='closed')socket.emitClose(1000);
     const set=window.setTimeout,clear=window.clearTimeout;let deadline,deadlines=0,cleared=0;
     if(mode==='timeout') {
      window.setTimeout=(fn,delay,...args)=>{if(delay===1000){deadlines++;deadline=fn;return 314159;}return set(fn,delay,...args);};
      window.clearTimeout=id=>{if(id===314159){cleared++;return;}return clear(id);};
     }
     const pending=executeCommand(0);await Promise.resolve();const before=submitted.length;
     if(mode==='late')socket.message({type:'auth_ok'});
     if(mode==='auth-error'||mode==='http-error')socket.message({type:'auth_error',message:'Unauthorized'});
     if(mode==='timeout'){deadline();window.setTimeout=set;window.clearTimeout=clear;}
     if(mode==='logout')api.clearAuth();
     if(mode==='same-token-auth-change')window.dispatchEvent(new CustomEvent('authChanged'));
     if(mode==='cancel')await cancelExecution();
     if(mode==='page-replaced')releaseExecDisplay();
     await pending;
     const after=submitted.length,text=document.getElementById('exec-result').textContent,token=api.token;
     socket.onmessage({data:JSON.stringify({type:'auth_ok'})});await Promise.resolve();
     return {before,after,final:submitted.length,text,token,deadlines,cleared,waiters:ws.authWaiters?.size||0,hasSession:currentExecSessionId===25,params:submitted[0]};
    },mode);
    const cancelled=['logout','same-token-auth-change','cancel','page-replaced'].includes(mode);
    assert.equal(result.after,cancelled?0:1);assert.equal(result.final,result.after);assert.equal(result.waiters,0);
    if(!['early','closed'].includes(mode))assert.equal(result.before,0);
    if(mode==='timeout'){assert.equal(result.deadlines,1);assert.equal(result.cleared,1);}
    if(!cancelled&&mode!=='http-error')assert(result.hasSession);
    if(['auth-error','timeout','closed'].includes(mode))assert.match(result.text,language==='zh-CN'?/指令已提交，实时输出尚未连接/:/Command submitted; live output is not connected/);
    if(mode==='http-error'){assert(!result.hasSession);assert.doesNotMatch(result.text,/实时输出尚未连接|live output is not connected/);}
    if(mode==='auth-error')assert.equal(result.token,'admin-token');
    if(result.params)assert.equal(result.params.command,'printf ordinary');
    assert.deepEqual(errors,[]);
    if(mode==='auth-error'&&process.env.TS_TEST_EVIDENCE_DIR)await page.screenshot({path:path.join(process.env.TS_TEST_EVIDENCE_DIR,`a02-${language}-submitted-output-unavailable.png`)});
   }finally{await context.close();}
  });
 }
}

for(const language of ['zh-CN','en-US'])test(`A02: no-auth server ACK keeps real logged-out UI output connected ${language}`,async()=>{
 const {page,context,errors}=await pageFor(language);const submissions=[];try{
  const version='00000000000000000000000000000001';
  await page.route('**/api/**',async route=>{
   const request=route.request(),u=new URL(request.url());let data={};
   if(u.pathname.endsWith('/auth/logout')){await route.fulfill({json:{success:true}});return;}
   if(u.pathname.endsWith('/auth/login'))data={token:'root-token',username:'root',level:'root',expires_in:3600,password_changed:true};
   if(u.pathname.endsWith('/auth/status'))data={valid:true,username:'root',level:'root'};
   if(u.pathname.endsWith('/ssh/hosts/list'))data={active_version:version,hosts:[{id:'local',host:'fixture.invalid',port:22,username:'ordinary',keyid:'default'}]};
   if(u.pathname.endsWith('/ssh/commands/list'))data={active_version:version,commands:[{id:'foreground',host_id:'local',name:'Normal command',command:'printf ordinary',enabled:true}]};
   if(u.pathname.endsWith('/ssh/exec_stream')){submissions.push({authorization:request.headers().authorization||null,params:request.postDataJSON()});data={session_id:25};}
   await route.fulfill({json:{code:0,data}});
  });
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&window.ws);
  await page.locator('#username').fill('root');await page.locator('#password').fill('fixture');await page.locator('#login-form button[type="submit"]').click();
  await page.waitForFunction(()=>api.token==='root-token');await page.evaluate(()=>{ws.ws.open();ws.ws.message({type:'auth_ok'});location.hash='/commands';});
  await page.locator('.page-commands .host-card').click();await page.locator('.page-commands .btn-exec').waitFor({state:'visible'});
  await page.locator('#login-btn').click();await page.waitForFunction(()=>api.token===null&&!document.getElementById('login-modal').classList.contains('hidden'));
  await page.locator('#login-form button[type="button"]').click();
  const ack=await page.evaluate(()=>{
   ws.ws.open();const before=ws.authenticated,sent=ws.ws.sent;
   ws.ws.message({type:'auth_ok'});
   return {before,after:ws.authenticated,sent,token:api.token,retainedCommands:!!document.querySelector('.page-commands'),routeDenied:!router.checkAccess('/commands').allowed};
  });
  assert(!ack.before&&ack.after&&ack.retainedCommands&&ack.routeDenied);assert.equal(ack.token,null);
  assert.equal(ack.sent[0].type,'auth');assert.equal(ack.sent[0].token,null);assert.equal(ack.sent[1].type,'subscribe');
  await page.locator('.page-commands .btn-exec').click();await page.waitForFunction(()=>currentExecSessionId===25);
  await page.evaluate(()=>ws.ws.message({type:'ssh_exec_output',session_id:25,data:'VALUE_FROM_SERVER\n'}));
  const output=await page.locator('#exec-result').innerText();assert.match(output,/VALUE_FROM_SERVER/);assert.doesNotMatch(output,/实时输出尚未连接|live output is not connected/);
  assert.equal(submissions.length,1);assert.equal(submissions[0].authorization,null);assert.deepEqual(errors,[]);
  if(process.env.TS_TEST_EVIDENCE_DIR)await page.screenshot({path:path.join(process.env.TS_TEST_EVIDENCE_DIR,`a02-noauth-${language}-output-connected.png`)});
 }finally{await context.close();}
});

async function batchFilesPage(language,{hold=false,failSecond=false}={}){
 const f=await pageFor(language);
 await f.page.goto(base);await f.page.waitForFunction(()=>i18n.isReady());
 await f.page.evaluate(async({hold,failSecond})=>{
  closeLoginModal();
  window.batchEntries=[{name:'A 中文.txt',type:'file',size:3},{name:'B folder',type:'dir'},{name:'C.txt',type:'file',size:5}];
  window.deleteCalls=[];window.fileListReads=0;
  api.storageStatus=async()=>({code:0,data:{sd:{mounted:true},spiffs:{mounted:true}}});
  api.storageList=async()=>{++fileListReads;return {code:0,data:{entries:structuredClone(batchEntries)}};};
  api.storageDelete=async path=>{
   deleteCalls.push(path);
   if(hold&&deleteCalls.length===1)await new Promise(resolve=>{window.releaseDelete=resolve;});
   if(failSecond&&path==='/sdcard/B folder')return {code:3,error:'Permission denied'};
   batchEntries=batchEntries.filter(entry=>'/sdcard/'+entry.name!==path);
   return {code:0,data:{}};
  };
  await loadFilesPage();
 },{hold,failSecond});
 return f;
}
for(const language of ['zh-CN','en-US']){
 test(language+': A04 normal one/multiple file deletion and cancellation retain request counts and refresh behavior',async()=>{
  for(const count of [1,2]){
   const f=await batchFilesPage(language);try{
    const {page}=f;
    await page.getByRole('checkbox',{name:'A 中文.txt',exact:true}).check();
    if(count===2)await page.getByRole('checkbox',{name:'B folder',exact:true}).check();
    const before=await page.evaluate(()=>fileListReads);
    await page.locator('#batch-toolbar button[onclick="batchDelete()"]').click();
    assert.match(await page.locator('.confirm-sheet').innerText(),new RegExp(String(count)));
    await page.locator('.confirm-sheet button[data-r="1"]').click();
    await page.waitForFunction(()=>document.getElementById('operation-summary'));
    assert.deepEqual(await page.evaluate(()=>deleteCalls),['/sdcard/A 中文.txt','/sdcard/B folder'].slice(0,count));
    assert.equal(await page.evaluate(()=>selectedFiles.size),0);
    assert.equal(await page.evaluate(()=>fileListReads),before+1);
    assert.equal(await page.locator('#toast').getAttribute('class').then(x=>x.includes('toast-success')),true);
    assert.equal(await page.getByRole('checkbox',{name:'C.txt',exact:true}).isChecked(),false);
    assert.deepEqual(f.errors,[]);
   }finally{await f.context.close();}
  }
  const f=await batchFilesPage(language);try{
   await f.page.getByRole('checkbox',{name:'A 中文.txt',exact:true}).check();
   const before=await f.page.evaluate(()=>fileListReads);
   await f.page.locator('#batch-toolbar button[onclick="batchDelete()"]').click();
   await f.page.locator('.confirm-sheet button[data-r="0"]').click();
   assert.deepEqual(await f.page.evaluate(()=>deleteCalls),[]);
   assert.equal(await f.page.evaluate(()=>fileListReads),before);
   assert.deepEqual(await f.page.evaluate(()=>[...selectedFiles]),['/sdcard/A 中文.txt']);
   assert.equal(await f.page.getByRole('checkbox',{name:'A 中文.txt',exact:true}).isChecked(),true);
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 for(const failSecond of [false,true]){
  test(language+': A04 confirmed paths stay fixed while awaiting the first delete; second failure='+failSecond,async()=>{
   const f=await batchFilesPage(language,{hold:true,failSecond});try{
    const {page}=f;
    await page.getByRole('checkbox',{name:'A 中文.txt',exact:true}).check();
    await page.getByRole('checkbox',{name:'B folder',exact:true}).check();
    const before=await page.evaluate(()=>fileListReads);
    await page.locator('#batch-toolbar button[onclick="batchDelete()"]').click();
    assert.match(await page.locator('.confirm-sheet').innerText(),/2/);
    await page.locator('.confirm-sheet button[data-r="1"]').click();
    await page.waitForFunction(()=>deleteCalls.length===1&&typeof releaseDelete==='function');
    await page.getByRole('checkbox',{name:'C.txt',exact:true}).check();
    await page.getByRole('checkbox',{name:'B folder',exact:true}).uncheck();
    assert.deepEqual(await page.evaluate(()=>[...selectedFiles]),['/sdcard/A 中文.txt','/sdcard/C.txt']);
    await page.evaluate(()=>releaseDelete());
    await page.waitForFunction(()=>document.getElementById('operation-summary'));
    assert.deepEqual(await page.evaluate(()=>deleteCalls),['/sdcard/A 中文.txt','/sdcard/B folder']);
    const expected=await page.evaluate(failSecond=>t('promptRepair.deleteSummary',{success:failSecond?1:2,fail:failSecond?1:0}),failSecond);
    assert.equal(await page.locator('#toast').innerText(),expected);
    const summary=await page.locator('#operation-summary').innerText();
    assert(summary.startsWith(expected));assert.equal(summary.includes('/sdcard/B folder'),failSecond);
    assert.equal(summary.includes('/sdcard/C.txt'),false);
    assert.equal(await page.evaluate(()=>fileListReads),before+1);
    assert.equal(await page.evaluate(()=>selectedFiles.size),0);
    assert.equal(await page.getByRole('checkbox',{name:'C.txt',exact:true}).isChecked(),false);
    assert.equal(await page.locator('#toast').getAttribute('class').then(x=>x.includes('toast-'+(failSecond?'warning':'success'))),true);
    assert.deepEqual(f.errors,[]);
   }finally{await f.context.close();}
  });
 }
}

for(const language of ['zh-CN','en-US']) {
 test('ordinary file names survive upload and download '+language,async()=>{
  const f=await pageFor(language);try{
   const names=['report.txt','report+v1.txt','report v1.txt','report%20v1.txt','report%25v1.txt','进度100%.txt','记录.txt'];
   const directory='/sdcard/b02',stored=new Map(),uploads=[],downloads=[];
   // Register these routes after the common fixture; only this flow models file bytes.
   await f.page.route('**/api/v1/storage/list',async route=>{
    assert.equal(route.request().postDataJSON().path,directory);
    await route.fulfill({json:{code:0,data:{entries:[...stored].map(([file,body])=>({name:file.slice(directory.length+1),type:'file',size:body.length}))}}});
   });
   await f.page.route('**/api/v1/file/*',async route=>{
    const request=route.request(),url=new URL(request.url()),file=url.searchParams.get('path');
    assert(names.some(name=>file===directory+'/'+name));
    assert.equal(request.url(),base+url.pathname+'?path='+encodeURIComponent(file));
    if(url.pathname.endsWith('/upload')){
     assert.equal(request.method(),'POST');const body=request.postDataBuffer();assert(body);
     stored.set(file,body);uploads.push(file);
     await route.fulfill({json:{path:file,size:body.length,status:'uploaded'}});
    }else{
     assert(url.pathname.endsWith('/download'));assert(stored.has(file));downloads.push(file);
     await route.fulfill({status:200,contentType:'text/plain',body:stored.get(file)});
    }
   });
   await f.page.goto(base);await f.page.waitForFunction(()=>i18n.isReady());
   await f.page.evaluate(async directory=>{
    closeLoginModal();currentFilePath=directory;
    api.storageStatus=async()=>({code:0,data:{sd:{mounted:true}}});
    await loadFilesPage();showUploadDialog();
   },directory);
   const files=names.map((name,i)=>({name,mimeType:'text/plain',buffer:Buffer.from('independent file contents '+i)}));
   await f.page.locator('#file-input').setInputFiles(files);
   await f.page.evaluate(()=>uploadFiles());
   assert.deepEqual(uploads,names.map(name=>directory+'/'+name));assert.equal(stored.size,names.length);
   for(let i=0;i<names.length;i++){
    const pending=f.page.waitForEvent('download');
    assert.deepEqual(await f.page.evaluate(file=>downloadFile(file),directory+'/'+names[i]),{started:true});
    const download=await pending;assert.equal(download.suggestedFilename(),names[i]);
    const stream=await download.createReadStream(),parts=[];for await(const part of stream)parts.push(part);
    assert.deepEqual(Buffer.concat(parts),files[i].buffer);
   }
   assert.deepEqual(downloads,names.map(name=>directory+'/'+name));assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
}

for (const language of ['zh-CN','en-US']) {
 test(`B07 LED buttons: explicit device off, all off and existing on policy ${language}`,async()=>{
  const {page,context,errors}=await pageFor(language),writes=[];
  try {
   await page.route('**/api/v1/led/**',async route=>{
    const request=route.request(),endpoint=new URL(request.url()).pathname;
    if(request.method()==='POST')writes.push({endpoint,params:request.postDataJSON()});
    const devices=['touch','board','matrix'].map(name=>({name,layout:name==='matrix'?'matrix':'strip',brightness:80,effects:['solid','breathing'],current:{on:true,animation:'breathing',speed:73,color:{r:0,g:0,b:255}}}));
    await route.fulfill({json:{code:0,data:endpoint.endsWith('/list')?{devices}:{}}});
   });
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{
    closeLoginModal();api.token='local-fixture';
    await loadSystemPage();
   });
   await page.locator('#toggle-matrix').click();
   await page.waitForFunction(()=>ledStates.matrix===false);
   assert.deepEqual(writes.at(-1),{endpoint:'/api/v1/led/clear',params:{device:'matrix',off:true}});
   await page.locator('#toggle-matrix').click();
   await page.waitForFunction(()=>ledStates.matrix===true);
   assert.deepEqual(writes.at(-1),{endpoint:'/api/v1/led/fill',params:{device:'matrix',color:'#ffffff'}});
   const before=writes.length;await page.locator('[onclick="allLedsOff()"]').click();
   await page.waitForFunction(()=>['touch','board','matrix'].every(device=>ledStates[device]===false));
   assert.deepEqual(writes.slice(before),['touch','board','matrix'].map(device=>({endpoint:'/api/v1/led/clear',params:{device,off:true}})));
   await page.evaluate(()=>openLedModal('matrix','effect'));
   await page.locator('#modal-device-enabled-matrix').check();
   await page.waitForFunction(()=>ledStates.matrix===true);
   assert.deepEqual(writes.at(-1),{endpoint:'/api/v1/led/effect/start',params:{device:'matrix',effect:'breathing',speed:73,color:'#0000ff'}});
   assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}

for (const language of ['zh-CN', 'en-US']) {
 test(`F50: network-only automatic reconnect restores visible home data and current listeners ${language}`, async () => {
  const {page,context,errors}=await pageFor(language);
  try {
   await page.addInitScript(()=>{
    localStorage.setItem('ts_token','f50-fixture-root');localStorage.setItem('ts_username','root');
    localStorage.setItem('ts_level','root');localStorage.setItem('ts_expires',String(Date.now()+3600000));
   });
   await page.route('**/api/v1/auth/status',route=>route.fulfill({json:{code:0,data:{valid:true,username:'root',level:'root'}}}));
   await page.route('**/api/v1/ui/widgets/get',route=>route.fulfill({json:{code:0,data:{widgets:[],refresh_interval:0,source:'nvs'}}}));
   await page.goto(base);
   await page.waitForFunction(()=>i18n.isReady()&&subscriptionManager?.subscriptions.has('system.dashboard'));
   const initial=await page.evaluate(()=>{
    closeLoginModal();window.__f50Cpu=document.getElementById('cpu-avg');window.__f50Identity=api.token;
    const transport=ws.ws;transport.open();transport.message({type:'auth_ok'});
    // Two independent current listeners legitimately share this telemetry topic.
    window.__f50Shared=[[],[]];
    subscriptionManager.subscribe('system.memory',msg=>__f50Shared[0].push(msg.data.internal.free),{interval:2345});
    subscriptionManager.subscribe('system.memory',msg=>__f50Shared[1].push(msg.data.internal.free),{interval:2345});
    return {count:__sockets.length,topics:[...subscriptionManager.subscriptionParams].map(([topic,params])=>({type:'subscribe',topic,params}))};
   });
   const topicFrames=()=>page.evaluate(()=>ws.ws.sent.filter(message=>message.type==='subscribe'&&message.topic));
   assert.deepEqual(await topicFrames(),initial.topics);
   const dashboard=usage=>page.evaluate(usage=>ws.ws.message({type:'data',topic:'system.dashboard',data:{cpu:{cores:[{id:0,usage},{id:1,usage}],total_usage:usage}},timestamp:1}),usage);
   await dashboard(13);await page.waitForFunction(()=>document.getElementById('cpu-avg')?.textContent==='13');
   assert.equal(await page.locator('#cpu-avg').isVisible(),true);
   for (const [round,usage] of [37,52].entries()) {
    await page.evaluate(()=>ws.ws.emitClose());
    // Let the production onclose timer create the next transport itself.
    await page.waitForFunction(count=>__sockets.length===count,initial.count+round+1);
    const connection=await page.evaluate(()=>{
     const transport=ws.ws;transport.open();transport.message({type:'auth_ok'});
     return {token:transport.sent[0].token,identity:__f50Identity,samePage:document.getElementById('cpu-avg')===__f50Cpu};
    });
    assert.equal(connection.token,connection.identity);assert.equal(connection.samePage,true);
    assert.deepEqual(await topicFrames(),initial.topics,'each current topic is restored once with its original params');
    await dashboard(usage);await page.waitForFunction(value=>document.getElementById('cpu-avg')?.textContent===String(value),usage);
    assert.equal(await page.locator('#cpu-avg').isVisible(),true);
   }
   const beforeAway=await page.evaluate(()=>{const count=__sockets.length;ws.ws.emitClose();return count;});
   await page.locator('a.nav-link[href="#/network"]').click();
   await page.locator('.page-network').waitFor();
   assert.equal(await page.evaluate(()=>subscriptionManager.subscriptions.has('system.dashboard')),false);
   assert.equal(await page.evaluate(()=>__sockets.length),beforeAway,'leave the page before the pending reconnect timer fires');
   await page.waitForFunction(count=>__sockets.length===count,beforeAway+1);
   const away=await page.evaluate(()=>{
    const transport=ws.ws;transport.open();transport.message({type:'auth_ok'});
    transport.message({type:'data',topic:'system.memory',data:{internal:{total:1000,free:444,allocated:556}},timestamp:2});
    return {token:transport.sent[0].token,identity:__f50Identity,shared:__f50Shared,homeGone:!document.getElementById('cpu-avg')};
   });
   assert.equal(away.token,away.identity);assert.equal(away.homeGone,true);assert.deepEqual(away.shared,[[444],[444]]);
   assert.deepEqual(await topicFrames(),[{type:'subscribe',topic:'system.memory',params:{interval:2345}}],'disposed home topic stays absent while shared listeners survive');
   await page.locator('a.nav-link[href="#/"]').click();
   await page.waitForFunction(()=>subscriptionManager.subscriptions.has('system.dashboard'));
   await dashboard(76);await page.waitForFunction(()=>document.getElementById('cpu-avg')?.textContent==='76');
   assert.equal(await page.locator('#cpu-avg').isVisible(),true);
   const final=await topicFrames();
   assert.equal(final.filter(message=>message.topic==='system.dashboard').length,1);
   assert.equal(final.filter(message=>message.topic==='system.memory').length,1);
   assert.deepEqual(final.find(message=>message.topic==='system.dashboard').params,{interval:1000});
   assert.equal(await page.evaluate(()=>__sockets.length),beforeAway+1);assert.deepEqual(errors,[]);
  } finally {await context.close();}
 });
}

// Deferred-eight F62: full production page, only storage/API boundaries delayed.
async function exportPage(language){
 const f=await pageFor(language),{page}=f;page.setDefaultTimeout(6000);
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
 await page.evaluate(async()=>{
  api.token='fixture-token';api.username='root';closeLoginModal();window.exportReads=[];window.exportRequests=[];window.exportHolds=false;window.listHolds=false;window.exportLists=[];
  const original=api.call.bind(api);
  api.call=async(name,params,method)=>{
   if(name==='config.pack.info')return {code:0,data:{can_export:true,device_type:'Developer'}};
   if(name==='config.pack.export'){exportRequests.push(structuredClone(params));return {code:0,data:{tscfg:'fixture',size:7,filename:'fixture.tscfg'}};}
   return original(name,params,method);
  };
  api.storageList=path=>{
   const entries=path.endsWith('/sub')?[{name:'D.json',type:'file',size:4}]:[{name:'sub',type:'dir'},...['A','B','C'].map(n=>({name:n+'.json',type:'file',size:4}))];
   if(listHolds)return new Promise(resolve=>exportLists.push({path,resolve}));
   return Promise.resolve({code:0,data:{entries}});
  };
  api.storageRead=path=>{
   const entry={path};exportReads.push(entry);
   if(exportHolds)return new Promise((resolve,reject)=>Object.assign(entry,{resolve,reject}));
   return Promise.resolve({code:0,data:{content:{file:path}}});
  };
  await loadSecurityPage();await refreshConfigPackStatus();
 });
 await page.locator('#btn-pack-export').click();await page.getByRole('checkbox',{name:'A.json',exact:true}).waitFor();
 return f;
}
const exportButton=(page,fn)=>page.locator(`#pack-export-modal button[onclick="${fn}()"]`);
async function flushExport(page,index,{error=false,version='old'}={}){
 await page.evaluate(({index,error,version})=>{
  const r=exportReads[index];if(error)r.reject(new Error('old read failed'));else r.resolve({code:0,data:{content:{version,file:r.path}}});
 },{index,error,version});
 await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
}
async function generateExport(page){
 await page.locator('#pack-export-name').fill('fixture');await page.locator('#pack-export-recipient-cert').fill('fixture recipient certificate');
 await page.locator('#btn-pack-export-generate').click();await page.waitForFunction(()=>exportRequests.length>0);
 return page.evaluate(()=>exportRequests.at(-1));
}
for(const language of ['zh-CN','en-US']){
 test(language+': F62 ordinary single/multiple, deselect B, cross-directory accumulation and real export payload',async()=>{
  const f=await exportPage(language);try{const {page}=f;
   await page.getByRole('checkbox',{name:'A.json',exact:true}).check();await page.getByRole('checkbox',{name:'B.json',exact:true}).check();
   await page.getByRole('checkbox',{name:'B.json',exact:true}).uncheck();
   let request=await generateExport(page);assert.deepEqual(request.content,{file:'/sdcard/config/A.json'});
   await page.locator('#pack-export-file-list [data-name="sub"]').click();await page.getByRole('checkbox',{name:'D.json',exact:true}).check();
   request=await generateExport(page);assert.deepEqual(request.content,{_batch:true,_files:{A:{file:'/sdcard/config/A.json'},D:{file:'/sdcard/config/sub/D.json'}}});
   await exportButton(page,'hideConfigPackExportModal').click();await page.locator('#btn-pack-export').click();
   assert.equal(await page.getByRole('checkbox',{name:'A.json',exact:true}).isChecked(),false);assert.equal(await page.locator('#btn-pack-export-generate').isDisabled(),true);
   await exportButton(page,'packExportSelectAll').click();await page.waitForFunction(()=>!document.getElementById('btn-pack-export-generate').disabled);
   request=await generateExport(page);assert.deepEqual(Object.keys(request.content._files),['A','B','C']);assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 for(const error of [false,true])test(language+': F62 cancelled single read ignores late '+(error?'error':'success'),async()=>{
  const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   await page.getByRole('checkbox',{name:'A.json',exact:true}).check();await page.waitForFunction(()=>exportReads.length===1);
   await page.getByRole('checkbox',{name:'A.json',exact:true}).uncheck();await flushExport(page,0,{error});
   assert.equal(await page.getByRole('checkbox',{name:'A.json',exact:true}).isChecked(),false);
   assert.equal(await page.locator('#pack-export-selected').isVisible(),false);assert.equal(await page.locator('#btn-pack-export-generate').isDisabled(),true);assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(language+': F62 same-path reselection accepts only the new read, including late failure',async()=>{
  for(const error of [false,true]){const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   const cb=page.getByRole('checkbox',{name:'A.json',exact:true});await cb.check();await cb.uncheck();await cb.check();await page.waitForFunction(()=>exportReads.length===2);
   await flushExport(page,1,{version:'new'});await flushExport(page,0,{error});
   assert.equal(await cb.isChecked(),true);const request=await generateExport(page);assert.equal(request.content.version,'new');assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}}
 });
 for(const operation of ['deselect','close','page'])test(language+': F62 select-all stops after '+operation+'; old read cannot affect reopened selection',async()=>{
  const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   await exportButton(page,'packExportSelectAll').click();await page.waitForFunction(()=>exportReads.length===1);
   if(operation==='deselect')await exportButton(page,'packExportDeselectAll').click();
   else {await exportButton(page,'hideConfigPackExportModal').click();if(operation==='page'){
    await page.locator('a[href="#/files"]').click();await page.waitForFunction(()=>!document.getElementById('pack-export-modal'));
    await page.locator('a[href="#/security"]').click();await page.waitForFunction(()=>document.getElementById('btn-pack-export')&&!document.getElementById('btn-pack-export').disabled);
   }await page.locator('#btn-pack-export').click();}
   await flushExport(page,0);assert.deepEqual(await page.evaluate(()=>exportReads.map(r=>r.path)),['/sdcard/config/A.json']);
   assert.equal(await page.getByRole('checkbox',{name:'A.json',exact:true}).isChecked(),false);assert.equal(await page.locator('#btn-pack-export-generate').isDisabled(),true);
   await page.evaluate(()=>exportHolds=false);await page.getByRole('checkbox',{name:'B.json',exact:true}).check();
   const request=await generateExport(page);assert.deepEqual(request.content,{file:'/sdcard/config/B.json'});assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(language+': F62 select-all snapshots paths while browsing, and stale directory success/error cannot replace current list',async()=>{
  const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   await exportButton(page,'packExportSelectAll').click();await page.waitForFunction(()=>exportReads.length===1);
   await page.locator('#pack-export-file-list [data-name="sub"]').click();await page.getByRole('checkbox',{name:'D.json',exact:true}).waitFor();
   for(let i=0;i<3;i++){await page.waitForFunction(n=>exportReads.length>n,i);await flushExport(page,i);}
   assert.deepEqual(await page.evaluate(()=>exportReads.map(r=>r.path)),['/sdcard/config/A.json','/sdcard/config/B.json','/sdcard/config/C.json']);
   const request=await generateExport(page);assert.deepEqual(Object.keys(request.content._files),['A','B','C']);
   for(const error of [false,true]){
    await page.evaluate(()=>listHolds=true);await exportButton(page,'packExportBrowseRefresh').click();
    await page.waitForFunction(()=>exportLists.length>0);
    await page.evaluate(()=>listHolds=false);await exportButton(page,'packExportBrowseUp').click();await page.getByRole('checkbox',{name:'A.json',exact:true}).waitFor();
    await page.evaluate(error=>{const old=exportLists.shift();old.resolve(error?{code:7,error:'old list failure'}:{code:0,data:{entries:[{name:'STALE.json',type:'file'}]}});},error);
    await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
    assert.equal(await page.getByRole('checkbox',{name:'A.json',exact:true}).isVisible(),true);assert.equal(await page.getByText('STALE.json',{exact:true}).count(),0);
   }
   assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(language+': F72 memory estimates, real HWM, absent versus numeric zero, sorting and reopening',async()=>{
  const f=await pageFor(language);try{const {page}=f;await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{closeLoginModal();const original=api.call.bind(api);api.call=(name,...args)=>name==='ui.widgets.get'?Promise.resolve({code:0,data:{widgets:[],source:'sdcard',refresh_interval:5000}}):original(name,...args);window.memoryReads=0;api.getMemoryDetail=async()=>{++memoryReads;return {code:0,data:{stack_allocation_estimated:true,dram:{total:4096,free:1024,used:3072,used_percent:75},tasks:[{name:'missing',stack_hwm:333,priority:1,state:'Ready'},{name:'zero',stack_alloc:0,stack_used:0,stack_usage_pct:0,stack_hwm:600,priority:2,state:'Ready'},{name:'used',stack_alloc:2048,stack_used:1024,stack_usage_pct:50,stack_hwm:1024,priority:3,state:'Blocked'}],total_stack_allocated:2048,task_count:3}};};await loadSystemPage();});
   const detail=page.locator('button[onclick="showMemoryDetailModal()"]');await detail.click();
   const modal=page.locator('#memory-detail-modal');await page.locator('#task-memory-table').waitFor();
   for(const key of ['stack_alloc','stack_used','stack_usage_pct'])assert.match(await page.locator(`th[data-sort="${key}"]`).innerText(),language==='zh-CN'?/估算/:/est\./);
   assert.doesNotMatch(await page.locator('th[data-sort="stack_hwm"]').innerText(),/估算|est\./);
   assert.match(await modal.innerText(),language==='zh-CN'?/测得的最低剩余量/:/measured minimum/);
   const row=name=>page.locator('#task-table-body tr').filter({has:page.locator('td:first-child',{hasText:new RegExp('^'+name+'$')})});
   assert.deepEqual(await row('missing').locator('td').allTextContents().then(c=>c.slice(1,5)),['-','-','333 B','-']);
   assert.deepEqual(await row('zero').locator('td').allTextContents().then(c=>c.slice(1,5)),['0 B','0 B','600 B','0%']);
   await page.locator('th[data-sort="stack_hwm"]').click();assert.match(await modal.innerText(),language==='zh-CN'?/测得的最低剩余量/:/measured minimum/);
   await modal.locator('button[onclick="hideMemoryDetailModal()"]').click();await detail.click();await page.locator('#task-memory-table').waitFor();assert.equal(await page.evaluate(()=>memoryReads),2);
   assert.deepEqual(f.errors,[]);
   if(process.env.TS_TEST_EVIDENCE_DIR){
    const close=page.locator('#toast .toast-close');if(await close.isVisible())await close.click();
    await modal.getByText(/测得的最低剩余量|measured minimum/).scrollIntoViewIfNeeded();
    await page.screenshot({path:path.join(process.env.TS_TEST_EVIDENCE_DIR,`F72-${language}.png`)});
   }
  }finally{await f.context.close();}
 });
}
for(const language of ['zh-CN','en-US']){
 test(language+': F62 current read errors remain visible; all-failed blocks export and partial success exports only readable content',async()=>{
  const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   await page.getByRole('checkbox',{name:'A.json',exact:true}).check();await flushExport(page,0,{error:true});
   assert.equal(await page.getByRole('checkbox',{name:'A.json',exact:true}).isChecked(),true);
   assert.match(await page.locator('#pack-export-selected').getAttribute('class'),/error/);
   assert.equal(await page.locator('#btn-pack-export-generate').isDisabled(),true);assert.equal(await page.evaluate(()=>exportRequests.length),0);
   await page.getByRole('checkbox',{name:'B.json',exact:true}).check();await flushExport(page,1,{version:'readable'});
   const request=await generateExport(page);assert.deepEqual(request.content,{version:'readable',file:'/sdcard/config/B.json'});assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
 test(language+': F62 cancelled select-directory operation never shows its late completion toast',async()=>{
  const f=await exportPage(language);try{const {page}=f;await page.evaluate(()=>exportHolds=true);
   await exportButton(page,'packExportSelectDir').click();await page.waitForFunction(()=>exportReads.length===1);
   await exportButton(page,'packExportDeselectAll').click();await flushExport(page,0);
   assert.equal(await page.evaluate(()=>exportReads.length),1);assert.equal(await page.locator('#btn-pack-export-generate').isDisabled(),true);
   assert.equal(await page.evaluate(()=>(document.getElementById('toast')?.textContent||'').includes(t('toast.selectedJsonFiles'))),false);assert.deepEqual(f.errors,[]);
  }finally{await f.context.close();}
 });
}
