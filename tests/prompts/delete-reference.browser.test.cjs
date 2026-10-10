// Replay exact production HTTP bytes; do not replace api.call or the deletion handlers.
const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');const {root}=require('./harness.cjs');
const fixture=key=>fs.readFileSync(path.resolve(__dirname,'../fixtures/delete-reference',key+'.json'),'utf8');
let browser;
before(async()=>{browser=await chromium.launch({channel:'chrome',headless:true});});
after(async()=>{await browser?.close();});
const name='Model “quoted” < & > 中文';
async function pageFor(language,width,run,level='root'){
 const state={templateName:name,wire:'in-use',service:false,serviceState:'stopped',rules:[],loaded:true,recovery:false,level,saveError:null},calls=[],errors=[];
 const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://local');
  const send=body=>{res.setHeader('Content-Type','application/json');res.end(typeof body==='string'?body:JSON.stringify(body));};
  if(url.pathname.startsWith('/api/')){
   let text='';req.on('data',c=>text+=c);req.on('end',()=>{
    const endpoint=url.pathname.slice('/api/v1/'.length);calls.push({endpoint,method:req.method,body:text});
    if(endpoint==='automation/actions/delete')return send(fixture(state.wire));
    if(endpoint==='automation/rules/add')return send(state.saveError?{code:4,message:state.saveError,data:{error_code:state.saveError}}:fixture('action-missing'));
    let data={};
    if(endpoint==='auth/status')data={valid:true,level:state.level,username:state.level};
    if(endpoint==='automation/actions/list')data={templates:[{id:'template',name:state.templateName,type:'log'},{id:'missing',name:'Selected human-readable model',type:'log'}]};
    if(endpoint==='automation/actions/get')data={id:'template',name:state.templateName,type:state.service?'ssh_cmd_ref':'log',ssh_ref:{cmd_id:'model'}};
    if(endpoint==='ssh/commands/get')data={id:'model',name:'Command < & >',nohup:true,serviceMode:true};
    if(endpoint==='automation/services/status')data=req.method==='POST'?{operation_id:31}:{operation_id:31,operation_phase:'succeeded',state:state.serviceState,busy:false};
    if(endpoint==='automation/rules/list')data={loaded:state.loaded,recovery_required:state.recovery,rules:state.rules};
    if(endpoint==='automation/sources/list')data={sources:[]};
    if(endpoint==='ssh/hosts/list')data={hosts:[]};
    if(endpoint==='ssh/commands/list')data={commands:[]};
    return send({code:0,data});
   });return;
  }
  const file=path.resolve(root,'.'+(url.pathname==='/'?'/index.html':url.pathname));
  if(!file.startsWith(root+'/'))return res.writeHead(403).end();
  fs.readFile(file,(error,body)=>{if(error)return res.writeHead(404).end();res.setHeader('Content-Type',({'.html':'text/html','.js':'application/javascript','.css':'text/css','.woff2':'font/woff2','.svg':'image/svg+xml'})[path.extname(file)]||'application/octet-stream');res.end(body);});
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const context=await browser.newContext({viewport:{width,height:900},locale:language});
 await context.addInitScript(({lang,level})=>{localStorage.setItem('ts_language',lang);localStorage.setItem('ts_level',level);localStorage.setItem('ts_token','fixture');localStorage.setItem('ts_username',level);class Socket{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}window.WebSocket=Socket;},{lang:language,level});
 const page=await context.newPage();page.setDefaultTimeout(5000);page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{if(new URL(route.request().url()).origin!==base)return route.abort();return route.continue();});
 try{await page.goto(base);await page.waitForFunction(()=>i18n.isReady());await page.evaluate(root=>{closeLoginModal();if(root)location.hash='/automation';},level==='root');await page.waitForSelector(level==='root'?'#actions-list button[onclick]':'#quick-actions-grid');await run({page,state,calls});assert.deepEqual(errors,[]);}
 finally{await context.close();await new Promise(r=>server.close(r));}
}
const confirm=async page=>{await page.locator('.confirm-sheet').waitFor();await page.locator('.confirm-sheet button[data-r="1"]').click();};
const attempt=async page=>{await page.locator('button[onclick="deleteAction(\'template\')"]').click();await confirm(page);};
const waitTitle=(page,key)=>page.waitForFunction(key=>document.querySelector('.confirm-sheet .st')?.textContent===t(key),key);
for(const language of ['zh-CN','en-US'])test(`${language}: navigation buttons open existing pages without positioning or deletion`,async()=>pageFor(language,390,async({page,state,calls})=>{
 const before=calls.filter(c=>c.endpoint==='automation/actions/list').length;
 await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');await page.keyboard.press('Enter');
 await page.waitForFunction(()=>!document.querySelector('.confirm-sheet'));await page.waitForSelector('#actions-list button[onclick]');
 assert(calls.filter(c=>c.endpoint==='automation/actions/list').length>before);assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,1);
 state.service=true;state.serviceState='ready';await attempt(page);await page.waitForFunction(()=>document.querySelector('.confirm-sheet .st')?.textContent.includes('quoted'));
 await page.keyboard.press('Enter');await page.waitForFunction(()=>location.hash==='#/commands');await page.waitForFunction(()=>document.querySelector('.nav a[href="#/commands"]')?.classList.contains('active'));
 assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,1);assert(!calls.some(c=>c.endpoint==='automation/services/stop'));
}));
for(const language of ['zh-CN','en-US'])for(const width of [390,1440]){
 test(`${language} ${width}: 32 production references, escaping, design, close and navigation`,async()=>pageFor(language,width,async({page,calls})=>{
  await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');
  const sheet=page.locator('.confirm-sheet');assert.equal(await sheet.locator('.grp .row').count(),32);
  assert.equal(await sheet.locator('.grp img,.grp script').count(),0);assert((await sheet.textContent()).includes('中文'));
  const style=await sheet.locator('.sheet').evaluate(e=>{const s=getComputedStyle(e),title=getComputedStyle(e.querySelector('.st')),body=getComputedStyle(e.querySelector('.t-body')),r=e.getBoundingClientRect();return {radius:s.borderRadius,padding:s.padding,weight:title.fontWeight,title:title.fontSize,body:body.fontSize,width:r.width,right:r.right,inner:e.scrollWidth,client:e.clientWidth};});
  const children=await sheet.locator('.grp .row,.grp .rl,.grp .rc').evaluateAll(es=>es.map(e=>({scroll:e.scrollWidth,client:e.clientWidth,right:e.getBoundingClientRect().right,parent:e.parentElement.getBoundingClientRect().right})));
  assert(children.every(e=>e.scroll<=e.client+1&&e.right<=e.parent+1),JSON.stringify(children));
  assert.equal(await page.locator('#toast.show.toast-info').count(),0);
  assert.equal(await page.locator('#toast').isVisible(),false);
  assert.equal(style.title,'20px');assert.equal(style.body,'14px');assert.equal(style.weight,'600');assert(style.right<=width+1);assert(style.inner<=style.client+1);
  assert.equal(style.radius,width===390?'32px 32px 0px 0px':'32px');
  if(process.env.DELETE_SCREENSHOTS==='1'){const out=path.resolve(__dirname,'../../output/playwright/delete-protection');fs.mkdirSync(out,{recursive:true});await page.screenshot({path:path.join(out,language+'-'+width+'.png')});fs.writeFileSync(path.join(out,language+'-'+width+'-style.json'),JSON.stringify(style,null,2));}
  await page.keyboard.press('Escape');assert.equal(await sheet.count(),0);assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,1);
  await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');await page.keyboard.press('Enter');await page.waitForFunction(()=>location.hash==='#/automation');
  assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,2);
 }));
 test(`${language} ${width}: 47-byte label and 63-byte ID remain readable`,async()=>pageFor(language,width,async({page,state})=>{
  state.wire='boundary';await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');
  assert.equal((await page.locator('.confirm-sheet .grp .rc').textContent()).length,63);assert.equal((await page.locator('.confirm-sheet .grp .rl').textContent()).length,47);
  const fields=await page.locator('.confirm-sheet .grp .rl,.confirm-sheet .grp .rc').evaluateAll(es=>es.map(e=>[e.scrollWidth,e.clientWidth]));assert(fields.every(([scroll,width])=>scroll<=width+1));
  const bounds=await page.locator('.confirm-sheet .sheet').evaluate(e=>[e.scrollWidth,e.clientWidth]);assert(bounds[0]<=bounds[1]+1);await page.keyboard.press('Escape');
 }));
 test(`${language} ${width}: service-blocked 47-byte template title does not clip`,async()=>pageFor(language,width,async({page,state})=>{
  state.service=true;state.serviceState='ready';state.templateName='N'.repeat(47);await page.evaluate(()=>refreshActions());await attempt(page);
  await page.waitForFunction(()=>document.querySelector('.confirm-sheet .st')?.textContent.includes('NNNNNNNN'));
  const boxes=await page.locator('.confirm-sheet .st,.confirm-sheet .t-body').evaluateAll(es=>es.map(e=>({scroll:e.scrollWidth,width:e.clientWidth,right:e.getBoundingClientRect().right,parent:e.parentElement.getBoundingClientRect().right})));
  assert(boxes.every(e=>e.scroll<=e.width+1&&e.right<=e.parent+1),JSON.stringify(boxes));await page.keyboard.press('Escape');
 }));
 test(`${language} ${width}: C group has only Close; missing list remains blocked`,async()=>pageFor(language,width,async({page,state,calls})=>{
  state.wire='check-loading';await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');assert.equal(await page.locator('.confirm-sheet button').count(),1);
  const text=await page.locator('.confirm-sheet').textContent();assert(text.includes(await page.evaluate(()=>t('deleteProtection.checkLoading'))));
  await page.keyboard.press('Enter');assert.equal(await page.locator('.confirm-sheet').count(),0);
  state.wire='in-use-no-details';await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');
  assert((await page.locator('.confirm-sheet').textContent()).includes(await page.evaluate(name=>t('deleteProtection.actionInUseWithoutDetails',{name}),name)));
  await page.locator('.confirm-sheet button[data-r="0"]').click();assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,2);
 }));
 test(`${language} ${width}: successive service and reference blockers have no stale sheet`,async()=>pageFor(language,width,async({page,state,calls})=>{
  state.service=true;state.serviceState='ready';await attempt(page);
  await page.waitForFunction(()=>document.querySelector('.confirm-sheet .st')?.textContent.startsWith(t('deleteProtection.templateTitle',{name:'Model'}).split('“')[0]));
  assert((await page.locator('.confirm-sheet').textContent()).includes('Command < & >'));assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,0);
  await page.locator('.confirm-sheet button[data-r="0"]').click();state.serviceState='stopped';await attempt(page);await waitTitle(page,'deleteProtection.actionTitle');
  assert.equal(await page.locator('.confirm-sheet').count(),1);assert.equal(calls.filter(c=>c.endpoint==='automation/actions/delete').length,1);
  await page.keyboard.press('Escape');
 }));
 test(`${language} ${width}: homepage empty/loading/recovery retains grid and entry`,async()=>pageFor(language,width,async({page,state})=>{
  await page.evaluate(()=>{document.getElementById('page-content').innerHTML='<div id="quick-actions-grid"></div>';});
  for(const scenario of ['empty','loading','recovery']){
   state.loaded=scenario==='empty';state.recovery=scenario==='recovery';await page.evaluate(()=>refreshQuickActions());
   const grid=page.locator('#quick-actions-grid');assert.equal(await grid.count(),1);assert.equal(await grid.locator('button').count(),1);
   const key=scenario==='empty'?'automationPage.noQuickActions':scenario==='loading'?'deleteProtection.quickLoadingTitle':'deleteProtection.quickRecoveryTitle';
   assert((await grid.textContent()).includes(await page.evaluate(k=>t(k),key)));
   if(scenario==='loading')assert((await grid.textContent()).includes(await page.evaluate(()=>t('deleteProtection.loadingDetails'))));
   if(scenario==='recovery'){assert((await grid.textContent()).includes(await page.evaluate(()=>runtimeText('recovery_required'))));assert((await grid.textContent()).includes(await page.evaluate(()=>t('deleteProtection.recoveryDetails'))));}
   const bodyKey=scenario==='empty'?'automationPage.quickActionsHint':scenario==='loading'?'runtimeRepair.configLoading':'runtimeRepair.recovery_required';
   assert((await grid.textContent()).includes(await page.evaluate(k=>t(k),bodyKey)));
  }
 }));
 test(`${language} ${width}: stale selection failure names the chosen option and retains draft`,async()=>pageFor(language,width,async({page,calls})=>{
  await page.evaluate(async()=>{showAddRuleModal();await addActionTemplateRow('missing');});
  await page.locator('#rule-id').fill('new-rule');await page.locator('#rule-name').fill('Draft remains');await page.locator('#rule-manual-only').check();
  await page.locator('#add-rule-modal button[onclick="submitAddRule()"] ').click();
  await page.waitForFunction(()=>document.getElementById('toast')?.textContent.includes('Selected human-readable model'));
  assert.equal(await page.locator('#rule-name').inputValue(),'Draft remains');assert.equal(await page.locator('#add-rule-modal').count(),1);assert.equal(calls.filter(c=>c.endpoint==='automation/rules/add').length,1);
 }));
}

