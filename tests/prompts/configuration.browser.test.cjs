const {test} = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const kit = require('./pagekit.cjs');
const {fixture, version, named} = require('./configuration-fixture.cjs');
kit.install();
for (const language of ['zh-CN','en-US']) test(`${language}: automation tables align columns and keep controls inside their cells`, async () => {
    const d = fixture();
    const respond = call => call.endpoint === 'automation/sources/list'
        ? {code:0,data:{...d.status(),sources:[{id:'source-with-a-long-name-for-alignment',label:'Source label',type:'rest',enabled:true,poll_interval_ms:1000}]}}
        : d.respond(call);
    await kit.withPage(language, respond, async ({page}) => {
        for (const width of [390,1440]) {
            await page.setViewportSize({width,height:900});
            await automation(page);
            const tables = await page.evaluate(() => ['sources-list','rules-list','actions-list'].map(id => {
                const card = document.getElementById(id), header = card.querySelector('.tr.th');
                const left = card.getBoundingClientRect().left;
                return {
                    columns:[...header.children].slice(0,4).map(e => e.getBoundingClientRect().left-left),
                    operations:header.lastElementChild.getBoundingClientRect().left-left,
                    controlsFit:[...card.querySelectorAll('.tr:not(.th) .act')].every(cell =>
                        [...cell.querySelectorAll('button')].every(button => button.getBoundingClientRect().right <= cell.getBoundingClientRect().right+.5))
                };
            }));
            for (const table of tables) {
                assert(table.controlsFit, 'operation buttons stay inside their shared column');
                table.columns.forEach((left,i) => assert(Math.abs(left-tables[0].columns[i])<.5, 'matching columns align across all tables'));
                assert(Math.abs(table.operations-tables[0].operations)<.5, 'operation columns align');
            }
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'table scrolling stays within the page');
        }
    });
});
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

