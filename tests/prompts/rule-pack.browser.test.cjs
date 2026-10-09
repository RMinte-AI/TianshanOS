// Real page and file picker, local simulated device. Cryptography/storage are verified separately by production C tests.
const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const root=path.resolve(__dirname,'../../components/ts_webui/web');
let browser;
before(async()=>{browser=await chromium.launch({channel:'chrome',headless:true});});
after(async()=>{await browser?.close();});
const hash='a'.repeat(64);
const id='night\'"<&>';
const preview={valid:true,trusted:true,target_matches:true,type:'automation_rule',id,name:'Night <b>test</b>',
 signer:'Test Developer',official:true,exists:false,expected_revision:0,expected_generation:7,credential_generation:4,
 package_digest:hash,rule:{id,name:'Night <b>test</b>',conditions:[{variable:'future.sample'}],actions:[{type:'log',message:'Test'}]},
 warnings:['dynamic_inputs']};
async function pageFor(language,mode,run){
 const calls=[];let saved=false,writes=0,sourceEnabled=false;
 const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://local');
  if(url.pathname.startsWith('/api/v1/')){
   let raw='';req.on('data',part=>raw+=part);
   req.on('end',()=>{
    const endpoint=url.pathname.slice(8);
    const params=req.method==='GET'?Object.fromEntries(url.searchParams):(raw?JSON.parse(raw):{});
    calls.push({endpoint,params});
    let reply={code:0,data:{}};
    if(endpoint==='automation/sources/list')reply={code:0,data:{sources:[{id:'source-only',label:'Source',enabled:sourceEnabled,type:'ssh'}]}};
    if(endpoint==='automation/sources/enable'||endpoint==='automation/sources/disable'){
     assert.deepEqual(params,{id:'source-only'});sourceEnabled=endpoint.endsWith('/enable');
    }
    if(endpoint==='automation/rules/get')reply={code:1,message:'not_found'};
    if(endpoint==='automation/rules/import'){
     if(params.preview){
      const data={...preview,exists:mode==='old',expected_revision:mode==='old'?4:0};
      if(params.tscfg==='untrusted')data.trusted=false;
      if(params.tscfg==='slow'||params.tscfg==='fast')data.rule={...preview.rule,name:params.tscfg};
      reply={code:0,data};
      if(params.tscfg==='slow'){
       setTimeout(()=>{res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(reply));},150);return;
      }
     }
     else{
      ++writes;
      assert.equal(params.expected_revision,mode==='old'?4:0);
      assert.equal(params.expected_generation,7);
      assert.equal(params.credential_generation,4);
      assert.equal(params.package_digest,hash);
      if(mode==='conflict')reply={code:1,message:'revision_conflict'};
      else{
       saved=true;
       if(mode==='lost'||mode==='unknown'){
        res.writeHead(200,{'Content-Type':'application/json'});res.flushHeaders();
        res.write('{"code":0,');setImmediate(()=>res.destroy());return;
       }
       reply={code:0,data:{id,saved:true,durable:true,runtime_applied:false,restart_required:true,saved_revision:1,saved_generation:8}};
      }
     }
    }
    if(endpoint==='automation/rules/list')reply={code:0,data:{loaded:true,recovery_required:mode==='unknown',rules:saved?[{id,name:'Night <b>test</b>',enabled:false,
      readonly:true,restart_required:true,runtime_active:mode==='old',saved_revision:mode==='old'?5:1,active_revision:mode==='old'?4:0,saved_generation:8,
      package_digest:hash,conditions_count:1,actions_count:1}]:[]}};
    res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(reply));
   });return;
  }
  const file=path.resolve(root,'.'+(url.pathname==='/'?'/index.html':url.pathname));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(error,data)=>{if(error){res.writeHead(404).end();return;}
   res.setHeader('Content-Type',({'.js':'application/javascript','.css':'text/css','.html':'text/html','.svg':'image/svg+xml','.woff2':'font/woff2'})[path.extname(file)]||'application/octet-stream');
   res.end(data);
  });
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const context=await browser.newContext({locale:language});
 await context.addInitScript(language=>{
  localStorage.setItem('ts_language',language);
  class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}
  window.WebSocket=Socket;
 },language);
 const page=await context.newPage(),errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 try{
  await page.goto('http://127.0.0.1:'+server.address().port);
  await page.waitForFunction(()=>i18n.isReady());
  await page.evaluate(()=>{closeLoginModal();document.body.insertAdjacentHTML('beforeend','<div id="rules-list"></div>');showImportRuleModal();});
  await page.locator('#import-rule-file').setInputFiles({name:'renamed-package.tscfg',mimeType:'application/json',buffer:Buffer.from('local page fixture')});
  await page.waitForFunction(()=>!document.getElementById('import-rule-btn').disabled);
  assert.equal(await page.locator('#import-rule-preview b').count(),0,'the signed name must remain text');
  assert.match(await page.locator('#import-rule-preview').innerText(),/Night <b>test<\/b>/);
  await run({page,calls,writes:()=>writes,saved:()=>saved});
  assert.deepEqual(errors,[]);
 }finally{await context.close();await new Promise(resolve=>server.close(resolve));}
}
for(const language of ['zh-CN','en-US']){
 test(language+': preview -> one save -> pending row and escaped ID',async()=>{
  await pageFor(language,'ok',async({page,calls,writes,saved})=>{
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.getElementById('import-rule-result').textContent===t('rulePack.saved'));
   await page.waitForFunction(()=>document.querySelector('#rules-list .tr:not(.th)'));
   assert(saved());assert.equal(writes(),1);
   const buttons=page.locator('#rules-list .tr:not(.th) button');
   for(let i=0;i<await buttons.count();++i)assert(await buttons.nth(i).isDisabled());
   assert.equal(await page.locator('#rules-list .tr:not(.th) .mono').innerText(),id);
   assert.equal(await page.locator('#rules-list b').count(),0);
   assert(calls.some(c=>c.endpoint==='automation/rules/list'));
  });
 });
 test(language+': a lost save reply is observed without repeating the write',async()=>{
  await pageFor(language,'lost',async({page,writes})=>{
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.getElementById('import-rule-result').textContent===t('rulePack.saved'));
   assert.equal(writes(),1);
  });
 });
 test(language+': saved generation conflict requires a fresh preview',async()=>{
  await pageFor(language,'conflict',async({page,writes,saved})=>{
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.getElementById('import-rule-result').textContent===t('rulePack.errors.revision_conflict'));
   assert.equal(writes(),1);assert.equal(saved(),false);
   assert(await page.locator('#import-rule-btn').isDisabled());
  });
 });
 test(language+': pending replacement retains only current trigger and export controls',async()=>{
  await pageFor(language,'old',async({page,writes})=>{
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.querySelector('#rules-list .tr:not(.th)'));
   const buttons=page.locator('#rules-list .tr:not(.th) button');
   for(const [index,disabled] of [[0,true],[1,false],[2,true],[3,false],[4,true]])assert.equal(await buttons.nth(index).isDisabled(),disabled);
   assert.match(await buttons.nth(1).getAttribute('title'),language==='zh-CN'?/当前|旧/:/older/);
   assert.equal(await buttons.nth(3).getAttribute('title'),await page.evaluate(()=>t('rulePack.exportCurrent')));
   assert.equal(writes(),1);
  });
 });
 test(language+': recovery failure cannot confirm an uncertain save',async()=>{
  await pageFor(language,'unknown',async({page,calls,writes})=>{
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.getElementById('import-rule-result').textContent===t('rulePack.unknown'));
   await page.waitForTimeout(100);
   assert(calls.some(c=>c.endpoint==='automation/rules/list'));assert.equal(writes(),1);
   assert.equal(await page.locator('#import-rule-result').innerText(),await page.evaluate(()=>t('rulePack.unknown')));
   assert(await page.locator('#import-rule-btn').isDisabled());
  });
 });
 test(language+': out-of-order previews cannot replace the newest selected bytes',async()=>{
  await pageFor(language,'ok',async({page,calls,writes})=>{
   await page.locator('#import-rule-file').setInputFiles({name:'slow.tscfg',buffer:Buffer.from('slow')});
   await page.waitForTimeout(20);
   await page.locator('#import-rule-file').setInputFiles({name:'fast.tscfg',buffer:Buffer.from('fast')});
   await page.waitForFunction(()=>!document.getElementById('import-rule-btn').disabled);
   await page.waitForTimeout(200);
   assert.equal(await page.evaluate(()=>rulePackPreview.rule.name),'fast');
   await page.locator('#import-rule-btn').click();
   await page.waitForFunction(()=>document.getElementById('import-rule-result').textContent===t('rulePack.saved'));
   assert.equal(calls.find(c=>c.endpoint==='automation/rules/import'&&!c.params.preview).params.tscfg,'fast');assert.equal(writes(),1);
  });
 });
 test(language+': an untrusted preview cannot enable import',async()=>{
  await pageFor(language,'ok',async({page,writes})=>{
   await page.locator('#import-rule-file').setInputFiles({name:'untrusted.tscfg',buffer:Buffer.from('untrusted')});
   await page.waitForFunction(()=>document.getElementById('import-rule-result').classList.contains('error'));
   assert(await page.locator('#import-rule-btn').isDisabled());assert.equal(writes(),0);
  });
 });
 test(language+': datasource switch works without a same-name rule',async()=>{
  await pageFor(language,'ok',async({page,calls})=>{
   await page.evaluate(async()=>{hideImportRuleModal();document.body.insertAdjacentHTML('beforeend','<div id="sources-list"></div>');await refreshSources();});
   const button=page.locator('#sources-list button[role="switch"]');
   await button.click();await page.waitForFunction(()=>document.querySelector('#sources-list button[role="switch"]').getAttribute('aria-checked')==='true');
   await button.click();await page.waitForFunction(()=>document.querySelector('#sources-list button[role="switch"]').getAttribute('aria-checked')==='false');
   assert.equal(calls.filter(c=>c.endpoint==='automation/sources/enable').length,1);
   assert.equal(calls.filter(c=>c.endpoint==='automation/sources/disable').length,1);
   assert.equal(calls.filter(c=>c.endpoint==='automation/rules/get').length,0);
  });
 });
}