// One Chinese admin scene: both auth status and initial storage use the same non-root level.
test('zh-CN: non-root homepage has approved guidance and no management entry',async()=>pageFor('zh-CN',390,async({page,state})=>{
 assert.equal(await page.evaluate(()=>api.getLevel()),'admin');assert.equal(await page.evaluate(()=>api.isRoot()),false);
 for(const scenario of ['empty','loading','recovery']){
  state.loaded=scenario==='empty';state.recovery=scenario==='recovery';await page.evaluate(()=>refreshQuickActions());
  const grid=page.locator('#quick-actions-grid');assert.equal(await grid.count(),1);assert.equal(await grid.locator('button').count(),0);
  const titleKey=scenario==='empty'?'automationPage.noQuickActions':scenario==='loading'?'deleteProtection.quickLoadingTitle':'deleteProtection.quickRecoveryTitle';
  assert.equal(await grid.locator('.t-body').textContent(),await page.evaluate(k=>t(k),titleKey));
  const newKey={empty:'deleteProtection.quickEmptyNeedRoot',loading:'deleteProtection.quickLoadingNeedRoot',recovery:'deleteProtection.quickRecoveryNeedRoot'}[scenario];
  assert((await grid.textContent()).includes(await page.evaluate(k=>t(k),newKey)));
  if(scenario!=='empty')assert.equal(await grid.locator('.t-note').first().textContent(),await page.evaluate(k=>t(k),scenario==='loading'?'runtimeRepair.configLoading':'runtimeRepair.recovery_required'));
  for(const oldKey of ['deleteProtection.loadingDetails','deleteProtection.recoveryDetails','automationPage.quickActionsHint'])
   assert(!(await grid.textContent()).includes(await page.evaluate(k=>t(k),oldKey)));
 }
},'admin'));
test('zh-CN: template lookup failure uses saveFailed and retains the actual draft',async()=>pageFor('zh-CN',390,async({page,state,calls})=>{
 state.saveError='template_lookup_failed';
 await page.evaluate(async()=>{showAddRuleModal();await addActionTemplateRow('missing');});
 await page.locator('#rule-id').fill('new-rule');await page.locator('#rule-name').fill('Lookup draft remains');await page.locator('#rule-manual-only').check();
 await page.locator('#add-rule-modal button[onclick="submitAddRule()"] ').click();
 await page.waitForFunction(()=>document.getElementById('toast')?.textContent===t('runtimeRepair.saveFailed'));
 assert.equal(await page.locator('#rule-name').inputValue(),'Lookup draft remains');assert.equal(await page.locator('#add-rule-modal').count(),1);
 assert.equal(calls.filter(c=>c.endpoint==='automation/rules/add').length,1);
}));
