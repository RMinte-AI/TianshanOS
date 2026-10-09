const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let server,browser,base;
before(async()=>{
 server=http.createServer((req,res)=>{
  const rel=new URL(req.url,'http://local').pathname;
  const file=path.resolve(root,'.'+(rel==='/'?'/index.html':rel));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(error,data)=>{if(error){res.writeHead(404).end();return;}res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css','.woff2':'font/woff2'})[path.extname(file)]||'application/octet-stream');res.end(data);});
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
after(async()=>{await browser?.close();await new Promise(resolve=>server?.close(resolve));});
for(const entry of ['action','command'])for(const state of ['stopped','ready'])test(`${entry}: real REST verification gates ${state} deletion`,async()=>{
 const context=await browser.newContext();
 await context.addInitScript(()=>{localStorage.setItem('ts_language','zh-CN');class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;});
 const page=await context.newPage(),calls=[];
 await page.route('**/*',async route=>{
  const req=route.request(),url=new URL(req.url());
  if(url.origin!==base){await route.abort();return;}
  if(!url.pathname.startsWith('/api/')){await route.continue();return;}
  const endpoint=url.pathname.slice('/api/v1/'.length),params=req.method()==='GET'?Object.fromEntries(url.searchParams):req.postDataJSON();
  calls.push({endpoint,method:req.method(),params});let data={};
  if(endpoint==='automation/actions/get')data={type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model'}};
  if(endpoint==='ssh/commands/get')data={id:'model',nohup:true,serviceMode:true};
  if(endpoint==='automation/services/status')data=req.method()==='POST'?{operation_id:31}:{operation_id:31,operation_phase:'succeeded',state,busy:false};
  await route.fulfill({json:{code:0,data}});
 });
 try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&typeof deleteAction==='function');
  await page.evaluate(()=>{confirmAction=async()=>true;refreshActions=async()=>{};window.commandRefreshes=0;refreshCommandsList=()=>{window.commandRefreshes++;};window.deleteNavigationCalls=0;const navigate=router.navigate.bind(router);router.navigate=(...args)=>{window.deleteNavigationCalls++;return navigate(...args);};selectedHostId='host';sshCommands={host:[{id:'model',name:'Model'}]};});
  const deletion=page.evaluate(entry=>entry==='action'?deleteAction('template'):deleteCommand(0),entry);
  if(state==='ready'){
   await page.locator('.confirm-sheet').waitFor();assert((await page.locator('.confirm-sheet').textContent()).includes('仍在运行'));
   if(entry==='command'){
    assert.equal(await page.locator('.confirm-sheet button').count(),1);
    assert.equal(await page.locator('.confirm-sheet button').textContent(),await page.evaluate(()=>t('common.close')));
    const before=await page.evaluate(()=>({nav:window.deleteNavigationCalls,refresh:window.commandRefreshes,hash:location.hash}));
    await page.locator('.confirm-sheet button').click();await deletion;
    assert.deepEqual(await page.evaluate(()=>({nav:window.deleteNavigationCalls,refresh:window.commandRefreshes,hash:location.hash})),before);
   }else await page.keyboard.press('Escape');
  }
  await deletion;
  const deletes=calls.filter(c=>c.endpoint===('action'===entry?'automation/actions/delete':'ssh/commands/remove'));
  assert.equal(deletes.length,state==='stopped'?1:0);
  const verify=calls.find(c=>c.endpoint==='automation/services/status'&&c.method==='POST');assert.equal(verify.params.verify,true);assert.equal(verify.params.command_id,'model');
  assert(!calls.some(c=>c.endpoint==='automation/services/stop'||c.endpoint==='ssh/services/start'));
  if(state==='ready')assert.equal(await page.locator('.confirm-sheet').count(),0);
 }finally{await context.close();}
});
for(const entry of ['action','command'])test(`${entry}: delayed verification cannot overwrite a newer real start`,async()=>{
 const context=await browser.newContext();
 await context.addInitScript(()=>{localStorage.setItem('ts_language','zh-CN');class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;});
 const page=await context.newPage(),calls=[];let release,entered,reads=0;const gate=new Promise(r=>entered=r);
 await page.route('**/*',async route=>{
  const req=route.request(),url=new URL(req.url());
  if(url.origin!==base){await route.abort();return;}
  if(!url.pathname.startsWith('/api/')){await route.continue();return;}
  const endpoint=url.pathname.slice('/api/v1/'.length);calls.push({endpoint,method:req.method()});let data={};
  if(endpoint==='automation/actions/get')data={type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model'}};
  if(endpoint==='ssh/commands/get')data={id:'model',nohup:true,serviceMode:true};
  if(endpoint==='ssh/services/start')data={operation_id:32};
  if(endpoint==='automation/services/status'){
   if(req.method()==='POST')data={operation_id:31};
   else if(++reads===1){data={operation_id:31,operation_phase:'succeeded',state:'stopped',busy:false};entered();await new Promise(r=>release=r);}
   else data={operation_id:32,operation_kind:'start',operation_phase:'queued',state:'starting',busy:false};
  }
  await route.fulfill({json:{code:0,data}});
 });
 try{
  await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&typeof deleteAction==='function');
  await page.evaluate(()=>{
   confirmAction=async()=>true;refreshActions=async()=>{};refreshCommandsList=()=>{};
   selectedHostId='host';sshCommands={host:[{id:'model',name:'Model',nohup:true,serviceMode:true}]};
   document.getElementById('page-content').innerHTML='<div class="service-mode-status"><span class="service-status" data-command="model"></span></div><pre id="exec-result"></pre>';
  });
  const deletion=page.evaluate(entry=>entry==='action'?deleteAction('template'):deleteCommand(0),entry);
  await gate;
  await page.evaluate(()=>executeManagedService(sshCommands.host[0],document.getElementById('exec-result')));
  const label=await page.locator('.service-status').textContent();assert(label.length>0);
  release();await deletion;
  assert.equal(await page.locator('.service-status').textContent(),label);
  assert(!calls.some(c=>c.endpoint==='automation/actions/delete'||c.endpoint==='ssh/commands/remove'));
 }finally{release?.();await context.close();}
});
