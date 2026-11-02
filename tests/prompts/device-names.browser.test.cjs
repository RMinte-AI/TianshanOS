const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
if(process.env.TS_NAMES_WEB){
 assert(path.isAbsolute(process.env.TS_NAMES_WEB));
 require('./harness.cjs').root=process.env.TS_NAMES_WEB;
}
const kit=require('./pagekit.cjs');
const {fixture}=require('./configuration-fixture.cjs');
kit.install();
function device(){const d=fixture();return call=>{
 if(call.endpoint==='ui/widgets/get')return {code:0,data:{widgets:[],refresh_interval:5000,source:'default'}};
 if(call.endpoint==='device/usb/status')return {code:0,data:{configured:true,target:'lpmu'}};
 if(call.endpoint==='device/status')return {code:0,data:{state:'on'}};
 if(call.endpoint==='device/ping')return {code:0,data:{reachable:true}};
 if(call.endpoint==='network/lpmu_access/status')return {code:0,data:{stage:'idle',running:false,run_id:0}};
 if(call.endpoint==='automation/actions/execute')return {code:0,data:{output:'AGX reset complete\nuser LPMU <script>literal</script>'}};
 return d.respond(call);
};}
async function login(page){await page.evaluate(async()=>{api.token='fixture';api.username='root';api.level='root';updateAuthUI();router.appReady=true;await router.navigate();});}
async function overflow(page,selector){return page.locator(selector).evaluateAll(nodes=>nodes.flatMap(el=>{
 const box=el.getBoundingClientRect(),card=el.closest('.card,.sheet')?.getBoundingClientRect();const errors=[];
 if(card&&(box.left<card.left-1||box.right>card.right+1))errors.push(el.id+' outside card');
 const range=document.createRange();range.selectNodeContents(el);
 for(const r of range.getClientRects())if(r.left<box.left-1||r.right>box.right+1||r.bottom>box.bottom+1)errors.push(el.id+' clipped text');
 return errors;
}));}
for(const language of ['zh-CN','en-US'])for(const width of [375,768,1440])test(`${language} ${width}px: computer controls and network dialog retain complete names`,async()=>{
 const d=await kit.open(language,device(),{viewport:{width,height:1000}});
 try{
  await login(d.page);await d.page.locator('#agx-power-btn').waitFor();
  assert.match(await d.page.locator('#agx-power-btn').innerText(),language==='zh-CN'?/推理计算机/:/Inference Computer/);
  assert.match(await d.page.locator('#lpmu-power-btn').innerText(),language==='zh-CN'?/应用计算机/:/Application Computer/);
  assert.deepEqual(await overflow(d.page,'#agx-power-btn,#lpmu-power-btn,#usb-mux-btn'),[]);
  if(process.env.NAME_SCREENSHOTS){fs.mkdirSync(process.env.NAME_SCREENSHOTS,{recursive:true});await d.page.screenshot({path:path.join(process.env.NAME_SCREENSHOTS,`${language}-${width}-system.png`),fullPage:true});}
  await d.page.evaluate(()=>router.navigate('/network'));await d.page.locator('#network-lpmu-access-btn').waitFor();
  assert.deepEqual(await overflow(d.page,'#network-lpmu-access-btn'),[]);
  await d.page.locator('#network-lpmu-access-btn').click();
  assert(!/AGX|LPMU/.test(await d.page.locator('#network-lpmu-password-modal').innerText()));
  assert.deepEqual(await overflow(d.page,'#network-lpmu-password-modal .sf button'),[]);
  if(process.env.NAME_SCREENSHOTS)await d.page.screenshot({path:path.join(process.env.NAME_SCREENSHOTS,`${language}-${width}-password.png`),fullPage:true});
  assert.deepEqual(d.errors,[]);
 }finally{await d.close();}
});
test('language switch uses new names and does not submit an open password dialog',async()=>{
 const d=await kit.open('zh-CN',device());
 try{
  await login(d.page);await d.page.evaluate(()=>selectLanguage('en-US'));
  await d.page.waitForFunction(()=>document.querySelector('#usb-mux-target')?.textContent==='Application Computer');
  await d.page.evaluate(()=>router.navigate('/network'));await d.page.locator('#network-lpmu-access-btn').click();
  await d.page.locator('#network-lpmu-sudo-password').fill('LOCAL_ONLY');
  await d.page.evaluate(()=>selectLanguage('zh-CN'));
  await d.page.waitForFunction(()=>!document.getElementById('network-lpmu-password-modal'));
  await d.page.locator('#network-lpmu-access-btn').click();
  assert.equal(await d.page.locator('#network-lpmu-sudo-password').inputValue(),'');
  assert.match(await d.page.locator('#network-lpmu-password-modal').innerText(),/应用计算机/);
  assert.equal(d.calls.filter(c=>c.endpoint==='network/lpmu_access/start').length,0);assert.deepEqual(d.errors,[]);
 }finally{await d.close();}
});
for(const language of ['zh-CN','en-US'])test(`${language}: user names and command output remain intact behind details`,async()=>{
 await kit.withPage(language,device(),async({page})=>{
  await page.evaluate(async()=>{api.level='root';await loadAutomationPage();actionNames.set('a1','User AGX / LPMU');await testAction('a1');});
  assert.match(await kit.toastText(page),/User AGX \/ LPMU/);
  assert(!(await kit.toastText(page)).includes('AGX reset complete'));
  assert.equal(await page.locator('#toast').getAttribute('role'),'status');
  assert.equal(await page.locator('.toast-detail').isVisible(),false);
  await page.locator('.toast-more').click();
  assert.match(await page.locator('.toast-detail').innerText(),/AGX reset complete\nuser LPMU <script>literal<\/script>/);
  assert.equal(await page.locator('#toast script').count(),0);
  await page.evaluate(()=>{document.getElementById('toast').classList.remove('show');showAddActionModal();});
  const quick=page.locator('#action-type-fields button').filter({hasText:language==='zh-CN'?'推理计算机 开机':'Inference Computer Power On'});
  await quick.click();assert.equal(await page.locator('#action-cli-command').inputValue(),'device --power-on agx0');
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: admin sees the same product names without root-only navigation`,async()=>{
 const d=await kit.open(language,device());
 try{
  await d.page.evaluate(async()=>{api.token='fixture';api.username='admin';api.level='admin';updateAuthUI();router.appReady=true;await router.navigate();});
  assert.match(await d.page.locator('#lpmu-power-btn').innerText(),language==='zh-CN'?/应用计算机/:/Application Computer/);
  assert.equal(await d.page.locator('a[href="#/automation"]').isVisible(),false);
  assert.equal(await d.page.locator('#toast.toast-error.show').count(),0);
  assert.deepEqual(d.errors,[]);
 }finally{await d.close();}
});

for(const language of ['zh-CN','en-US'])test(`${language}: unavailable log preserves original failure and output tail in details`,async()=>{
 await kit.withPage(language,device(),async({page})=>{
  await page.evaluate(()=>{
   api.lpmuAccessLog=async()=>({code:2,error:'Execution log for this run is no longer available'});
   showNetworkLpmuAccessResult(normalizeNetworkLpmuAccessStatus({run_id:29,stage:'failed',running:false,reason:'access_script_failed',
    last_error:'setup-smart-route failed (exit=1): remote script failed',output_tail:'LPMU diagnostic <literal>\nAGX context'}));
  });
  assert(!/LPMU|AGX|remote script failed/.test(await page.locator('#network-lpmu-result-modal .t-body').innerText()));
  await page.locator('#network-lpmu-result-modal summary').click();
  await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent.includes('AGX context'));
  const raw=await page.locator('#network-lpmu-result-modal pre').textContent();
  assert(raw.includes('setup-smart-route failed (exit=1): remote script failed'));
  assert(raw.includes('LPMU diagnostic <literal>\nAGX context'));
 });
});