// Controlled promises exercise the real modal entry points without device access.
async function prepareSettingsModalFixture(page, options={}) {
    await page.evaluate(options=>{
        window.b06={options,power:[],fan:[],binding:[],writes:[],pendingWrites:[],
            db:{low_voltage_threshold:12.3,recovery_voltage_threshold:18.4,shutdown_delay_sec:123,recovery_hold_sec:7,fan_stop_delay_sec:99}};
        b06.config=id=>({curve:[{temp:30,duty:30},{temp:50,duty:60},{temp:70,duty:100}],min_duty:id?35:20,max_duty:95,hysteresis:4,min_interval:2500});
        b06.defer=list=>{const request={};request.promise=new Promise((resolve,reject)=>Object.assign(request,{resolve,reject}));list.push(request);return request.promise;};
        b06.apply=values=>{b06.db={low_voltage_threshold:values.low_threshold,recovery_voltage_threshold:values.recovery_threshold,shutdown_delay_sec:values.shutdown_delay,recovery_hold_sec:values.recovery_hold,fan_stop_delay_sec:values.fan_stop_delay};};
        api.call=(endpoint,params)=>{
            if(endpoint==='power.protection.config')return options.delayPower?b06.defer(b06.power):Promise.resolve({code:0,data:structuredClone(b06.db)});
            if(endpoint==='power.protection.set'){
                b06.writes.push(structuredClone(params));
                if(options.delayWrite){const promise=b06.defer(b06.pendingWrites),request=b06.pendingWrites.at(-1),done=request.resolve;request.resolve=()=>{b06.apply(params);done({code:0,data:{persisted:true}});};return promise;}
                b06.apply(params);return Promise.resolve({code:0,data:{persisted:true}});
            }
            if(endpoint==='fan.config')return options.delayFan?b06.defer(b06.fan):Promise.resolve({code:0,data:b06.config(params.id)});
            if(endpoint==='temp.status')return options.delayBinding?b06.defer(b06.binding):Promise.resolve({code:0,data:{temperature_c:42,valid:true}});
            if(endpoint==='automation.variables.list')return Promise.resolve({code:0,data:{variables:[]}});
            if(endpoint==='temp.bind')return Promise.resolve({code:0,data:{bound_variables:[]}});
            if(['fan.limits','fan.curve','fan.mode'].includes(endpoint)){b06.writes.push({endpoint,params:structuredClone(params)});return Promise.resolve({code:0,data:{applied:true,persisted:true}});}
            if(endpoint==='fan.status')return Promise.resolve({code:0,data:{fans:[]}});
            return Promise.resolve({code:0,data:{}});
        };
    },options);
}
for(const language of ['zh-CN','en-US']) {
    test(`${language}: settings modal baseline normal power read/cancel/save/reopen/default confirmation`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showShutdownSettingsModal());
            assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'123');
            await page.locator('#ss-shutdown-delay').fill('124');await page.evaluate(()=>closeShutdownSettingsModal());
            await page.evaluate(()=>showShutdownSettingsModal());assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'123');
            await page.locator('#ss-shutdown-delay').fill('124');await page.evaluate(()=>saveShutdownSettings());
            await page.evaluate(()=>showShutdownSettingsModal());assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'124');
            assert.deepEqual(await page.evaluate(()=>[b06.db.low_voltage_threshold,b06.db.recovery_voltage_threshold,b06.db.recovery_hold_sec,b06.db.fan_stop_delay_sec]),[12.3,18.4,7,99]);
            await page.evaluate(()=>{b06.resetting=resetShutdownSettings();});await kit.confirmSheet(page,false);await page.evaluate(()=>b06.resetting);
            assert.equal(await page.evaluate(()=>b06.writes.length),1);
            await page.evaluate(()=>{b06.resetting=resetShutdownSettings();});await kit.confirmSheet(page,true);await page.evaluate(()=>b06.resetting);
            assert.equal(await page.evaluate(()=>b06.writes.length),2);assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'60');
        });
    });
    for(const failOld of [false,true])test(`${language}: settings modal old power ${failOld?'failure':'success'} cannot overwrite reopened same DOM`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayPower:true});
            await page.evaluate(()=>{b06.first=showShutdownSettingsModal();closeShutdownSettingsModal();b06.second=showShutdownSettingsModal();});
            await page.evaluate(async()=>{b06.power[1].resolve({code:0,data:{...b06.db,shutdown_delay_sec:222}});await b06.second;});
            await page.locator('#ss-shutdown-delay').fill('223');
            await page.evaluate(async fail=>{if(fail)b06.power[0].reject(new Error('old read failed'));else b06.power[0].resolve({code:0,data:{...b06.db,shutdown_delay_sec:333}});await b06.first;},failOld);
            assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'223');
        });
    });
    test(`${language}: settings modal old fan read cannot replace new fan window draft`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayFan:true});
            await page.evaluate(async()=>{await showFanCurveModal(0);closeFanCurveModal();await showFanCurveModal(1);});
            await page.evaluate(async()=>{b06.fan[1].resolve({code:0,data:b06.config(1)});await b06.fan[1].promise;});
            await page.locator('#fan-curve-min-duty').fill('36');
            await page.evaluate(async()=>{b06.fan[0].resolve({code:0,data:b06.config(0)});await b06.fan[0].promise;});
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'36');
        });
    });
    test(`${language}: settings modal initial curve group waits independently of binding group`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayFan:true,delayBinding:true});await page.evaluate(()=>showFanCurveModal(0));
            const before=await page.locator('#fan-curve-min-duty').isDisabled();
            const cancel=await page.locator('#fan-curve-modal [onclick="closeFanCurveModal()"]').isEnabled();
            await page.evaluate(async()=>{b06.fan[0].resolve({code:0,data:b06.config(0)});await b06.fan[0].promise;});
            const apply=await page.locator('#fan-curve-modal [onclick="applyFanCurve()"]').isEnabled();
            const binding=await page.locator('#fan-curve-modal [onclick="bindTempVariable()"]').isDisabled();
            assert.deepEqual({before,cancel,apply,binding},{before:true,cancel:true,apply:true,binding:true});
            await page.evaluate(()=>applyFanCurve());
            assert.deepEqual(await page.evaluate(()=>b06.writes.map(w=>w.endpoint)),['fan.limits','fan.curve','fan.mode']);
            assert.equal(await page.locator('#fan-curve-modal').count(),0);
            await page.evaluate(async()=>{b06.binding[0].resolve({code:0,data:{temperature_c:88,valid:true}});await b06.binding[0].promise;});
        });
    });
    test(`${language}: settings modal old accepted save cannot close newly opened power window`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayWrite:true});await page.evaluate(()=>showShutdownSettingsModal());
            await page.locator('#ss-shutdown-delay').fill('124');await page.evaluate(()=>{b06.saving=saveShutdownSettings();});
            assert.equal(await page.locator('#ss-shutdown-delay').isDisabled(),true);
            assert.equal(await page.locator('#shutdown-settings-modal [onclick="closeShutdownSettingsModal()"]').isEnabled(),true);
            await page.evaluate(()=>saveShutdownSettings());assert.equal(await page.evaluate(()=>b06.pendingWrites.length),1);
            await page.evaluate(()=>closeShutdownSettingsModal());
            await page.evaluate(()=>showShutdownSettingsModal());await page.locator('#ss-shutdown-delay').fill('222');
            await page.evaluate(async()=>{b06.pendingWrites[0].resolve();await b06.saving;});
            assert.equal(await page.locator('#shutdown-settings-modal').evaluate(el=>el.classList.contains('hidden')),false);
            assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'222');assert.equal(await page.evaluate(()=>b06.db.shutdown_delay_sec),124);
        });
    });
    test(`${language}: settings modal old file.text result cannot import into new fan window`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showFanCurveModal(0));
            await page.evaluate(()=>{
                const click=HTMLInputElement.prototype.click;let chooser;
                HTMLInputElement.prototype.click=function(){if(this.type==='file')chooser=this;else return click.call(this);};
                importFanCurveConfig();HTMLInputElement.prototype.click=click;
                const file=new File(['{}'],'late.json',{type:'application/json'});file.text=()=>new Promise(resolve=>b06.fileDone=resolve);
                const transfer=new DataTransfer();transfer.items.add(file);chooser.files=transfer.files;
                const handler=chooser.onchange;chooser.onchange=e=>b06.importing=handler(e);chooser.dispatchEvent(new Event('change'));
                closeFanCurveModal();
            });
            await page.evaluate(()=>showFanCurveModal(1));
            await page.evaluate(async()=>{b06.fileDone(JSON.stringify({type:'fan_curve_config',curve:[{temp:20,duty:10},{temp:80,duty:90}],min_duty:11}));await b06.importing;});
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'35');
            assert.equal(await page.evaluate(()=>b06.writes.length),0);
        });
    });
}

