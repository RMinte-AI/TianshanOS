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
   class Socket {static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}
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
    return {preserved,disabled,history:document.getElementById('config-pack-task-results')?.textContent};
   });assert(r.preserved&&r.disabled);assert(r.history.includes('older failure'));
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
    return {paths,result,list:document.getElementById('file-list').textContent,summary:document.getElementById('operation-summary').textContent};
   });assert.deepEqual(r.paths.slice(0,2),['/sdcard/a/a.txt','/sdcard/a/b.txt']);assert.equal(r.result.success,1);assert.equal(r.result.failures.length,0);assert(r.list.includes('read failed'));
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
