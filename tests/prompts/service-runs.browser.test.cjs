const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const kit=require('./pagekit.cjs'),{serviceFixture,OLD,NEW,START}=require('./service-runs-fixture.cjs');
kit.install();
const row=(page,id)=>page.locator(`#service-runs-body [data-run="${id}"]`);
for(const lang of ['zh-CN','en-US'])test(`${lang}: runs survive deleted definitions and same-ID replacement`,async()=>{
 const d=serviceFixture();
 await kit.withPage(lang,d.respond,async({page,calls})=>{
  await page.evaluate(()=>showServiceRuns());
  assert.equal(await page.locator('#service-runs-body [data-run]').count(),2);
  assert.equal(d.state.controls.length,0);
  assert.match(await row(page,OLD).innerText(),/192\.0\.2\.1/);assert.match(await row(page,NEW).innerText(),/192\.0\.2\.2/);
  await row(page,OLD).locator('[data-run-action="verify"]').click();
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='running',OLD);
  assert.equal(d.state.controls.length,1);assert.equal(d.state.controls[0].run_id,OLD);assert.equal(d.state.controls[0].command_id,undefined);
  await row(page,OLD).locator('[data-run-action="stop"]').click();
  await kit.confirmSheet(page);
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='stopped',OLD);
  assert.equal(d.state.runs.get(NEW).state,'unknown');
  assert.equal(d.state.controls.at(-1).run_id,OLD);
  assert.equal(calls.filter(c=>c.endpoint==='ssh/services/start').length,0);
 });
});
test('failed verification reports the original account and leaves the run controllable',async()=>{
 const d=serviceFixture();d.state.failVerify=true;
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>showServiceRuns());await row(page,OLD).locator('[data-run-action="verify"]').click();
  await kit.toastIs(page,'warning');
  assert.match(await kit.toastText(page),/original-user/);
  assert.match(await row(page,OLD).innerText(),/original-user/);
  await page.waitForFunction(id=>!document.querySelector(`[data-run="${id}"] [data-run-action="verify"]`).disabled,OLD);
  assert.match(await row(page,OLD).locator('[data-run-status]').innerText(),/unconfirmed/i);
 });
});
for(const index of [0,1])test(`${index===0?'ordinary':'ready-pattern'} background command uses managed admission and its run ID`,async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page,calls})=>{
  await page.evaluate(()=>loadCommandsPage());
  await page.waitForFunction(()=>configurationList('commands')?.version);
  await page.evaluate(()=>selectHost('host-one'));
  await page.evaluate(i=>executeCommand(i),index);
  assert.equal(d.state.starts.length,1);assert.equal(d.state.starts[0].command_id,d.commands[index].id);
  assert.equal(d.state.starts[0].active_version,'00000000000000000000000000000001');
  assert.equal(await page.locator('#exec-result').getAttribute('data-service-run'),START);
  assert.equal(calls.filter(c=>c.endpoint==='ssh/exec_stream').length,0);
  // Selecting another host cannot retarget controls/logs for the accepted run.
  await page.evaluate(()=>selectHost('host-two'));
  await page.evaluate(()=>nohupViewLog());
  await page.waitForFunction(()=>document.querySelector('[data-run-log]')?.textContent.includes('frozen-target'));
  assert.equal(d.state.logs.at(-1).host,'192.0.2.10');assert.equal(d.state.logs.at(-1).keyid,'original-key');
  assert.match(d.state.logs.at(-1).command,new RegExp(START));
 });
});
test('foreground commands keep the streaming execution path',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page,calls})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));await page.evaluate(()=>executeCommand(2));
  assert.equal(d.state.starts.length,0);
  const stream=calls.find(c=>c.endpoint==='ssh/exec_stream');assert.equal(stream.params.command,'printf direct');
 });
});
test('cached refresh preserves open details and keyboard focus, without remote verification',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>showServiceRuns());await row(page,OLD).locator('summary').click();
  await row(page,OLD).locator('[data-run-action="log"]').focus();
  await page.evaluate(()=>refreshServiceStates());
  assert.equal(await row(page,OLD).locator('details').getAttribute('open'),'');
  assert.equal(await row(page,OLD).locator('[data-run-action="log"]').evaluate(el=>el===document.activeElement),true);
  assert.equal(d.state.controls.length,0);
 });
});
test('late cached results cannot overwrite a newer explicit verification',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>showServiceRuns());
  let pending,signal;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/automation/services/list',route=>{pending=route;signal();});
  await page.evaluate(()=>{refreshServiceStates();});await seen;
  await row(page,OLD).locator('[data-run-action="verify"]').click();
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='running',OLD);
  await pending.fulfill({json:{code:0,data:{items:[{...d.state.runs.get(OLD),state:'unknown'}]}}});
  assert.match(await row(page,OLD).locator('[data-run-status]').innerText(),/running/i);
 });
});
test('both languages fit a phone and desktop with separate runs and long original names',async()=>{
 const dir=path.resolve('output/playwright');fs.mkdirSync(dir,{recursive:true});
 for(const lang of ['zh-CN','en-US'])for(const width of [390,1440]){
  const d=serviceFixture();d.state.runs.get(OLD).name='长名称 with quotes “<&>” '.repeat(5);
  const run=await kit.open(lang,d.respond,{viewport:{width,height:900}});
  try{
   await run.page.evaluate(()=>showServiceRuns());
   assert.equal(await run.page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   assert.equal(await run.page.locator('#service-runs-modal .sheet').evaluate(el=>el.scrollWidth>el.clientWidth),false);
   await run.page.screenshot({path:path.join(dir,`service-runs-${lang}-${width}.png`)});
   assert.deepEqual(run.errors,[]);
  }finally{await run.close();}
 }
});
test('fast streamed output is adopted only after its matching admission',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));
  let pending,signal;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/ssh/exec_stream',route=>{pending=route;signal();});
  await page.evaluate(()=>{window.testStream=executeCommand(2);});await seen;
  await page.evaluate(()=>{
   handleSshExecMessage({type:'ssh_exec_output',session_id:8,data:'FOREIGN_OUTPUT'});
   handleSshExecMessage({type:'ssh_exec_output',session_id:71,data:'CURRENT_OUTPUT'});
   handleSshExecMessage({type:'ssh_exec_done',session_id:71,status:'success',exit_code:0});
  });
  await pending.fulfill({json:{code:0,data:{session_id:71}}});await page.evaluate(()=>window.testStream);
  const text=await page.locator('#exec-result').innerText();assert.match(text,/CURRENT_OUTPUT/);assert.doesNotMatch(text,/FOREIGN_OUTPUT/);
 });
});
test('late foreground acknowledgement cannot replace a newer managed run',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));
  let pending,signal;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/ssh/exec_stream',route=>{pending=route;signal();});
  await page.evaluate(()=>{window.testStream=executeCommand(2);});await seen;await page.evaluate(()=>executeCommand(0));
  const original=await page.locator('#exec-result').innerText();
  await pending.fulfill({json:{code:0,data:{session_id:71}}});await page.evaluate(()=>window.testStream);
  await page.evaluate(()=>handleSshExecMessage({type:'ssh_exec_output',session_id:71,data:'OLD_FOREGROUND'}));
  assert.equal(await page.locator('#exec-result').innerText(),original);assert.equal(await page.locator('#exec-result').getAttribute('data-service-run'),START);
 });
});
test('closing a followed log ends its reads and ignores its late response',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>showServiceRuns());
  let pending,signal,oldReads=0;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/ssh/exec',async route=>{
   if(route.request().postDataJSON().host==='192.0.2.1'){++oldReads;pending=route;signal();}else await route.continue();
  });
  await page.evaluate(id=>{showServiceRunLog(id,true);},OLD);await seen;
  await page.locator('[data-log-close]').click();
  await row(page,NEW).locator('[data-run-action="log"]').click();
  await page.waitForFunction(()=>document.querySelector('[data-run-log]')?.textContent.includes('frozen-target'));
  await pending.fulfill({json:{code:0,data:{exit_code:0,stdout:'OLD_LOG_RESPONSE'}}});
  await new Promise(resolve=>setTimeout(resolve,2100));
  assert.equal(oldReads,1);assert.doesNotMatch(await page.locator('[data-run-log]').innerText(),/OLD_LOG_RESPONSE/);
 });
});
test('expired or replaced operations never submit a second verification',async()=>{
 for(const mode of ['expired','replaced']){
  const d=serviceFixture();
  await kit.withPage('en-US',call=>{
   if(call.endpoint==='automation/services/status'&&!call.params.verify){
    const run=d.state.runs.get(OLD);run.operation_phase=mode==='expired'?'expired':'succeeded';if(mode==='replaced')++run.operation_id;
    return {code:0,data:{...run,operation_reason:'service_not_started'}};
   }
   return d.respond(call);
  },async({page,calls})=>{
   await page.evaluate(()=>showServiceRuns());await row(page,OLD).locator('[data-run-action="verify"]').click();await kit.toastIs(page,'warning');
   assert.equal(d.state.controls.length,1);
   assert.equal(calls.filter(c=>c.endpoint==='automation/services/status'&&!c.params.verify).length,1);
  });
 }
});
test('a failed new start cannot leave controls bound to the preceding run',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));await page.evaluate(()=>executeCommand(0));
  ++d.configuration.revision;
  await page.evaluate(()=>executeCommand(1));
  assert.equal(await page.locator('#exec-result').getAttribute('data-service-run'),null);
  assert.equal(await page.locator('#nohup-check-process').isDisabled(),true);
  assert.equal(await page.locator('#nohup-stop-process').isDisabled(),true);
  assert.equal(await page.locator('#nohup-view-log').isDisabled(),true);
  assert.equal(d.state.runs.get(START).state,'running');
  await page.evaluate(()=>showServiceRuns());assert.equal(await row(page,START).isVisible(),true);
 });
});
test('clearing a pending display removes its run binding while the independent run remains',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));
  let pending,signal;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/automation/services/status**',route=>{pending=route;signal();});
  await page.evaluate(()=>{window.testManaged=executeCommand(0);});await seen;
  await page.evaluate(()=>clearExecResult());
  const item=d.state.runs.get(START);item.state='running';item.operation_phase='succeeded';
  await pending.fulfill({json:{code:0,data:{...item}}});await page.evaluate(()=>window.testManaged);
  await page.evaluate(()=>refreshServiceStates());
  assert.equal(await page.locator('#exec-result').innerText(),'');
  assert.equal(await page.locator('#exec-result').getAttribute('data-service-run'),null);
  assert.equal(d.state.runs.get(START).state,'running');
 });
});
test('Escape closes service and import windows without cancelling an unrelated foreground command',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page,calls})=>{
  await page.evaluate(()=>loadCommandsPage());await page.evaluate(()=>selectHost('host-one'));await page.evaluate(()=>executeCommand(2));
  await page.evaluate(()=>showServiceRuns());await page.keyboard.press('Escape');
  assert.equal(await page.locator('#service-runs-modal').count(),0);
  await page.evaluate(()=>showImportSshCommandModal());await page.keyboard.press('Escape');
  assert.equal(await page.locator('#import-ssh-cmd-modal').count(),0);
  assert.equal(await page.evaluate(()=>currentExecSessionId),71);
  assert.equal(calls.filter(c=>/cancel/.test(c.endpoint)).length,0);
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: a missing command icon is shown as unavailable without changing its configuration`,async()=>{
 const d=serviceFixture();d.commands[0].icon='/sdcard/images/missing.png';
 await kit.withPage(language,d.respond,async({page,calls})=>{
  await page.route('**/api/v1/file/download?*',route=>route.fulfill({status:404,contentType:'application/json',body:'{"code":2,"error":"File not found"}'}));
  await page.evaluate(()=>loadCommandsPage());await page.waitForFunction(()=>configurationList('commands')?.version);
  await page.evaluate(()=>selectHost('host-one'));
  const card=page.locator('.command-card').first();
  await card.locator('[data-command-image-note]:not([hidden])').waitFor();
  assert.match(await card.locator('[data-command-image-note]').innerText(),language==='zh-CN'?/图片.*读取/:/image unavailable/);
  assert.equal(await card.locator('[data-command-image-note]').getAttribute('title'),'/sdcard/images/missing.png');
  assert.equal(d.commands[0].icon,'/sdcard/images/missing.png');
  assert(!calls.some(call=>call.endpoint==='ssh/commands/add'||call.endpoint==='automation/config/backup'));
 });
});

for(const lang of ['zh-CN','en-US'])test(`${lang}: dashboard log shortcut uses the original run without triggering the rule`,async()=>{
 const d=serviceFixture();d.state.runs.delete(NEW);d.state.runs.get(OLD).rule_id=''; // Legacy records have no rule link.
 await kit.withPage(lang,call=>call.endpoint==='automation/rules/list'?{code:0,data:{active_version:'00000000000000000000000000000001',loaded:true,rules:[{id:'model-rule',name:'Model',enabled:true,manual_trigger:true,allow_manual_trigger:true,show_on_dashboard:true,runtime_active:true,ssh_commands:['model']}]}}:d.respond(call),async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  const button=page.locator('#quick-actions-grid [data-quick-action="log"]');
  await button.click();await page.waitForFunction(()=>document.querySelector('[data-run-log]')?.textContent.includes('frozen-target'));
  assert.equal(d.state.logs.at(-1).host,'192.0.2.1');assert.equal(d.state.controls.length,0);
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
  assert.equal(calls.filter(c=>c.endpoint==='ssh/services/start').length,0);
 });
});

function dashboardFixture(d) {
 return call=>call.endpoint==='automation/rules/list'?{code:0,data:{active_version:'00000000000000000000000000000001',loaded:true,rules:[{id:'model-rule',name:'Qwen model with a long name',enabled:true,manual_trigger:true,allow_manual_trigger:true,show_on_dashboard:true,runtime_active:true,ssh_commands:['model']}]}}:d.respond(call);
}
for(const lang of ['zh-CN','en-US'])test(`${lang}: dashboard has explicit controls and start clicks submit exactly once`,async()=>{
 const d=serviceFixture();const respond=dashboardFixture(d);
 await kit.withPage(lang,respond,async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  const card=page.locator('#quick-action-model-rule');
  assert.deepEqual(await card.locator('[data-quick-action]').allTextContents(),lang==='zh-CN'?['启动','停止','查看日志']:['Start','Stop','View Log']);
  for(const width of [390,1440]){
   await page.setViewportSize({width,height:1000});
   assert.equal(await card.evaluate(el=>el.scrollWidth>el.clientWidth),false);
   for(const button of await card.locator('[data-quick-action]').all())assert.equal(await button.isVisible(),true);
   await card.screenshot({path:path.resolve('output/playwright',`dashboard-controls-${lang}-${width}.png`)});
  }
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
  await card.locator('[data-quick-action="start"]').click();
  await page.waitForFunction(()=>!document.getElementById('quick-action-model-rule').classList.contains('triggering'));
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,1);
  assert.equal(calls.find(c=>c.endpoint==='automation/rules/trigger').params.active_version,'00000000000000000000000000000001');
  await card.locator('.quick-action-name').click();
  await page.waitForFunction(()=>!document.getElementById('quick-action-model-rule').classList.contains('triggering'));
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,2);
  assert.equal(d.state.controls.length,0);
 });
});
for(const lang of ['zh-CN','en-US'])test(`${lang}: dashboard stop targets the original run and does not start the rule`,async()=>{
 const d=serviceFixture();d.state.runs.get(OLD).rule_id='';d.state.runs.get(NEW).state='stopped';
 await kit.withPage(lang,dashboardFixture(d),async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  const button=page.locator('#quick-action-model-rule [data-quick-action="stop"]');
  await button.click();await kit.confirmSheet(page,false);
  assert.equal(d.state.controls.length,0);
  await button.click();await kit.confirmSheet(page);
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='stopped',OLD);
  assert.equal(d.state.controls.length,1);
  assert.equal(d.state.controls[0].endpoint,'automation/services/stop');
  assert.equal(d.state.controls[0].run_id,OLD);
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
  assert.equal(calls.filter(c=>c.endpoint==='ssh/services/start').length,0);
 });
});
test('dashboard stop asks which run to stop when there are multiple related records',async()=>{
 const d=serviceFixture();d.state.runs.set('unrelated',{...d.state.runs.get(OLD),run_id:'unrelated',command_id:'other',rule_id:'other-rule'});
 await kit.withPage('zh-CN',dashboardFixture(d),async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  await page.locator('#quick-action-model-rule [data-quick-action="stop"]').click();
  await page.locator('#service-runs-body [data-run]').first().waitFor();
  assert.equal(await page.locator('#service-runs-body [data-run]').count(),2);
  assert.equal(d.state.controls.length,0);
  await row(page,NEW).locator('[data-run-action="stop"]').click();await kit.confirmSheet(page);
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='stopped',NEW);
  assert.equal(d.state.controls.length,1);assert.equal(d.state.controls[0].run_id,NEW);
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
 });
});
test('dashboard stop explains an empty list and refuses to use stale records after a read failure',async()=>{
 const d=serviceFixture();const respond=dashboardFixture(d);let fail=false;
 d.state.runs.clear();
 await kit.withPage('zh-CN',call=>fail&&call.endpoint==='automation/services/list'?{code:4,error:'ssh_connection_failed'}:respond(call),async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  const button=page.locator('#quick-action-model-rule [data-quick-action="stop"]');
  await button.click();await kit.toastIs(page,'info');assert.match(await kit.toastText(page),/没有需要停止的任务/);
  await page.evaluate(()=>serviceRuns.set('stale',{run_id:'stale',command_id:'model',state:'unknown'}));fail=true;
  await button.click();await kit.toastIs(page,'warning');assert.match(await kit.toastText(page),/暂时无法读取运行记录/);
  assert.equal(await page.locator('.confirm-sheet').count(),0);
  assert.equal(d.state.controls.length,0);
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
 });
});
test('retained records are labelled as records, and an absent remote log has a clear explanation',async()=>{
 const d=serviceFixture();
 await kit.withPage('zh-CN',call=>call.endpoint==='ssh/exec'?{code:0,data:{exit_code:44,stdout:''}}:d.respond(call),async({page,calls})=>{
  await page.evaluate(()=>showServiceRuns());
  assert.match(await page.locator('#service-runs-modal').innerText(),/运行记录/);
  assert.match(await page.locator('#service-runs-modal').innerText(),/不代表它们仍在运行/);
  await row(page,OLD).locator('[data-run-action="log"]').click();
  await page.waitForFunction(()=>document.querySelector('[data-run-log]')?.textContent.includes('已没有这次运行的日志'));
  assert.equal(await page.locator('[data-run-log-error]').innerText(),'');
  assert.equal(calls.filter(c=>c.endpoint==='ssh/services/start').length,0);
 });
});

for(const lang of ['zh-CN','en-US'])test(`${lang}: automatic retry status stays on the card without resubmitting a start`,async()=>{
 const d=serviceFixture();const respond=dashboardFixture(d);
 await kit.withPage(lang,respond,async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();});
  await page.waitForFunction(()=>!automaticStartPoll || document.querySelector('[data-rule-start-status]'));
  const note=page.locator('#quick-action-model-rule [data-rule-start-status]');
  await page.evaluate(()=>renderAutomaticStarts([{id:'model-rule',state:'waiting',reason:'service_outcome_unknown',retry_after_ms:30000}]));
  assert.match(await note.innerText(),lang==='zh-CN'?/30 秒后再检查，确认前不会重复启动/:/30 seconds before starting another copy/);
  await page.evaluate(()=>renderAutomaticStarts([{id:'model-rule',state:'failed',reason:'ssh_auth_failed'}]));
  assert.match(await note.innerText(),lang==='zh-CN'?/自动启动已停止/:/Automatic start stopped/);
  assert.doesNotMatch(await note.innerText(),/ssh_auth_failed|failure\.reasons/);
  await page.evaluate(()=>renderAutomaticStarts([{id:'model-rule',state:'failed',reason:'automatic_start_timeout'}]));
  assert.match(await note.innerText(),lang==='zh-CN'?/已等待 6 分钟/:/after 6 minutes/);
  assert.equal(await page.locator('#toast.show').count(),0);
  assert.equal(calls.filter(c=>['automation/rules/trigger','ssh/services/start'].includes(c.endpoint)).length,0);
  await page.setViewportSize({width:390,height:1000});
  assert.equal(await page.locator('#quick-action-model-rule').evaluate(el=>el.scrollWidth>el.clientWidth),false);
  await page.locator('#quick-action-model-rule').screenshot({path:path.resolve('output/playwright',`automatic-start-timeout-${lang}.png`)});
 });
});

for(const lang of ['zh-CN','en-US'])test(`${lang}: dashboard and command list share live readiness lamps without SSH polling`,async()=>{
 const d=serviceFixture();d.state.runs.clear();
 const current={run_id:NEW,command_id:'managed',name:'Ready-pattern background',rule_id:'model-rule',host_id:'host-one',host:'192.0.2.10',port:22,username:'original-user',keyid:'original-key',current:true,state:'ready',operation_phase:'succeeded',operation_kind:'start',operation_id:1};
 d.state.runs.set(NEW,current);
 d.state.runs.set(OLD,{...current,run_id:OLD,current:false,host:'192.0.2.2',state:'ready'});
 d.state.runs.set('failed-request',{...current,run_id:'failed-request',current:false,state:'stopped',operation_phase:'failed'});
 const respond=call=>call.endpoint==='automation/rules/list'?{code:0,data:{active_version:'00000000000000000000000000000001',loaded:true,rules:[{id:'model-rule',name:'Qwen3.8-27B-NVFP4 long model name — AGX service',enabled:true,manual_trigger:true,allow_manual_trigger:true,show_on_dashboard:true,runtime_active:true,ssh_commands:['managed']}]}}:d.respond(call);
 await kit.withPage(lang,respond,async({page,calls})=>{
  await page.evaluate(async()=>{await loadSystemPage();await refreshQuickActions();await refreshServiceStates();});
  const lamp=page.locator('#quick-action-model-rule [data-service-commands]');
  assert.equal(await lamp.getAttribute('data-state'),'ready');assert.equal(await lamp.innerText(),lang==='zh-CN'?'已就绪':'Ready');
  assert.equal(await lamp.evaluate(el=>getComputedStyle(el,'::before').backgroundColor),'rgb(52, 199, 89)');
  const statusPosition=async(element,heading,name)=>{
   const bounds=await element.evaluate((e,args)=>{const row=e.closest(args.heading),n=row.querySelector(args.name),r=row.getBoundingClientRect(),s=e.getBoundingClientRect(),t=n.getBoundingClientRect();return {right:Math.abs(r.right-s.right),gap:s.left-t.right,center:Math.abs(s.y+s.height/2-t.y-t.height/2)};},{heading,name});
   assert(bounds.right<1&&bounds.gap>=11&&bounds.center<1,'status has its own top-right position without overlapping the name');
  };
  await statusPosition(lamp,'.quick-action-head','.quick-action-name');
  for(const state of ['checking','running','unknown','failed','timeout','stopped','ready']){
   current.state=state;await page.evaluate(()=>refreshServiceStates());
   assert.equal(await lamp.getAttribute('data-state'),state);
   await statusPosition(lamp,'.quick-action-head','.quick-action-name');
   assert.equal(await lamp.evaluate(el=>el.classList.contains('ok')),state==='ready');
  }
  await page.locator('#quick-action-model-rule').screenshot({path:path.resolve('output/playwright',`status-top-right-home-desktop-${lang}.png`)});
  await page.setViewportSize({width:390,height:1000});
  await statusPosition(lamp,'.quick-action-head','.quick-action-name');
  assert.equal(await page.locator('#quick-action-model-rule').evaluate(el=>el.scrollWidth>el.clientWidth),false);
  await page.locator('#quick-action-model-rule').screenshot({path:path.resolve('output/playwright',`service-ready-home-${lang}.png`)});
  await page.evaluate(async()=>{await loadCommandsPage();selectHost('host-one');await refreshServiceStates();});
  const commandLamp=page.locator('#commands-list [data-service-commands]');
  assert.equal(await commandLamp.getAttribute('data-state'),'ready');
  await statusPosition(commandLamp,'.command-titlebar','.cmd-name');
  await commandLamp.locator('xpath=ancestor::*[contains(@class,"command-card")][1]').screenshot({path:path.resolve('output/playwright',`status-top-right-command-${lang}.png`)});
  current.state='stopped';await page.evaluate(()=>refreshServiceStates());
  assert.equal(await commandLamp.getAttribute('data-state'),'stopped');assert.equal(await commandLamp.evaluate(el=>el.classList.contains('ok')),false);
  assert.equal(d.state.controls.length,0);assert.equal(d.state.starts.length,0);assert.equal(d.state.logs.length,0);
  assert.equal(calls.filter(c=>c.endpoint==='automation/rules/trigger').length,0);
 });
});