for(const language of ['zh-CN','en-US']) {
    for(const rejectOld of [false,true])test(`${language}: settings modal same-window latest file wins over old ${rejectOld?'failure':'success'}`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showFanCurveModal(0));
            await page.evaluate(()=>{
                b06.files=[];b06.failures=0;const report=reportFailure;reportFailure=(...args)=>{++b06.failures;return report(...args);};
                b06.choose=()=>{
                    const click=HTMLInputElement.prototype.click;let chooser;
                    HTMLInputElement.prototype.click=function(){if(this.type==='file')chooser=this;else return click.call(this);};
                    importFanCurveConfig();HTMLInputElement.prototype.click=click;
                    const request={};b06.files.push(request);
                    const file=new File(['{}'],`draft${b06.files.length}.json`,{type:'application/json'});
                    file.text=()=>new Promise((resolve,reject)=>Object.assign(request,{resolve,reject}));
                    const transfer=new DataTransfer();transfer.items.add(file);chooser.files=transfer.files;
                    request.finished=chooser.onchange({target:chooser});
                };
                b06.choose();b06.choose();
                b06.imported=min=>JSON.stringify({type:'fan_curve_config',curve:[{temp:20,duty:10},{temp:80,duty:90}],min_duty:min});
            });
            await page.evaluate(async()=>{b06.files[1].resolve(b06.imported(36));await b06.files[1].finished;});
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'36');
            await page.evaluate(async rejectOld=>{if(rejectOld)b06.files[0].reject(new Error('old file failure'));else b06.files[0].resolve(b06.imported(11));await b06.files[0].finished;},rejectOld);
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'36');
            assert.equal(await page.evaluate(()=>b06.failures),0,'stale file error does not report failure for current draft');
            assert.equal(await page.evaluate(()=>b06.writes.length),0);
        });
    });
}
for(const language of ['zh-CN','en-US']) {
    test(`${language}: settings modal normal import is draft-only and cancel/reopen restores current fan config`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showFanCurveModal(0));
            await page.evaluate(async()=>{
                const click=HTMLInputElement.prototype.click;let chooser;
                HTMLInputElement.prototype.click=function(){if(this.type==='file')chooser=this;else return click.call(this);};
                importFanCurveConfig();HTMLInputElement.prototype.click=click;
                const transfer=new DataTransfer();transfer.items.add(new File([JSON.stringify({type:'fan_curve_config',curve:[{temp:25,duty:30},{temp:75,duty:90}],min_duty:55})],'ordinary.json',{type:'application/json'}));
                chooser.files=transfer.files;await chooser.onchange({target:chooser});
            });
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'55');
            assert.equal(await page.evaluate(()=>b06.writes.length),0);
            await page.evaluate(()=>{closeFanCurveModal();return showFanCurveModal(0);});
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'20');
        });
    });
    test(`${language}: settings modal normal binding writes immediately and curve cancel preserves accepted binding`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(async()=>{
                b06.bound=[];b06.selected=null;const call=api.call;
                api.call=(endpoint,params)=>{
                    if(endpoint==='automation.variables.list')return Promise.resolve({code:0,data:{variables:[{name:'cpu_temp',type:'float',value:45}]}});
                    if(endpoint==='temp.bind'){
                        if(params?.variables){b06.bound=structuredClone(params.variables);b06.writes.push({endpoint,params:structuredClone(params)});}
                        return Promise.resolve({code:0,data:{bound_variables:b06.bound.map(b=>({...b,valid:true,value:45})),weighted_temp_c:45}});
                    }
                    if(endpoint==='temp.select'){b06.selected=params.source;b06.writes.push({endpoint,params:structuredClone(params)});return Promise.resolve({code:0,data:{}});}
                    return call(endpoint,params);
                };
                await showFanCurveModal(0);await loadTempSourceStatus();
                tempVarBindings=[{name:'cpu_temp',weight:0.75}];renderTempVarBindings();await bindTempVariable();
            });
            assert.deepEqual(await page.evaluate(()=>b06.writes.map(w=>w.endpoint)),['temp.bind','temp.select']);
            await page.evaluate(()=>{closeFanCurveModal();return showFanCurveModal(0);});
            await page.evaluate(()=>loadTempSourceStatus());
            assert.deepEqual(await page.evaluate(()=>[b06.bound,b06.selected,tempVarBindings]),[[{name:'cpu_temp',weight:0.75}],'variable',[{name:'cpu_temp',weight:0.75}]]);
            assert.equal(await page.locator('#temp-var-bindings-container select').inputValue(),'cpu_temp');
        });
    });
}
for(const language of ['zh-CN','en-US']) {
    test(`${language}: settings modal stale reset confirmation does not write into a new power window`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showShutdownSettingsModal());
            await page.evaluate(()=>{b06.resetting=resetShutdownSettings();});
            await page.locator('.confirm-sheet').waitFor();
            await page.evaluate(()=>{closeShutdownSettingsModal();return showShutdownSettingsModal();});
            await page.locator('#ss-shutdown-delay').fill('222');
            await kit.confirmSheet(page,true);await page.evaluate(()=>b06.resetting);
            assert.equal(await page.evaluate(()=>b06.writes.length),0);
            assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'222');
        });
    });
    test(`${language}: settings modal accepted fan apply keeps submitted snapshot and cannot close new window`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(async()=>{
                const call=api.call;api.call=(endpoint,params)=>{
                    if(endpoint==='fan.limits'){b06.writes.push({endpoint,params:structuredClone(params)});return new Promise(resolve=>b06.limitsDone=resolve);}
                    return call(endpoint,params);
                };
                await showFanCurveModal(0);b06.applying=applyFanCurve();
            });
            assert.equal(await page.locator('#fan-curve-modal [onclick="applyFanCurve()"]').isDisabled(),true);
            await page.evaluate(()=>applyFanCurve());assert.equal(await page.evaluate(()=>b06.writes.length),1);
            assert.equal(await page.locator('#fan-curve-points input').first().isDisabled(),true);
            assert.equal(await page.locator('#fan-curve-modal [onclick="closeFanCurveModal()"]').isEnabled(),true);
            await page.evaluate(()=>{closeFanCurveModal();return showFanCurveModal(1);});
            await page.locator('#fan-curve-min-duty').fill('36');
            await page.evaluate(async()=>{b06.limitsDone({code:0,data:{applied:true,persisted:true}});await b06.applying;});
            assert.equal(await page.locator('#fan-curve-modal').count(),1);
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'36');
            assert.deepEqual(await page.evaluate(()=>b06.writes.map(w=>[w.endpoint,w.params.id])),[['fan.limits',0],['fan.curve',0],['fan.mode',0]]);
            assert.deepEqual(await page.evaluate(()=>b06.writes[0].params),{id:0,min_duty:20,max_duty:95});
            assert.deepEqual(await page.evaluate(()=>b06.writes[1].params),{id:0,curve:[{temp:30,duty:30},{temp:50,duty:60},{temp:70,duty:100}],hysteresis:4,min_interval:2500});
            assert.deepEqual(await page.evaluate(()=>b06.writes[2].params),{id:0,mode:'curve'});
        });
    });
}
for (const language of ['zh-CN','en-US']) {
    for (const rejected of [false,true]) test(`${language}: settings modal failed initial reads allow manual settings (${rejected?'exception':'response'})`, async () => {
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayPower:true,delayFan:true});
            await page.evaluate(()=>{b06.opening=showShutdownSettingsModal();});
            assert.equal(await page.locator('#ss-shutdown-delay').isDisabled(),true);
            assert.equal(await page.locator('#shutdown-settings-modal [onclick="closeShutdownSettingsModal()"]').isEnabled(),true);
            await page.evaluate(()=>saveShutdownSettings());
            assert.equal(await page.evaluate(()=>b06.writes.length),0);
            await page.evaluate(async rejected=>{if(rejected)b06.power[0].reject(new Error('read failed'));else b06.power[0].resolve({code:9});await b06.opening;},rejected);
            assert.equal(await page.locator('#shutdown-settings-load-status').innerText(),await page.evaluate(()=>t('modalSettings.defaults')));
            await page.locator('#ss-shutdown-delay').fill('64');await page.evaluate(()=>saveShutdownSettings());
            assert.deepEqual(await page.evaluate(()=>[b06.db.shutdown_delay_sec,b06.writes[0].persist]),[64,true]);
            await page.evaluate(()=>showFanCurveModal(0));
            await page.evaluate(async rejected=>{if(rejected)b06.fan[0].reject(new Error('read failed'));else b06.fan[0].resolve({code:9});await b06.fan[0].promise.catch(()=>{});},rejected);
            assert.equal(await page.locator('#fan-curve-load-status').innerText(),await page.evaluate(()=>t('modalSettings.defaults')));
            await page.locator('#fan-curve-min-duty').fill('21');await page.evaluate(()=>applyFanCurve());
            assert.deepEqual(await page.evaluate(()=>b06.writes.slice(1).map(w=>[w.endpoint,w.params.id])),[['fan.limits',0],['fan.curve',0],['fan.mode',0]]);
        });
    });
    test(`${language}: settings modal fan selection owns latest read and failed new fan identifies prior draft`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayFan:true});await page.evaluate(()=>showFanCurveModal(0));
            assert.equal(await page.locator('#fan-curve-fan-select').isEnabled(),true);
            await page.evaluate(async()=>{b06.fan[0].resolve({code:0,data:b06.config(0)});await b06.fan[0].promise;});
            await page.locator('#fan-curve-min-duty').fill('31');
            await page.evaluate(()=>{document.getElementById('fan-curve-fan-select').value='1';b06.switching=updateFanCurvePreview();});
            await page.evaluate(async()=>{b06.fan[1].resolve({code:9});await b06.switching;});
            assert.equal(await page.locator('#fan-curve-load-status').innerText(),await page.evaluate(()=>t('modalSettings.fanDraft',{id:0})));
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'31');
            await page.evaluate(()=>{document.getElementById('fan-curve-fan-select').value='2';b06.secondSwitch=updateFanCurvePreview();document.getElementById('fan-curve-fan-select').value='3';b06.thirdSwitch=updateFanCurvePreview();});
            await page.evaluate(async()=>{b06.fan[3].resolve({code:0,data:{...b06.config(3),min_duty:43}});await b06.thirdSwitch;});
            await page.locator('#fan-curve-min-duty').fill('44');
            await page.evaluate(async()=>{b06.fan[2].reject(new Error('old fan read failed'));await b06.secondSwitch;});
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'44');
            assert.equal(await page.evaluate(()=>fanCurveConfig.fanId),3);
            assert.equal(await page.locator('#fan-curve-load-status').innerText(),'');
            await page.evaluate(()=>applyFanCurve());
            assert.deepEqual(await page.evaluate(()=>b06.writes.map(w=>[w.endpoint,w.params.id])),[['fan.limits',3],['fan.curve',3],['fan.mode',3]]);
        });
    });
    for(const endpoint of ['temp.status','automation.variables.list','temp.bind']) for(const rejected of [false,true]) {
        test(`${language}: settings modal latest binding group rejects old ${endpoint} ${rejected?'failure':'success'}`,async()=>{
            await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
                await prepareSettingsModalFixture(page);await page.evaluate(()=>showFanCurveModal(0));await page.evaluate(()=>loadTempSourceStatus());
                await page.evaluate(endpoint=>{
                    b06.bindingReads=[];const call=api.call;
                    b06.bindingResponse=(name)=>endpoint==='temp.status'?{code:0,data:{temperature_c:name==='current'?77:44,valid:true}}:
                        endpoint==='automation.variables.list'?{code:0,data:{variables:[{name,type:'float',value:77}]}}:
                        {code:0,data:{bound_variables:[{name,weight:1,value:77,valid:true}],weighted_temp_c:77}};
                    api.call=(name,params)=>{
                        if(name===endpoint)return b06.defer(b06.bindingReads);
                        if(name==='automation.variables.list')return Promise.resolve({code:0,data:{variables:[{name:'current',type:'float',value:77}]}});
                        if(name==='temp.bind')return Promise.resolve({code:0,data:{bound_variables:[{name:'current',weight:1,value:77,valid:true}],weighted_temp_c:77}});
                        return call(name,params);
                    };
                    b06.oldBinding=loadTempSourceStatus();
                },endpoint);
                await page.waitForFunction(()=>b06.bindingReads.length===1);
                await page.evaluate(()=>{b06.newBinding=loadTempSourceStatus();});
                await page.waitForFunction(()=>b06.bindingReads.length===2);
                await page.evaluate(async()=>{b06.bindingReads[1].resolve(b06.bindingResponse('current'));await b06.newBinding;});
                await page.evaluate(async rejected=>{if(rejected)b06.bindingReads[0].reject(new Error('old binding query'));else b06.bindingReads[0].resolve(b06.bindingResponse('old'));await b06.oldBinding;},rejected);
                assert.deepEqual(await page.evaluate(()=>tempVarBindings),[{name:'current',weight:1}]);
                assert.deepEqual(await page.evaluate(()=>availableTempVars.map(v=>v.name)),['current']);
                assert.equal(await page.locator('#fan-curve-temp-current').innerText(),'77.0°C');
                assert.equal(await page.locator('#temp-source-hint').innerText(),await page.evaluate(()=>t('fanPage.selectVariableWeightHint')));
                assert.equal(await page.locator('#fan-curve-modal [onclick="bindTempVariable()"]').isEnabled(),true);
            });
        });
    }
    test(`${language}: settings modal navigation disposal closes both owners before late reads and draw`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayPower:true,delayFan:true,delayBinding:true});
            await page.evaluate(()=>{
                const timeout=window.setTimeout;b06.draws=[];
                window.setTimeout=(fn,delay,...args)=>delay===50?(b06.draws.push(fn),0):timeout(fn,delay,...args);
                b06.opening=showShutdownSettingsModal();showFanCurveModal(0);window.setTimeout=timeout;
                b06.oldCurve=structuredClone(fanCurveConfig);b06.oldBinding=structuredClone(tempVarBindings);
                router.navigation.dispose();b06.draws.forEach(fn=>fn());
            });
            assert.equal(await page.locator('#fan-curve-modal').count(),0);
            assert.equal(await page.locator('#shutdown-settings-modal').evaluate(el=>el.classList.contains('hidden')),true);
            await page.evaluate(async()=>{b06.fan[0].resolve({code:0,data:{...b06.config(0),min_duty:11}});b06.binding[0].reject(new Error('late status'));b06.power[0].reject(new Error('late power'));await b06.opening;await b06.fan[0].promise;});
            assert.deepEqual(await page.evaluate(()=>[fanCurveConfig,tempVarBindings]),await page.evaluate(()=>[b06.oldCurve,b06.oldBinding]));
        });
    });
}
for (const language of ['zh-CN','en-US']) {
    for (const rejected of [false,true]) test(`${language}: settings modal accepted default write ${rejected?'unconfirmed result':'success'} cannot refill reopened window`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page,{delayWrite:true});await page.evaluate(()=>showShutdownSettingsModal());
            await page.evaluate(()=>{b06.resetting=resetShutdownSettings();});await kit.confirmSheet(page,true);
            await page.waitForFunction(()=>b06.pendingWrites.length===1);
            await page.evaluate(()=>{closeShutdownSettingsModal();return showShutdownSettingsModal();});await page.locator('#ss-shutdown-delay').fill('222');
            await page.evaluate(async rejected=>{if(rejected)b06.pendingWrites[0].reject(new Error('default write failed'));else b06.pendingWrites[0].resolve();await b06.resetting;},rejected);
            assert.equal(await page.locator('#ss-shutdown-delay').inputValue(),'222');
            assert.equal(await page.locator('#shutdown-settings-modal').evaluate(el=>el.classList.contains('hidden')),false);
            assert.equal(await page.evaluate(()=>b06.db.shutdown_delay_sec),rejected?123:60);
            await kit.toastIs(page,rejected?'warning':'success');
        });
    });
    test(`${language}: settings modal accepted binding finishes without reading into new owner`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(async()=>{
                b06.bound=[];b06.bindGets=0;const call=api.call;
                api.call=(endpoint,params)=>{
                    if(endpoint==='automation.variables.list')return Promise.resolve({code:0,data:{variables:['cpu_temp','gpu_temp'].map(name=>({name,type:'float',value:45}))}});
                    if(endpoint==='temp.bind'){
                        if(params?.variables){b06.writes.push({endpoint,params:structuredClone(params)});return new Promise(resolve=>b06.bindDone=()=>{b06.bound=structuredClone(params.variables);resolve({code:0,data:{}});});}
                        ++b06.bindGets;return Promise.resolve({code:0,data:{bound_variables:b06.bound}});
                    }
                    if(endpoint==='temp.select'){b06.writes.push({endpoint,params:structuredClone(params)});return Promise.resolve({code:0,data:{}});}
                    return call(endpoint,params);
                };
                await showFanCurveModal(0);await loadTempSourceStatus();
            });
            await page.locator('#fan-curve-modal [onclick="addTempVarBinding()"]').click();
            await page.locator('#temp-var-bindings-container select').selectOption('cpu_temp');
            await page.evaluate(()=>{b06.bindingSaving=bindTempVariable();});
            assert.equal(await page.locator('#fan-curve-modal [onclick="bindTempVariable()"]').isDisabled(),true);
            await page.evaluate(()=>{closeFanCurveModal();return showFanCurveModal(1);});await page.evaluate(()=>loadTempSourceStatus());
            await page.locator('#fan-curve-modal [onclick="addTempVarBinding()"]').click();
            await page.locator('#temp-var-bindings-container select').selectOption('gpu_temp');
            const reads=await page.evaluate(()=>b06.bindGets);
            await page.evaluate(async()=>{b06.bindDone();await b06.bindingSaving;});
            assert.equal(await page.evaluate(()=>b06.bindGets),reads);
            assert.equal(await page.locator('#temp-var-bindings-container select').inputValue(),'gpu_temp');
            assert.equal(await page.evaluate(()=>tempVarBindings[0].name),'gpu_temp');
            assert.equal(await page.evaluate(()=>b06.bound[0].name),'cpu_temp');
            assert.deepEqual(await page.evaluate(()=>b06.writes.map(w=>w.endpoint)),['temp.bind','temp.select']);
        });
    });
    test(`${language}: settings modal export keeps submitted local and SD snapshot after close`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(()=>showFanCurveModal(0));
            await page.locator('#fan-curve-min-duty').fill('21');
            await page.evaluate(()=>{api.fileUpload=(path,blob)=>{b06.exported={path,blob};return new Promise(resolve=>b06.exportDone=resolve);};});
            const downloaded=page.waitForEvent('download');
            await page.evaluate(()=>{b06.exporting=exportFanCurveConfig();});
            assert.equal((await downloaded).suggestedFilename(),'fan_curve_config_0.json');
            await page.evaluate(()=>{closeFanCurveModal();return showFanCurveModal(1);});await page.locator('#fan-curve-min-duty').fill('36');
            await page.evaluate(async()=>{b06.exportDone({code:0});await b06.exporting;});
            const exported=await page.evaluate(async()=>({path:b06.exported.path,config:JSON.parse(await b06.exported.blob.text())}));
            assert.equal(exported.path,'/sdcard/config/fan_curve_config_0.json');
            assert.deepEqual([exported.config.fan_id,exported.config.min_duty,exported.config.hysteresis,exported.config.min_interval],[0,21,4,2500]);
            assert.equal(await page.locator('#fan-curve-min-duty').inputValue(),'36');assert.equal(await page.evaluate(()=>b06.writes.length),0);
        });
    });
}
for(const language of ['zh-CN','en-US']) for(const rejected of [false,true]) {
    test(`${language}: settings modal failed current binding reads allow manual binding (${rejected?'exception':'response'})`,async()=>{
        await kit.withPage(language,()=>({code:0,data:{}}),async({page})=>{
            await prepareSettingsModalFixture(page);await page.evaluate(async rejected=>{
                availableTempVars=[{name:'cpu_temp',type:'float',value:45}];tempVarBindings=[{name:'cpu_temp',weight:0.75}];
                const call=api.call;api.call=(endpoint,params)=>{
                    if(endpoint==='temp.bind'&&params?.variables){b06.writes.push({endpoint,params:structuredClone(params)});return Promise.resolve({code:0,data:{}});}
                    if(endpoint==='temp.select'){b06.writes.push({endpoint,params:structuredClone(params)});return Promise.resolve({code:0,data:{}});}
                    if(['temp.status','automation.variables.list','temp.bind'].includes(endpoint))return rejected?Promise.reject(new Error('current binding read failed')):Promise.resolve({code:9});
                    return call(endpoint,params);
                };
                await showFanCurveModal(0);await loadTempSourceStatus();
            },rejected);
            assert.equal(await page.locator('#temp-source-hint').innerText(),await page.evaluate(()=>t('modalSettings.bindingDraft')));
            assert.equal(await page.locator('#fan-curve-modal [onclick="bindTempVariable()"]').isEnabled(),true);
            assert.equal(await page.locator('#fan-curve-modal [onclick="applyFanCurve()"]').isEnabled(),true);
            assert.equal(await page.locator('#temp-var-bindings-container select').inputValue(),'cpu_temp');
            await page.evaluate(()=>bindTempVariable());
            assert.deepEqual(await page.evaluate(()=>b06.writes),[
                {endpoint:'temp.bind',params:{variables:[{name:'cpu_temp',weight:0.75}]}},
                {endpoint:'temp.select',params:{source:'variable'}}
            ]);
        });
    });
}

