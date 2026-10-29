const {test} = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const kit = require('./pagekit.cjs');
const {fixture, version, named} = require('./configuration-fixture.cjs');
kit.install();
async function automation(page) {
    await page.evaluate(() => loadAutomationPage());
    await page.waitForFunction(() => configurationList('actions')?.version);
}
async function file(page, key) {
    await page.locator(`#import-${key}-file`).setInputFiles({name: 'fixture.tscfg', mimeType: 'application/json', buffer: Buffer.from('{"fixture":true}')});
    await page.locator(`#import-${key}-btn`).waitFor();
    await page.waitForFunction(key => !document.getElementById(`import-${key}-btn`).disabled, key);
}
for (const lang of ['zh-CN','en-US']) {
    test(`${lang}: SD backup, card choice and preview cancellation`, async () => {
        const d = fixture();
        await kit.withPage(lang, d.respond, async ({page}) => {
            await automation(page);await page.evaluate(() => showAutomationConfig());
            await page.locator('[data-config-operation="backup"]').click();
            await page.waitForFunction(() => automationConfigStatus?.internal_exists);
            assert.equal(d.state.calls.filter(c => c.endpoint === 'automation/config/backup').length, 1);
            const body = await page.locator('#automation-config-body').innerText();
            assert.match(body, /SSH/);assert.match(body, lang === 'zh-CN' ? /密钥.*图片.*字体.*日志/ : /Keys.*images.*fonts.*logs/);
            await page.evaluate(() => closeModal('automation-config-modal'));
            d.state.source = 'internal';
            await page.evaluate(() => showAutomationConfig());
            assert.equal(d.state.calls.filter(c => c.endpoint === 'automation/config/apply').length, 0);
            await page.locator('[data-config-operation="use_sd"]').click();
            await page.locator('.confirm-sheet').waitFor();
            assert.match(await page.locator('.confirm-sheet').innerText(), /Card rule/);
            assert.equal(await page.locator('.modal:not(.hidden)').count(), 1);
            await kit.confirmSheet(page, false);
            assert.equal(d.state.source, 'internal');
            await page.locator('[data-config-operation="save_to_sd"]').click();
            await kit.confirmSheet(page);
            await page.waitForFunction(() => automationConfigStatus?.source === 'sd');
            const write = d.state.calls.find(c => c.endpoint === 'automation/config/apply');
            assert.deepEqual(write.params, {operation: 'save_to_sd', active_version: version(1), target_version: 'target-one'});
        });
    });
    test(`${lang}: named references, replacement and force independent of a failed preview`, async () => {
        const d = fixture();
        await kit.withPage(lang, d.respond, async ({page}) => {
            await automation(page);
            await page.evaluate(() => {deleteAction('act');});
            await page.locator('#configuration-delete-replacement').waitFor();
            assert.match(await page.locator('#configuration-delete-modal').innerText(), /Rule <one>/);
            assert.match(await page.locator('#configuration-delete-modal .st').innerText(), /Quoted “模板” <&>/);
            await page.locator('#configuration-delete-replacement').selectOption('replacement');
            await page.locator('[data-delete-submit]').click();
            await page.waitForFunction(() => !document.getElementById('configuration-delete-modal'));
            const applied = d.state.calls.find(c => c.endpoint === 'automation/actions/delete' && !c.params.preview);
            assert.equal(applied.params.replacement_id, 'replacement');
            d.state.deleted = false;d.state.previewError = true;d.state.forceNeedsPause = true;
            await page.evaluate(() => refreshActions());
            await page.evaluate(() => {deleteAction('act');});
            await page.locator('[data-delete-mode="force"]').click();
            await page.locator('[data-delete-submit]').click();
            await page.locator('#configuration-delete-pause').waitFor();
            assert.equal(d.state.deleted, false);
            await page.locator('#configuration-delete-pause').check();
            await page.locator('[data-delete-submit]').click();
            await page.waitForFunction(() => !document.getElementById('configuration-delete-modal'));
            assert.equal(d.state.paused, true);
            assert.equal(d.state.calls.filter(c => c.params.verify || c.endpoint === 'automation/services/stop').length, 0);
        });
    });
    test(`${lang}: editors retain input and original version when configuration changes`, async () => {
        const d = fixture();
        await kit.withPage(lang, d.respond, async ({page}) => {
            await automation(page);await page.evaluate(() => editAction('act'));
            await page.locator('#action-name').fill('Keep my input');
            ++d.state.revision;
            await page.locator('#action-modal .sf .primary').click();
            await kit.toastIs(page, 'error');
            assert.equal(await page.locator('#action-name').inputValue(), 'Keep my input');
            const writes = d.state.calls.filter(c => c.endpoint === 'automation/actions/update');
            assert.equal(writes.length, 1);assert.equal(writes[0].params.active_version, version(1));
        });
    });
}
for (const [key,show] of [['ssh-host','showImportSshHostModal'],['ssh-cmd','showImportSshCommandModal'],['source','showImportSourceModal'],['action','showImportActionModal'],['rule','showImportRuleModal']]) {
    test(`${key}: import submits the verified package and configuration version`, async () => {
        const d = fixture();d.state.source = 'internal';
        await kit.withPage('zh-CN', d.respond, async ({page}) => {
            await automation(page);await page.evaluate(show => window[show](), show);await file(page,key);
            await page.locator(`#import-${key}-overwrite`).check();
            await page.locator(`#import-${key}-btn`).click();
            await page.waitForFunction(key => document.getElementById(`import-${key}-result`).classList.contains('success') && !document.getElementById(`import-${key}-modal`)._pack.preview,key);
            const [preview,write] = d.state.calls.filter(c => c.endpoint.endsWith('/import'));
            assert.equal(write.params.active_version, version(1));assert.equal(write.params.tscfg,preview.params.tscfg);
            assert.equal(write.params[key === 'rule' ? 'credential_generation' : 'expected_credential'],7);
            assert.equal(write.params[key === 'rule' ? 'package_digest' : 'expected_digest'],'a'.repeat(64));
            assert.equal(write.params.overwrite,true);
            assert.equal(await page.locator(`#import-${key}-btn`).isDisabled(),true);
            assert.doesNotMatch(await page.locator(`#import-${key}-result`).innerText(),/重启|restart/i);
            assert.equal(d.state.source,'internal');
        });
    });
}
test('a stale import keeps the file and never repeats the write', async () => {
    const d=fixture();
    await kit.withPage('en-US',d.respond,async({page})=>{
        await automation(page);await page.evaluate(()=>showImportActionModal());await file(page,'action');++d.state.revision;
        await page.locator('#import-action-btn').click();
        await page.locator('#import-action-result.error').waitFor();
        assert.equal(await page.locator('#import-action-file').evaluate(el=>el.files[0].name),'fixture.tscfg');
        assert.equal(d.state.calls.filter(c=>c.endpoint==='automation/actions/import'&&!c.params.preview).length,1);
        assert.equal(await page.locator('#import-action-btn').isDisabled(),true);
    });
});
test('English and Chinese configuration panels fit a phone and desktop', async () => {
    const dir=path.resolve('output/playwright');fs.mkdirSync(dir,{recursive:true});
    for (const lang of ['zh-CN','en-US']) for (const width of [390,1440]) {
        const d=fixture();d.state.source='internal';d.state.backup=true;
        const run=await kit.open(lang,d.respond,{viewport:{width,height:900}});
        try {
            await automation(run.page);await run.page.evaluate(()=>showAutomationConfig());
            await run.page.locator('[data-config-operation="save_to_sd"]').waitFor();
            assert.equal(await run.page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
            assert.equal(await run.page.locator('#automation-config-modal .sheet').evaluate(el=>el.scrollWidth>el.clientWidth),false);
            await run.page.screenshot({path:path.join(dir,`configuration-${lang}-${width}.png`)});
            assert.deepEqual(run.errors,[]);
        } finally {await run.close();}
    }
});
test('force deletion stays available while impact preview is still pending', async () => {
    const d=fixture();
    await kit.withPage('en-US',d.respond,async({page})=>{
        await automation(page);
        let pending, signal;const seen=new Promise(resolve=>signal=resolve);
        await page.route('**/api/v1/automation/actions/delete',async route=>{
            if (route.request().postDataJSON()?.preview) {pending=route;signal();}
            else await route.continue();
        });
        await page.evaluate(()=>{deleteAction('act');});
        await seen;
        await page.locator('[data-delete-mode="force"]').click();
        await page.locator('[data-delete-submit]').click();
        await page.waitForFunction(()=>!document.getElementById('configuration-delete-modal'));
        assert.equal(d.state.deleted,true);
        await pending.fulfill({json:{code:0,data:{...d.status(),impact:{rules:[{id:'r1',name:'Late impact'}]}}}});
        assert.equal(await page.locator('#configuration-delete-modal').count(),0);
    });
});
test('an old save response cannot close or fill a newly opened editor', async () => {
    const d=fixture();
    await kit.withPage('en-US',d.respond,async({page})=>{
        await automation(page);await page.evaluate(()=>editAction('act'));
        let pending,reply,signal;const seen=new Promise(resolve=>signal=resolve);
        await page.route('**/api/v1/automation/actions/update',async route=>{
            reply=d.respond({endpoint:'automation/actions/update',params:route.request().postDataJSON()});pending=route;signal();
        });
        await page.locator('#action-name').fill('First draft');
        await page.locator('#action-modal .sf .primary').click();
        await seen;
        await page.evaluate(async()=>{await router.navigate('/');await loadAutomationPage();});
        await page.waitForFunction(()=>configurationList('actions')?.version);
        await page.evaluate(()=>showAddActionModal());
        await page.locator('#action-name').fill('New editor input');
        await pending.fulfill({json:reply});
        await page.waitForFunction(()=>!document.querySelector('#action-modal .sf .primary').disabled);
        assert.equal(await page.locator('#action-name').inputValue(),'New editor input');
        assert.equal(await page.locator('#action-modal').isVisible(),true);
    });
});
test('deletion shows long reference lists, escapes names and requires an explicit click', async () => {
    const d=fixture(),base=d.respond;
    await kit.withPage('en-US',call=>{
        if (call.endpoint==='automation/actions/delete'&&call.params.preview) return {code:0,data:{...d.status(),impact:{rules:Array.from({length:32},(_,i)=>({id:'r'+i,name:'Long rule <'+i+'> '+named})),actions:[],commands:[],sources:[]}}};
        return base(call);
    },async({page})=>{
        await page.setViewportSize({width:390,height:900});await automation(page);
        await page.evaluate(()=>{deleteAction('act');});
        await page.locator('#configuration-delete-impact li').first().waitFor();
        assert.equal(await page.locator('#configuration-delete-impact li').count(),32);
        assert.equal(await page.locator('#configuration-delete-modal .sheet').evaluate(el=>el.scrollWidth>el.clientWidth),false);
        await page.locator('[data-delete-submit]').focus();await page.keyboard.press('Enter');
        assert.equal(d.state.deleted,false);
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('#configuration-delete-modal').count(),0);
        assert.equal(d.state.deleted,false);
    });
});
test('revoking a remote key uses the same local host deletion decision', async () => {
    const d=fixture();
    await kit.withPage('en-US',call=>{
        if (call.endpoint==='ssh/hosts/list') return {code:0,data:{...d.status(),hosts:[{id:'host-one',host:'192.0.2.1',port:22,username:'root',keyid:'test-key'}]}};
        if (call.endpoint==='ssh/revoke') return {code:0,data:{revoked:true,removed_count:1}};
        return d.respond(call);
    },async({page,calls})=>{
        await page.evaluate(async()=>{await loadSecurityPage();await refreshSshHostsList();});
        await page.waitForFunction(()=>configurationList('hosts')?.version);
        await page.evaluate(()=>revokeKeyFromHost(0));
        await page.locator('#revoke-host-password').fill('fixture-password');
        await page.locator('#revoke-host-btn').click();
        await page.locator('#configuration-delete-modal').waitFor();
        assert.equal(calls.filter(c=>c.endpoint==='ssh/revoke').length,1);
        assert.equal(calls.filter(c=>c.endpoint==='ssh/hosts/remove'&&!c.params.preview).length,0);
        await page.locator('[data-delete-cancel]').click();
        assert.equal(calls.filter(c=>c.endpoint==='ssh/hosts/remove'&&!c.params.preview).length,0);
    });
});
test('empty, loading and unavailable states retain the dashboard widget and appropriate management entry',async()=>{
 for(const lang of ['zh-CN','en-US'])for(const role of ['root','admin'])for(const mode of ['empty','loading','unavailable']){
  const d=fixture();
  await kit.withPage(lang,call=>call.endpoint==='automation/rules/list'?{code:0,data:{...d.status(),rules:[],loaded:mode==='empty',load_error:mode==='unavailable'?'configuration_unavailable':''}}:d.respond(call),async({page})=>{
   await page.evaluate(async role=>{api.level=role;await loadSystemPage();await refreshQuickActions();},role);
   const grid=page.locator('#quick-actions-grid');assert.equal(await grid.isVisible(),true);
   assert(await grid.evaluate(e=>Math.abs(e.firstElementChild.getBoundingClientRect().width-e.getBoundingClientRect().width)<1),'notice spans the widget width');
   assert.equal(await grid.locator('button').count(),role==='root'?1:0);
   if(mode==='unavailable'){
    assert.match(await grid.innerText(),lang==='zh-CN'?/配置无法读取/:/could not be read/);
    if(role==='root'){const out=path.resolve(__dirname,'../../output/playwright/reviewer');fs.mkdirSync(out,{recursive:true});await page.setViewportSize({width:390,height:900});await grid.scrollIntoViewIfNeeded();await page.screenshot({path:path.join(out,'unavailable-'+lang+'.png')});}
   }
   if(role!=='root')assert.match(await grid.innerText(),lang==='zh-CN'?/管理员/:/administrator/);
  });
 }
});
test('a closed deployment cannot fill or unlock a newly opened key deployment',async()=>{
 const d=fixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  await page.evaluate(async()=>{await loadSecurityPage();await refreshSshHostsList();showDeployKeyModal('first-key');});
  await page.locator('#deploy-host').fill('192.0.2.8');await page.locator('#deploy-password').fill('synthetic');
  let pending,signal;const seen=new Promise(resolve=>signal=resolve);
  await page.route('**/api/v1/ssh/copyid',route=>{pending=route;signal();});
  await page.locator('#deploy-btn').click();await seen;
  assert.equal(pending.request().postDataJSON().active_version,version(1));
  await page.evaluate(()=>{hideDeployKeyModal();showDeployKeyModal('second-key');});
  await page.locator('#deploy-host').fill('192.0.2.99');
  await pending.fulfill({json:{code:0,data:{deployed:true,registered:true,verified:true}}});
  await page.waitForLoadState('networkidle');
  assert.equal(await page.locator('#deploy-host').inputValue(),'192.0.2.99');
  assert.equal(await page.locator('#deploy-key-id').innerText(),'second-key');
  assert.equal(await page.locator('#deploy-result').isVisible(),false);
  assert.equal(await page.locator('#deploy-btn').isDisabled(),false);
 });
});
test('deployment distinguishes a remote success from a local version conflict without replay',async()=>{
 const d=fixture();
 await kit.withPage('zh-CN',call=>call.endpoint==='ssh/copyid'?{code:4,message:'host_registration_failed',data:{deployed:true,registered:false,verified:true,registration_error:'configuration_changed'}}:d.respond(call),async({page,calls})=>{
  await page.evaluate(async()=>{await loadSecurityPage();await refreshSshHostsList();showDeployKeyModal('test-key');});
  await page.locator('#deploy-host').fill('192.0.2.8');await page.locator('#deploy-password').fill('synthetic');await page.locator('#deploy-btn').click();
  await page.locator('#deploy-result.warning').waitFor();
  assert.match(await page.locator('#deploy-result').innerText(),/已部署/);assert.match(await page.locator('#deploy-result').innerText(),/没有保存/);
  assert.doesNotMatch(await page.locator('#deploy-result').innerText(),/ESP_ERR|NVS|服务保护/);
  assert.equal(await page.locator('#deploy-btn').isDisabled(),true);assert.equal(await page.locator('#deploy-password').inputValue(),'');
  assert.equal(calls.filter(c=>c.endpoint==='ssh/copyid').length,1);
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: testing a template uses its displayed configuration version`,async()=>{
 const d=fixture();
 const respond=call=>call.endpoint==='automation/actions/execute'
  ? (d.state.calls.push(call),{code:4,message:'configuration_changed'}) : d.respond(call);
 await kit.withPage(language,respond,async({page})=>{
  await automation(page);
  ++d.state.revision;
  await page.evaluate(()=>testAction('act'));
  const starts=d.state.calls.filter(call=>call.endpoint==='automation/actions/execute');
  assert.equal(starts.length,1);assert.equal(starts[0].params.id,'act');
  assert.equal(starts[0].params.active_version,version(1));
  const text=await kit.toastText(page);
  assert.match(text,language==='zh-CN'?/配置|刷新/:/configuration|Refresh/);
  assert(!/configuration_changed|ESP_/.test(text));
 });
});

for(const language of ['zh-CN','en-US'])for(const [kind,endpoint] of [
 ['rules','automation/rules/delete'],['actions','automation/actions/delete'],['sources','automation/sources/delete'],
 ['commands','ssh/commands/remove'],['hosts','ssh/hosts/remove']
])test(`${language}: ${kind} normal/force deletion reports a real save failure and retains the item`,async()=>{
 const d=fixture(),writes=[];
 await kit.withPage(language,call=>{
  if(call.endpoint===endpoint){
   if(call.params.preview)return {code:0,data:{...d.status(),impact:{rules:[],actions:[],commands:[],sources:[],references_confirmed:true}}};
   writes.push(call);return {code:1,message:'storage_write_failed',data:d.status({saved:false})};
  }return d.respond(call);
 },async({page,calls})=>{
  await page.evaluate(({kind,status})=>{
   recordConfigurationList(kind,status,[{id:'kept',name:'Kept item',enabled:true}]);
   deleteConfiguration(kind,'kept','Kept item');
  },{kind,status:d.status()});
  await page.waitForFunction(()=>!document.querySelector('[data-delete-submit]').disabled);
  await page.locator('[data-delete-submit]').click();
  await page.waitForFunction(()=>document.getElementById('configuration-delete-result')?.textContent.trim());
  assert.equal(writes.length,1);assert.equal(await page.locator('#configuration-delete-modal').count(),1);
  const failure=await page.locator('#configuration-delete-result').innerText();
  assert.match(failure,language==='zh-CN'?/保存|写入/:/sav|writ/i);assert(!/storage_write_failed|ESP_/.test(failure));
  assert.equal(await page.evaluate(kind=>configurationList(kind)?.items.length,kind),1);
  await page.locator('[data-delete-mode="force"]').click();await page.locator('[data-delete-submit]').click();
  await page.waitForFunction(()=>!document.querySelector('[data-delete-submit]').disabled);
  assert.equal(writes.length,2);assert.equal(writes[1].params.force,true);
  assert.equal(writes[1].params.active_version,version(1));
  assert.equal(await page.locator('#configuration-delete-modal').count(),1);
  assert(!calls.some(call=>/services|ssh\/exec/.test(call.endpoint)));
  await page.locator('[data-delete-cancel]').click();
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: editing a rule preserves inline actions, repeat fields and explicit false`,async()=>{
 const d=fixture(),inline={type:'cli',command:'synthetic --arg',async:true,delay_ms:200,repeat_mode:'count',repeat_count:3,repeat_interval_ms:0,
  condition:{variable:'system.ready',operator:'eq',value:false}};
 const rule={id:'inline',name:'Inline rule',enabled:false,manual_trigger:true,allow_manual_trigger:true,show_on_dashboard:false,revision:7,
  conditions:[],actions:[inline]};let write;
 await kit.withPage(language,call=>{
  if(call.endpoint==='automation/rules/get')return {code:0,data:{...rule,active_version:version(1)}};
  if(call.endpoint==='automation/rules/update'){write=call;return {code:0,data:d.status({saved:true})};}
  return d.respond(call);
 },async({page})=>{
  await automation(page);await page.evaluate(()=>editRule('inline'));
  await page.locator('#rule-name').fill('Renamed rule');
  await page.evaluate(()=>submitAddRule('inline'));
  assert(write);assert.equal(write.params.active_version,version(1));
  assert.deepEqual(write.params.actions,[inline]);assert.equal(write.params.show_on_dashboard,false);
  assert.equal(write.params.manual_trigger,true);assert.equal(write.params.name,'Renamed rule');
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: emergency mode explains unavailable SD images and fonts`,async()=>{
 const d=fixture();d.state.source='internal';d.state.sd_available=false;d.state.backup=true;
 await kit.withPage(language,d.respond,async({page})=>{
  await page.setViewportSize({width:390,height:900});await automation(page);
  await page.waitForFunction(()=>automationConfigStatus?.source==='internal');
  const expected=language==='zh-CN'?/图片和字体/:/images and fonts/;
  assert.match(await page.locator('#automation-config-summary').innerText(),expected);
  await page.evaluate(()=>showAutomationConfig());
  assert.match(await page.locator('#automation-config-modal [role="status"]').first().innerText(),expected);
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
 });
});

for(const language of ['zh-CN','en-US'])test(`${language}: an asynchronous template acknowledges its queue without claiming completion`,async()=>{
 const d=fixture();
 await kit.withPage(language,call=>call.endpoint==='automation/actions/execute'?{code:0,data:{async:true,status:'queued'}}:d.respond(call),async({page,calls})=>{
  await automation(page);await page.evaluate(()=>testAction('act'));
  assert.equal(calls.filter(call=>call.endpoint==='automation/actions/execute').length,1);
  assert.match(await kit.toastText(page),language==='zh-CN'?/排队/:/queued/);
  await kit.toastIs(page,'info');
 });
});

for(const lang of ['zh-CN','en-US'])test(`${lang}: unloaded configuration shows the actual signer setup problem without claiming SD is in use`,async()=>{
 const f=fixture();
 await kit.withPage(lang,call=>{
  if(call.endpoint==='automation/config/status')return {code:0,data:f.status({loaded:false,configured:false,paused:true,enabled:false,load_error:'signing_trust_unconfigured'})};
  if(['automation/rules/list','automation/actions/list','automation/sources/list'].includes(call.endpoint))return {code:1,message:'signing_trust_unconfigured'};
  return f.respond(call);
 },async({page,errors})=>{
  await page.evaluate(()=>loadAutomationPage());
  const wording=await page.evaluate(()=>({notLoaded:configText('notLoaded'),usingSd:configText('usingSd'),reason:t('failure.reasons.signing_trust_unconfigured.why')}));
  assert((await page.locator('#automation-config-summary').innerText()).includes(wording.notLoaded));
  assert(!(await page.locator('#automation-config-summary').innerText()).includes(wording.usingSd));
  assert((await page.locator('#actions-list').innerText()).includes(wording.reason));
  await page.getByRole('button',{name:lang==='zh-CN'?'配置与备份':'Configuration & backup',exact:true}).click();
  await page.waitForFunction(reason=>document.getElementById('automation-config-body')?.textContent.includes(reason),wording.reason);
  assert((await page.locator('#automation-config-body').innerText()).includes(wording.reason));
  assert.equal(await page.locator('[data-config-operation="backup"]').isEnabled(),false);
  assert.deepEqual(errors,[]);
 });
});

test('configuration identifiers remain literal in command and host controls',async()=>{
 const d=fixture(),id="lab'host",cmd="cmd'one",key="key'one";
 await kit.withPage('en-US',call=>call.endpoint==='ssh/hosts/list'?{code:0,data:{...d.status(),hosts:[{id,host:'192.0.2.4',username:'fixture',keyid:'key',auth_type:'key',port:22}]}}:
  call.endpoint==='ssh/commands/list'?{code:0,data:{...d.status(),commands:[{id:cmd,host_id:id,name:'Quoted command',command:'echo fixture',icon:'ri-play-line'}]}}:
  call.endpoint==='key/list'?{code:0,data:{keys:[{id:key,type:'ed25519',has_pubkey:true,exportable:true}]}}:d.respond(call),async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.locator('[data-host-id]').click();
  assert.equal(await page.evaluate(()=>selectedHostId),id);
  await page.evaluate(()=>{window.exported=[];window.exportSshCommand=id=>window.exported.push(id);});
  await page.locator('.command-card .act button[onclick*="exportSshCommand"]').click();
  assert.deepEqual(await page.evaluate(()=>window.exported),[cmd]);
  await page.evaluate(()=>loadSecurityPage());await page.locator('#ssh-hosts-table-body .tr').waitFor();
  await page.evaluate(()=>{window.exportedHosts=[];window.exportSshHost=id=>window.exportedHosts.push(id);});
  await page.locator('#ssh-hosts-table-body button[onclick*="exportSshHost"]').click();
  assert.deepEqual(await page.evaluate(()=>window.exportedHosts),[id]);
  await page.evaluate(()=>{window.exportedKeys=[];window.exportKey=id=>window.exportedKeys.push(id);});
  await page.locator('#keys-table-body button[onclick*="exportKey"]').click();
  assert.deepEqual(await page.evaluate(()=>window.exportedKeys),[key]);
 });
});
test('command icons render configuration text without creating markup',async()=>{
 const d=fixture(),icon='<img src="invalid-local-icon" onerror="window.iconMarkupRan=true">',svg='ri-play-line" data-injected="yes';
 await kit.withPage('en-US',call=>call.endpoint==='ssh/hosts/list'?{code:0,data:{...d.status(),hosts:[{id:'h',host:'192.0.2.4',username:'fixture',keyid:'key',port:22}]}}:
  call.endpoint==='ssh/commands/list'?{code:0,data:{...d.status(),commands:[icon,svg].map((icon,i)=>({id:'c'+i,host_id:'h',name:'Literal icon',command:'echo fixture',icon}))}}:d.respond(call),async({page})=>{
  await page.evaluate(()=>loadCommandsPage());await page.locator('[data-host-id]').click();
  assert.equal(await page.locator('.command-icon img').count(),0);
  assert.equal(await page.locator('.command-icon').first().textContent(),icon);
  assert.notEqual(await page.evaluate(()=>window.iconMarkupRan),true);
  assert.equal(await page.locator('.command-icon use').getAttribute('href'),'#'+svg);
  assert.equal(await page.locator('.command-icon use').getAttribute('data-injected'),null);
 });
});
test('deletion rechecks a changed configuration before another confirmation',async()=>{
 for(const lang of ['zh-CN','en-US'])for(const timing of ['before-preview','after-preview']){
  const d=fixture();
  await kit.withPage(lang,d.respond,async({page})=>{
   await automation(page);
   if(timing==='before-preview')++d.state.revision;
   await page.evaluate(()=>deleteAction('act'));
   if(timing==='after-preview'){
    ++d.state.revision;await page.locator('[data-delete-mode="force"]').click();await page.locator('[data-delete-submit]').click();
    await page.waitForFunction(()=>document.getElementById('configuration-delete-result').textContent.length>0);
   }
   const review=page.locator('[data-delete-recheck]');assert.equal(await review.count(),1,'changed version has an explicit recheck');
   assert(await page.locator('[data-delete-submit]').isDisabled());assert.equal(d.state.deleted,false);
   if(lang==='en-US'&&timing==='after-preview'){const out=path.resolve(__dirname,'../../output/playwright/reviewer');fs.mkdirSync(out,{recursive:true});await page.setViewportSize({width:390,height:900});await review.scrollIntoViewIfNeeded();await page.screenshot({path:path.join(out,'delete-recheck-en-US.png')});}
   const writes=()=>d.state.calls.filter(c=>c.endpoint==='automation/actions/delete'&&!c.params.preview);
   const count=writes().length;await review.click();
   await page.waitForFunction(()=>document.querySelector('[data-delete-submit]')?.disabled===false);
   assert.equal(writes().length,count,'reviewing never submits deletion');
   await page.locator('[data-delete-mode="force"]').click();await page.locator('[data-delete-submit]').click();
   await page.waitForFunction(()=>!document.getElementById('configuration-delete-modal'));
   assert.equal(writes().at(-1).params.active_version,version(2));assert.equal(d.state.deleted,true);
  });
 }
});

for (const lang of ['zh-CN','en-US']) test(`${lang}: approved command disclosure and management sheet geometry`, async () => {
 const d=fixture();let noReferences=false;
 const respond=call=>noReferences&&call.endpoint==='automation/actions/delete'&&call.params.preview?{code:0,data:{...d.status(),impact:{rules:[],actions:[],commands:[],sources:[]}}}:call.endpoint==='ssh/hosts/list'?{code:0,data:{...d.status(),hosts:[{id:'layout-host',host:'192.0.2.4',username:'fixture',port:22}]}}:
  call.endpoint==='ssh/commands/list'?{code:0,data:{...d.status(),commands:[{id:'long-command',host_id:'layout-host',name:'Qwen3.8-27B-NVFP4',command:'printf "<literal>" '+('long-command-argument '.repeat(45))+'\nprintf second-line',nohup:true,varName:'model',icon:'ri-rocket-line'}]}}:d.respond(call);
 await kit.withPage(lang,respond,async({page,calls})=>{
  const out=path.resolve(__dirname,'../../output/playwright/approved-layouts');fs.mkdirSync(out,{recursive:true});
  for(const width of [1440,390]){
   await page.setViewportSize({width,height:1000});await page.evaluate(()=>loadCommandsPage());await page.locator('[data-host-id="layout-host"]').click();
   const card=page.locator('.command-card');await card.locator('.command-toggle').waitFor();
   const geometry=await card.evaluate(e=>{
    const r=s=>e.querySelector(s).getBoundingClientRect(),b=r('.command-body'),a=r('.act'),c=r('.command-top');
    const buttons=[...e.querySelectorAll('.act button')].map(x=>{const z=x.getBoundingClientRect();return {x:z.x,y:z.y,h:z.height}});
    return {center:Math.abs(b.y+b.height/2-a.y-a.height/2),top:Math.min(a.y,b.y)-c.y,bottom:c.bottom-Math.max(a.bottom,b.bottom),buttons};
   });
   assert(geometry.center<.1&&Math.abs(geometry.top-geometry.bottom)<.1,'details and buttons centered below the separate title/status row');
   assert.equal(geometry.buttons.length,6);
   const headingLines=await page.locator('.page-commands>.sec-h:first-child>.sec-t').evaluate(e=>{const r=document.createRange();r.selectNodeContents(e);return r.getClientRects().length;});assert.equal(headingLines,1,'host heading remains on one line');assert.equal(new Set(geometry.buttons.map(x=>x.x)).size,3);assert.equal(new Set(geometry.buttons.map(x=>x.y)).size,2);assert.equal(new Set(geometry.buttons.map(x=>x.h)).size,1);
   assert.equal(await card.locator('.cmd-code').isVisible(),false);await page.screenshot({path:path.join(out,`commands-${lang}-${width}.png`)});
   await card.locator('.command-toggle').click();assert.equal(await card.locator('.command-toggle').getAttribute('aria-expanded'),'true');
   const expanded=await card.evaluate(e=>{const a=e.getBoundingClientRect(),b=e.querySelector('.cmd-code').getBoundingClientRect(),s=getComputedStyle(e);return {expected:a.width-parseFloat(s.paddingLeft)-parseFloat(s.paddingRight),actual:b.width,left:b.left-a.left,right:a.right-b.right};});
   assert(Math.abs(expanded.expected-expanded.actual)<.1&&Math.abs(expanded.left-expanded.right)<.1,'expanded command fills both columns');
   assert.match(await card.locator('.cmd-code').innerText(),/\n.*second-line/);assert.equal(await card.locator('.cmd-code script').count(),0);
   await page.screenshot({path:path.join(out,`commands-expanded-${lang}-${width}.png`)});
   await card.locator('.command-toggle').click();assert.equal(await card.locator('.cmd-code').isVisible(),false);
   await automation(page);await page.evaluate(()=>showAutomationConfig());await page.locator('.config-operation').first().waitFor();
   assert.equal(await page.locator('.config-operation').count(),4);
   const operations=await page.locator('.config-operation').evaluateAll(es=>es.map(e=>{const b=e.querySelector('button').getBoundingClientRect(),d=e.querySelector('.t-note').getBoundingClientRect();return {gap:innerWidth<=600?b.top-d.bottom:b.left-d.right}}));
   assert(operations.every(x=>x.gap>=11.9),'each operation is separated from its explanation');
   const sheet=await page.locator('#automation-config-modal .sheet').boundingBox();assert(sheet.x>=0&&sheet.x+sheet.width<=width);
   await page.screenshot({path:path.join(out,`backup-${lang}-${width}.png`)});await page.keyboard.press('Escape');await page.locator('#automation-config-modal').waitFor({state:'detached'});
   await page.evaluate(()=>deleteAction('act'));await page.locator('#configuration-delete-replacement').waitFor();
   const gaps=await page.locator('#configuration-delete-modal').evaluate(e=>{let r=s=>e.querySelector(s).getBoundingClientRect();return {impact:r('.delete-impact').top-r('.delete-method').bottom,note:r('.delete-service-note').top-r('.delete-impact').bottom,foot:r('.sf').top-r('.delete-service-note').bottom};});
   assert(gaps.impact>=23.9&&gaps.note>=19.9&&gaps.foot>=23.9,'delete mode, references, note and footer have independent spacing');
   await page.screenshot({path:path.join(out,`delete-${lang}-${width}.png`)});
   await page.locator('[data-delete-mode="force"]').click();assert.equal(await page.locator('[data-delete-mode="force"]').getAttribute('aria-pressed'),'true');
   await page.screenshot({path:path.join(out,`delete-force-${lang}-${width}.png`)});await page.keyboard.press('Escape');await page.locator('#configuration-delete-modal').waitFor({state:'detached'});
   noReferences=true;await page.evaluate(()=>deleteAction('act'));await page.waitForFunction(()=>!document.querySelector('[data-delete-submit]').disabled);
   assert.equal(await page.locator('#configuration-delete-impact').innerText(),await page.evaluate(()=>configText('noReferences')));
   assert.equal(await page.locator('#configuration-delete-replacement').count(),0);
   await page.screenshot({path:path.join(out,`delete-empty-${lang}-${width}.png`)});
   await page.keyboard.press('Escape');await page.locator('#configuration-delete-modal').waitFor({state:'detached'});noReferences=false;
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  }
  assert(!calls.some(c=>/\/delete$|\/backup$|\/apply$/.test(c.endpoint)&&!c.params.preview),'layout review performs no configuration write');
 });
});

for(const lang of ['zh-CN','en-US'])test(lang+': long names and emergency controls fit compact viewports',async()=>{
 const d=fixture();const name='W'.repeat(63);
 await kit.withPage(lang,call=>call.endpoint==='automation/actions/delete'&&call.params.preview?{code:0,data:{...d.status(),impact:{rules:[{id:name,name}],actions:[],commands:[],sources:[]}}}:d.respond(call),async({page})=>{
  await page.evaluate(()=>loadAutomationPage());await page.waitForFunction(()=>configurationList('actions')?.version);
  for(const viewport of [{width:320,height:568},{width:844,height:390}]){
   await page.setViewportSize(viewport);await page.evaluate(n=>deleteConfiguration('actions','act',n),name);await page.locator('#configuration-delete-replacement').waitFor();
   const b=await page.locator('#configuration-delete-modal').evaluate(m=>{const s=m.querySelector('.sheet').getBoundingClientRect(),t=m.querySelector('.st').getBoundingClientRect(),body=m.querySelector('.sb');return {title:t.right<=s.right,body:body.scrollWidth<=body.clientWidth,sheet:s.top>=0&&s.bottom<=innerHeight}});assert.deepEqual(b,{title:true,body:true,sheet:true});
   await page.locator('[data-delete-cancel]').click();
   d.state.source='internal';d.state.sd_available=false;await page.evaluate(()=>showAutomationConfig());await page.locator('[data-config-operation="check"]').waitFor();
   assert(await page.locator('[data-config-operation="use_sd"]').isDisabled());assert(await page.locator('[data-config-operation="save_to_sd"]').isDisabled());
   const overflow=await page.locator('#automation-config-modal .sb').evaluate(e=>e.scrollWidth>e.clientWidth);assert.equal(overflow,false);
   await page.locator('[data-config-operation="check"]').click();await page.waitForFunction(()=>document.getElementById('automation-config-modal').dataset.busy==='false');
   await page.locator('#automation-config-modal .sf button').click();await page.locator('#automation-config-modal').waitFor({state:'detached'});
   d.state.sd_available=true;await page.evaluate(()=>showAutomationConfig());await page.locator('[data-config-operation="use_sd"]').waitFor();
   assert.equal(await page.locator('[data-config-operation="use_sd"]').isDisabled(),false);
   assert.equal(await page.locator('#automation-config-modal .sb').evaluate(e=>e.scrollWidth>e.clientWidth),false);
   await page.locator('#automation-config-modal .sf button').click();await page.locator('#automation-config-modal').waitFor({state:'detached'});
  }
 });
});
