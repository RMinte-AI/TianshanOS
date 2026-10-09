const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let browser,server,base,state;
const send=(res,value)=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(value));};
before(async()=>{
 server=http.createServer((req,res)=>{
  const u=new URL(req.url,'http://local');
  if(u.pathname.startsWith('/api/')){
   if(u.pathname==='/api/v1/key/generate'){
    let body='';req.on('data',part=>body+=part);req.on('end',async()=>{
     const params=JSON.parse(body);state.posts.push(params);state.release=()=>{
      if(state.failure==='json'){res.end('{broken');return;}
      if(state.failure==='body'){res.writeHead(200,{'Content-Type':'application/json','Content-Length':'1000'});res.flushHeaders();res.write('{');setTimeout(()=>res.destroy(),50);return;}
      if(state.failure==='response-no-message'){send(res,{code:6});return;}
      if(state.failure==='business'){send(res,{code:7,error:'key_storage_full',data:{failed_stage:'metadata_write',esp_error:'ESP_ERR_NVS_NOT_ENOUGH_SPACE',cleanup_complete:false,cleanup_error:'ESP_FAIL'}});return;}
      state.keys.push({id:params.id,type:({ec256:'ecdsa-p256',ec384:'ecdsa-p384'})[params.type]||params.type});
      send(res,{code:0,data:{generated:true,id:params.id,type:state.keys.at(-1).type}});
     };
     if(!state.hold)state.release();
    });return;
   }
   if(u.pathname==='/api/v1/key/list')return send(res,state.listFail?{code:7,error:'list failed'}:{code:0,data:{keys:state.keys}});
   if(u.pathname==='/api/v1/key/info'){state.checks++;return send(res,{code:0,data:{id:u.searchParams.get('id')}});}
   if(u.pathname==='/api/v1/cert/status')return send(res,{code:0,data:{has_private_key:true,has_certificate:true,cert_info:{subject_cn:'fixture'}}});
   if(u.pathname==='/api/v1/ssh/hosts/list'||u.pathname==='/api/v1/hosts/list')return send(res,{code:0,data:{hosts:[]}});
   return send(res,{code:0,data:{}});
  }
  const file=path.resolve(root,'.'+(u.pathname==='/'?'/index.html':u.pathname));if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css'})[path.extname(file)]||'application/octet-stream');res.end(data);});
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
after(async()=>{await browser?.close();await new Promise(r=>server?.close(r));});
async function open(lang){
 state={posts:[],keys:[],checks:0};const context=await browser.newContext({locale:lang,viewport:{width:1280,height:900}});
 await context.addInitScript(lang=>{localStorage.setItem('ts_language',lang);class Socket{static OPEN=1;constructor(){this.readyState=0;}send(){}close(){}}window.WebSocket=Socket;},lang);
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady());await page.evaluate(async()=>{closeLoginModal();await loadSecurityPage();showGenerateKeyModal();});
 await page.locator('#keygen-id').fill('fixture');return {page,context,errors};
}
const done=page=>page.waitForFunction(()=>api.keyGenerationTrace?.events.some(e=>e.stage==='ui_done'));
for(const lang of ['zh-CN','en-US']){
 for(const type of ['rsa2048','rsa4096','ec256','ec384'])test(`${lang} ${type}: real modal persistent feedback and generated row`,async()=>{
  const {page,context,errors}=await open(lang);try{
   state.hold=true;await page.locator('#keygen-type').selectOption(type);await page.locator('#keygen-submit').click();await page.waitForFunction(()=>api.keyGenerationTrace?.events.some(e=>e.stage==='fetch_start'));
   await new Promise(r=>setTimeout(r,3200));assert(await page.locator('#keygen-status').isVisible());assert((await page.locator('#keygen-status').textContent()).includes('fixture'));assert(await page.locator('#keygen-submit').isDisabled());
   await page.evaluate(()=>generateKey());assert.equal(state.posts.length,1);state.release();await done(page);
   assert(await page.locator('#keygen-modal').evaluate(e=>e.classList.contains('hidden')));assert((await page.locator('#toast').getAttribute('class')).includes('toast-success'));
   assert((await page.locator('#keys-table-body').textContent()).includes('fixture'));assert((await page.locator('#keys-table-body').textContent()).includes('HTTPS'));assert.match(state.posts[0].request_id,/^kg-/);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 test(`${lang}: business and transport failures retain modal with accurate state`,async()=>{
  for(const failure of ['business','json','body','response-no-message']){
   const {page,context,errors}=await open(lang);try{state.failure=failure;await page.locator('#keygen-submit').click();await done(page);
    assert(await page.locator('#keygen-modal').isVisible());assert.equal(await page.locator('#keygen-id').inputValue(),'fixture');assert(await page.locator('#keygen-submit').isEnabled());assert.equal(state.posts.length,1);
    const status=await page.locator('#keygen-status').textContent();if(failure==='business'){assert(status.includes('metadata_write'));assert(status.includes('ESP_FAIL'));assert.equal(state.checks,0);}else{assert.equal(state.checks,1);assert.equal(status,await page.evaluate(()=>t('keyGeneration.recordFound')));}
    assert.deepEqual(errors,[]);
   }finally{await context.close();}
  }
 });
 test(`${lang}: list error after saving never reports generation failure`,async()=>{
  const {page,context,errors}=await open(lang);try{state.listFail=true;await page.locator('#keygen-submit').click();await done(page);assert(await page.locator('#keygen-modal').evaluate(e=>e.classList.contains('hidden')));assert.equal(await page.locator('#toast').textContent(),await page.evaluate(()=>t('keyGeneration.refreshFailed')));assert.equal(state.posts.length,1);assert.deepEqual(errors,[]);}finally{await context.close();}
 });
 test(`${lang}: closed and reopened modal owns its new inputs`,async()=>{
  const {page,context,errors}=await open(lang);try{state.hold=true;await page.locator('#keygen-submit').click();await page.waitForFunction(()=>api.keyGenerationTrace?.events.some(e=>e.stage==='fetch_start'));
   await page.locator('#keygen-close').click();await page.evaluate(()=>showGenerateKeyModal());await page.locator('#keygen-id').fill('fresh');state.release();await done(page);
   assert(await page.locator('#keygen-modal').isVisible());assert.equal(await page.locator('#keygen-id').inputValue(),'fresh');assert(await page.locator('#keygen-submit').isEnabled());assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}
for(const lang of ['zh-CN','en-US'])test(`${lang}: language rebuild releases only controls attached to the pending generation`,async()=>{
 const {page,context,errors}=await open(lang);try{
  await page.evaluate(()=>{api.token='fixture';api.level='root';window.oldModal=document.getElementById('keygen-modal');return router.navigate('/security');});
  await page.waitForFunction(()=>document.getElementById('keygen-modal')&&document.getElementById('keygen-modal')!==oldModal);
  await page.evaluate(()=>{showGenerateKeyModal();window.originalModal=document.getElementById('keygen-modal');});await page.locator('#keygen-id').fill('fixture');state.hold=true;state.failure='business';await page.locator('#keygen-submit').click();await page.waitForFunction(()=>api.keyGenerationTrace?.events.some(e=>e.stage==='fetch_start'));
  await page.evaluate(lang=>selectLanguage(lang),lang==='zh-CN'?'en-US':'zh-CN');
  await page.waitForFunction(()=>document.getElementById('keygen-modal')&&document.getElementById('keygen-modal')!==originalModal);await page.evaluate(()=>showGenerateKeyModal());await page.locator('#keygen-id').fill('fresh');assert(await page.locator('#keygen-submit').isDisabled());
  state.release();await done(page);assert(await page.locator('#keygen-submit').isEnabled());assert(await page.locator('#keygen-modal').isVisible());assert.equal(await page.locator('#keygen-id').inputValue(),'fresh');assert.equal(await page.locator('#keygen-status').textContent(),'');assert.deepEqual(errors,[]);
 }finally{await context.close();}
});