function ruleBatchFixture() {
 const d=fixture(),templates=['late_A1','late_A2','late_A3','current_B'].map(id=>({id,name:id,type:'log',enabled:true}));
 const inline={type:'cli',command:'synthetic --inline',delay_ms:17,repeat_mode:'count',repeat_count:2,repeat_interval_ms:500,condition:{variable:'ready',operator:'eq',value:false}};
 const actions=[{template_id:'late_A1',delay_ms:123,repeat_mode:'count',repeat_count:3,repeat_interval_ms:0,condition:{variable:'ready',operator:'eq',value:false}},inline,
  {template_id:'late_A2',delay_ms:456,repeat_mode:'while_true',repeat_count:1,repeat_interval_ms:2345},{template_id:'late_A3',delay_ms:0}];
 const rules={A:{id:'A',name:'Old multi-action rule',enabled:false,manual_trigger:true,conditions:[],actions,revision:1},
  B:{id:'B',name:'Current rule',enabled:false,manual_trigger:true,conditions:[],actions:[{template_id:'current_B'}],revision:1}};
 const writes=[];
 const respond=call=>{
  const {endpoint,params}=call;
  if(endpoint==='automation/rules/list')return {code:0,data:{...d.status(),rules:Object.values(rules)}};
  if(endpoint==='automation/rules/get')return {code:0,data:{...rules[params.id],active_version:version(1)}};
  if(endpoint==='automation/actions/list')return {code:0,data:{...d.status(),templates}};
  if(endpoint==='automation/rules/update'||endpoint==='automation/rules/add'){writes.push(call);rules[params.id]={...params,revision:1};return {code:0,data:d.status({saved:true})};}
  return d.respond(call);
 };
 return {d,templates,rules,writes,respond,inline};
}
async function ruleBatchPage(page) {
 if(process.env.RULE_MODAL_BASELINE_APP){
  let served=0;await page.route(url=>new URL(url).pathname==='/js/app.js',route=>{served++;return route.fulfill({contentType:'application/javascript',body:fs.readFileSync(process.env.RULE_MODAL_BASELINE_APP,'utf8')});});
  await page.reload();await page.waitForFunction(()=>i18n.isReady());await page.evaluate(()=>closeLoginModal());assert.equal(served,1,'archived app source was actually served for this trigger');
 }
 await automation(page);
}
for(const language of ['zh-CN','en-US']) {
 for(const outcome of ['success','failure'])for(const target of ['edit','new'])test(`${language}: F51 old multi-action ${outcome} batch cannot enter ${target} rule`,async()=>{
  const f=ruleBatchFixture();
  await kit.withPage(language,f.respond,async({page})=>{
   await ruleBatchPage(page);
   let release,reached;const gate=new Promise(resolve=>release=resolve),paused=new Promise(resolve=>reached=resolve);let first=true;
   await page.route('**/api/v1/automation/actions/list',async route=>{
    if(first){first=false;reached();await gate;await route.fulfill({json:outcome==='success'?{code:0,data:{...f.d.status(),templates:f.templates}}:{code:7,message:'synthetic old list failure',data:f.d.status()}});}
    else await route.fulfill({json:{code:0,data:{...f.d.status(),templates:f.templates}}});
   });
   await page.locator('#rules-list [onclick*="editRule"]').first().click();await paused;
   await page.evaluate(()=>{window.__oldRuleBatch=document.getElementById('add-rule-modal');});
   await page.locator('#add-rule-modal .sf button:not(.primary)').click();
   if(target==='edit')await page.locator('#rules-list [onclick*="editRule"]').nth(1).click();
   else {
    await page.locator('[onclick="showAddRuleModal()"]').click();
    await page.locator('#rule-id').fill('new_B');await page.locator('#rule-name').fill('New current rule');
    await page.locator('#add-rule-modal [onclick="addActionTemplateRow()"]').click();
   }
   await page.waitForFunction(()=>document.querySelectorAll('#add-rule-modal .action-template-id').length===1);
   await page.locator('#add-rule-modal .action-template-id').selectOption('current_B');
   release();await page.waitForFunction(()=>__oldRuleBatch.dataset.loading==='false');
   assert.equal(await page.locator('#add-rule-modal .action-row').count(),1,'the remaining old batch never appends its inline or template tail');
   assert.deepEqual(await page.locator('#add-rule-modal .action-template-id').evaluateAll(nodes=>nodes.map(node=>node.value)),['current_B']);
   await page.locator('#add-rule-modal .sf .primary').click();
   await page.waitForFunction(()=>!document.getElementById('add-rule-modal'));
   assert.equal(f.writes.length,1);assert.equal(f.writes[0].params.id,target==='edit'?'B':'new_B');
   assert.deepEqual(f.writes[0].params.actions.map(action=>action.template_id),['current_B']);
  });
 });
 test(`${language}: F51 current multi-action batch saves and reopens in order with inline and repeat fields`,async()=>{
  const f=ruleBatchFixture();
  await kit.withPage(language,f.respond,async({page})=>{
   await ruleBatchPage(page);await page.locator('#rules-list [onclick*="editRule"]').first().click();
   await page.waitForFunction(()=>document.getElementById('add-rule-modal')?.dataset.loading==='false');
   assert.equal(await page.locator('#add-rule-modal .action-row').count(),4);
   await page.locator('#rule-name').fill('Kept multi-action order');await page.locator('#add-rule-modal .sf .primary').click();
   await page.waitForFunction(()=>!document.getElementById('add-rule-modal'));
   const actions=f.writes[0].params.actions;
   assert.deepEqual(actions.map(action=>action.template_id||action.type),['late_A1','cli','late_A2','late_A3']);assert.deepEqual(actions[1],f.inline);
   assert.equal(actions[0].delay_ms,123);assert.equal(actions[0].repeat_mode,'count');assert.equal(actions[0].repeat_count,3);assert.equal(actions[0].repeat_interval_ms,0);assert.equal(actions[0].condition.value,false);
   assert.equal(actions[2].delay_ms,456);assert.equal(actions[2].repeat_mode,'while_true');assert.equal(actions[2].repeat_interval_ms,2345);
   await page.locator('#rules-list [onclick*="editRule"]').first().click();await page.waitForFunction(()=>document.getElementById('add-rule-modal')?.dataset.loading==='false');
   assert.deepEqual(await page.locator('#add-rule-modal .action-template-id').evaluateAll(nodes=>nodes.map(node=>node.value)),['late_A1','late_A2','late_A3']);
   assert.match(await page.locator('#add-rule-modal [data-inline="true"]').innerText(),/synthetic --inline/);
  });
 });
}

