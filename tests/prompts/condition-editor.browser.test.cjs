const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
const samples=[
 ['ready','ready',4],['numeric-string','0',4],['boolean-string','true',4],['empty-string','',4],
 ['quotes-and-escapes','a"b\'\\c<&>\n\t',4],['entity-literal','&quot;ready&quot;',4],['spaces',' ready ',4],
 ['zero',0,2],['negative',-42,2],['fraction',1.25,3],['integral-float',1,3],
 ['true',true,1],['false',false,1],['null',null,0],['limit-string','a'.repeat(63),4],['unicode','中'.repeat(20),4]
];
let server,browser,base;const roundtrips=[];
before(async()=>{
 server=http.createServer((req,res)=>{
  const rel=new URL(req.url,'http://local').pathname,file=path.resolve(root,'.'+(rel==='/'?'/index.html':rel));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css','.woff2':'font/woff2'})[path.extname(file)]||'application/octet-stream');res.end(data);});
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
after(async()=>{
 await browser?.close();await new Promise(resolve=>server?.close(resolve));
 const dir=path.resolve('output/condition-editor');fs.mkdirSync(dir,{recursive:true});
 fs.writeFileSync(path.join(dir,'browser-roundtrips.json'),JSON.stringify(roundtrips,null,2));
});
function rule(value='ready',value_type=4,operator='ne'){
 const condition={variable:'model"\'&<>.status',operator,value,value_type};
 return {id:'fixture-rule',name:'Rule " & < >',icon:'ri-thunderstorms-line',revision:1,enabled:false,manual_trigger:false,show_on_dashboard:false,allow_manual_trigger:false,logic:'or',cooldown_ms:1234,
  conditions:[condition],actions:[{type:'log',template_id:'fixture-action',message:'fixture',delay_ms:17,async:false,repeat_mode:'count',repeat_count:3,repeat_interval_ms:250,condition:structuredClone(condition)}]};
}
async function fixture(original=rule(),language='zh-CN'){
 let stored=structuredClone(original);const writes=[];
 const context=await browser.newContext();await context.addInitScript(language=>{localStorage.setItem('ts_language',language);class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;},language);
 const page=await context.newPage();
 await page.route('**/*',async route=>{
  const req=route.request(),url=new URL(req.url());
  if(url.origin!==base){await route.abort();return;}
  if(url.pathname==='/js/app.js'&&process.env.CONDITION_EDITOR_BASELINE){await route.fulfill({contentType:'application/javascript',body:fs.readFileSync(process.env.CONDITION_EDITOR_BASELINE,'utf8')});return;}
  if(!url.pathname.startsWith('/api/')){await route.continue();return;}
  const endpoint=url.pathname.slice('/api/v1/'.length);let data={};
  if(endpoint==='automation/actions/list')data={templates:[{id:'fixture-action',name:'Action " & < >',type:'log'}]};
  if(endpoint==='automation/rules/get')data=stored;
  if(endpoint==='automation/rules/update'||endpoint==='automation/rules/add'){
   const params=req.postDataJSON();writes.push(structuredClone(params));stored={...stored,...params,revision:stored.revision+1};delete stored.expected_revision;data={revision:stored.revision,mirror_synced:true};
  }
  await route.fulfill({json:{code:0,data}});
 });
 await page.goto(base);await page.waitForFunction(()=>i18n.isReady()&&typeof showAddRuleModal==='function');
 const open=async()=>{
  await page.evaluate(async()=>{closeLoginModal();const r=await api.call('automation.rules.get',{id:'fixture-rule'});showAddRuleModal(r.data);});
  await page.waitForFunction(()=>document.getElementById('add-rule-modal')?.dataset.loading==='false');
 };
 await open();return {page,context,writes,stored:()=>structuredClone(stored),open,original};
}
for(const language of ['zh-CN','en-US'])for(const [name,value,type]of samples)test(`${language}: ${name} survives both editors and two saves`,async()=>{
 const f=await fixture(rule(value,type),language);
 try{
  for(let iteration=0;iteration<2;iteration++){
   assert.equal(await f.page.locator('.cond-value').inputValue(),JSON.stringify(value));
   assert.equal(await f.page.locator('.action-condition-value').inputValue(),JSON.stringify(value));
   await f.page.evaluate(()=>submitAddRule('fixture-rule'));
   assert.equal(f.writes.length,iteration+1);
   const written=f.writes.at(-1);
   assert.deepEqual(written.conditions,f.original.conditions);
   assert.deepEqual(written.actions,f.original.actions);
   for(const k of ['id','name','icon','enabled','manual_trigger','show_on_dashboard','allow_manual_trigger','logic','cooldown_ms'])assert.deepEqual(written[k],f.original[k],k);
   await f.open();
  }
  roundtrips.push({name:language+':'+name,original:f.original,saved:f.stored()});
 }finally{await f.context.close();}
});
for(const input of ['ready','"0"','0','"true"','true','false','null','""'])test(`editing ${input} uses the same type contract in both editors`,async()=>{
 const f=await fixture();try{
  await f.page.locator('.cond-value').fill(input);await f.page.locator('.action-condition-value').fill(input);
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,1);
  const w=f.writes[0];assert.deepEqual(w.conditions[0],w.actions[0].condition);
  let expected;try{expected=JSON.parse(input);}catch{expected=input;}
  assert.deepEqual(w.conditions[0].value,expected);
  const type=expected===null?0:typeof expected==='boolean'?1:typeof expected==='number'?2:4;
  assert.equal(w.conditions[0].value_type,type);
 }finally{await f.context.close();}
});
for(const kind of ['trigger','action'])for(const input of ['[]','{}','1e999','"\\u0000"',JSON.stringify('a'.repeat(64)),JSON.stringify('中'.repeat(22))])test(`${kind}: invalid ${input.slice(0,20)} stays in draft and never writes`,async()=>{
 const f=await fixture();try{
  const field=f.page.locator(kind==='trigger'?'.cond-value':'.action-condition-value');await field.fill(input);
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));
  assert.equal(f.writes.length,0);assert.equal(await field.inputValue(),input);
  assert.equal(await field.getAttribute('aria-invalid'),'true');
 }finally{await f.context.close();}
});
for(const kind of ['trigger','action'])test(`${kind}: missing variable cannot silently remove a condition`,async()=>{
 const f=await fixture();try{
  await f.page.locator(kind==='trigger'?'.cond-variable':'.action-condition-variable').evaluate(e=>e.value='');
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,0);
  assert.equal(await f.page.locator('#add-rule-modal').count(),1);
 }finally{await f.context.close();}
});
test('manual-only preserves dormant trigger conditions',async()=>{
 const r=rule('0',4);r.manual_trigger=true;const f=await fixture(r);
 try{
  assert.equal(await f.page.locator('.cond-value').isDisabled(),true);
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,1);
  assert.deepEqual(f.writes[0].conditions,r.conditions);assert.equal(f.writes[0].manual_trigger,true);
 }finally{await f.context.close();}
});
test('unsupported operator is visible and blocked rather than changed to equality',async()=>{
 const f=await fixture(rule('ready',4,'changed'));try{
  assert.equal(await f.page.locator('.cond-operator').inputValue(),'changed');
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,0);
  assert.equal(await f.page.locator('.cond-operator').getAttribute('aria-invalid'),'true');
 }finally{await f.context.close();}
});
test('all supported operators stay identical across both editors',async()=>{
 const f=await fixture();try{
  const expected=['eq','ne','lt','le','gt','ge','contains'];
  assert.deepEqual(await f.page.locator('.cond-operator option').evaluateAll(es=>es.map(e=>e.value)),expected);
  assert.deepEqual(await f.page.locator('.action-condition-operator option').evaluateAll(es=>es.map(e=>e.value)),expected);
 }finally{await f.context.close();}
});
test('unchecked action condition is intentionally removed; empty string is valid',async()=>{
 const f=await fixture();try{
  await f.page.locator('.cond-value').fill('');await f.page.locator('.action-has-condition').uncheck();
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,1);
  assert.equal(f.writes[0].conditions[0].value,'');assert.equal(f.writes[0].conditions[0].value_type,4);
  assert.equal(Object.hasOwn(f.writes[0].actions[0],'condition'),false);
 }finally{await f.context.close();}
});
test('one escape implementation preserves text and attribute values without double encoding',async()=>{
 const f=await fixture();try{
  const raw='"\'&<> &quot;ready&quot;';
  const result=await f.page.evaluate(raw=>{
   const host=document.createElement('div'),encoded=escapeHtml(raw);
   host.innerHTML=`<span>${encoded}</span><input value="${encoded}"><div title="${encoded}"></div>`;
   return {text:host.querySelector('span').textContent,value:host.querySelector('input').value,title:host.querySelector('div').title};
  },raw);
  assert.deepEqual(result,{text:raw,value:raw,title:raw});
  assert.equal((fs.readFileSync(path.join(root,'js/app.js'),'utf8').match(/^function escapeHtml\(/gm)||[]).length,1);
 }finally{await f.context.close();}
});
for(const [kind,show,run]of [['source','showExportSourceModal','doExportSource'],['rule','showExportRuleModal','doExportRule'],['action','showExportActionModal','doExportAction'],['ssh-cmd','showExportSshCommandModal','doExportSshCommandFromModal'],['ssh-host','showExportSshHostModal','doExportSshHostFromModal']])test(`${kind}: export button passes quoted IDs as data rather than JavaScript source`,async()=>{
 const f=await fixture();try{
  const id='id"\'&<>';
  const returned=await f.page.evaluate(({id,show,run,kind})=>{
   let received;window[run]=value=>received=value;
   window[show](id);document.getElementById('export-'+kind+'-btn').click();return received;
  },{id,show,run,kind});
  assert.equal(returned,id);
 }finally{await f.context.close();}
});
test('inline action conditions remain unchanged even though they are read-only',async()=>{
 const r=rule(1,3);delete r.actions[0].template_id;const f=await fixture(r);
 try{
  assert.equal(await f.page.locator('.action-row[data-inline="true"]').count(),1);
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,1);
  assert.deepEqual(f.writes[0].actions,r.actions);roundtrips.push({name:'inline',original:r,saved:f.stored()});
 }finally{await f.context.close();}
});
test('unresolved action references retain their conditions and type',async()=>{
 const r=rule('true',4);r.actions[0].template_id='missing"\'&<>';const f=await fixture(r);
 try{
  assert.equal(await f.page.locator('.action-template-id').inputValue(),r.actions[0].template_id);
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));assert.equal(f.writes.length,1);
  assert.deepEqual(f.writes[0].actions,r.actions);roundtrips.push({name:'unresolved',original:r,saved:f.stored()});
 }finally{await f.context.close();}
});
test('rejected save preserves the editor, typed value and operator',async()=>{
 const f=await fixture();try{
  await f.page.route('**/api/v1/automation/rules/update',route=>route.fulfill({json:{code:7,error:'fixture rejected'}}));
  await f.page.evaluate(()=>submitAddRule('fixture-rule'));
  assert.equal(await f.page.locator('#add-rule-modal').count(),1);
  assert.equal(await f.page.locator('.cond-value').inputValue(),'"ready"');
  assert.equal(await f.page.locator('.action-condition-value').inputValue(),'"ready"');
  assert.equal(await f.page.locator('.cond-operator').inputValue(),'ne');
  assert.equal(await f.page.locator('#add-rule-modal').getAttribute('data-saving'),'false');
  assert.equal(f.writes.length,0);
 }finally{await f.context.close();}
});