for(const language of ['zh-CN','en-US'])test(`${language}: F48 source and action export downloads feed the original import preview and confirmation`,async()=>{
 const d=fixture(),calls=[],payloads={source:JSON.stringify({fixture:'source export bytes'}),action:JSON.stringify({fixture:'action export bytes'})};
 const respond=call=>{
  calls.push(call);
  if(call.endpoint==='automation/sources/list')return {code:0,data:{...d.status(),sources:[{id:'source',label:'SSH variable source',type:'variable',enabled:false,poll_interval_ms:60000}]}};
  const kind=call.endpoint==='automation/sources/export'?'source':call.endpoint==='automation/actions/export'?'action':null;
  if(kind)return {code:0,data:{tscfg:payloads[kind],filename:`${kind}_${call.params.id}.tscfg`}};
  return d.respond(call);
 };
 await kit.withPage(language,respond,async({page})=>{
  await automation(page);
  for(const [key,collection,id,title] of [['source','sources','source','Source'],['action','actions','act','Action']]){
   await page.locator(`#${collection}-list [onclick*="showExport${title}Modal"]`).first().click();
   const download=page.waitForEvent('download');await page.locator(`#export-${key}-btn`).click();const artifact=await download;
   assert.equal(artifact.suggestedFilename(),`${key}_${id}.tscfg`);const bytes=fs.readFileSync(await artifact.path());assert.equal(bytes.toString(),payloads[key]);
   const exported=calls.find(call=>call.endpoint===`automation/${collection}/export`);assert.deepEqual(exported.params,{id});
   await page.waitForFunction(key=>document.getElementById(`export-${key}-modal`).classList.contains('hidden'),key);
   await page.locator(`[onclick="showImport${title}Modal()"]`).click();
   await page.locator(`#import-${key}-file`).setInputFiles({name:artifact.suggestedFilename(),mimeType:'application/json',buffer:bytes});
   await page.waitForFunction(key=>!document.getElementById(`import-${key}-btn`).disabled,key);
   const previews=calls.filter(call=>call.endpoint===`automation/${collection}/import`);assert.equal(previews.length,1);assert.equal(previews[0].params.preview,true);assert.equal(previews[0].params.tscfg,payloads[key]);
   await page.locator(`#import-${key}-overwrite`).check();await page.locator(`#import-${key}-btn`).click();
   await page.waitForFunction(key=>document.getElementById(`import-${key}-result`).classList.contains('success'),key);
   const writes=calls.filter(call=>call.endpoint===`automation/${collection}/import`);assert.equal(writes.length,2);assert.equal(writes[1].params.tscfg,payloads[key]);assert.equal(writes[1].params.overwrite,true);assert.equal(writes[1].params.active_version,previews[0].params.active_version);
   await page.locator(`#import-${key}-modal .sf button`).first().click();
  }
 });
});
