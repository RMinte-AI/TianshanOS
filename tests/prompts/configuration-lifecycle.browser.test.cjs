const {test} = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const kit = require('./pagekit.cjs');
const {fixture} = require('./configuration-fixture.cjs');
kit.install();
function lifecycleFixture() {
    const base = fixture();
    const state = {hosts: [], commands: [{id:'orphan',name:'Saved command',host_id:'',command:'printf test',orphan:true,enabled:false}],
        sources: [{id:'sensor',label:'Temperature',type:'rest',rest_url:'http://example.invalid/old',rest_method:'GET',rest_auth:'Bearer synthetic',enabled:true,poll_interval_ms:5000,auto_discover:false,mappings:[{path:'temperature',var:'sensor.temperature'}]}],
        variables: [{name:'saved_value',value:3}], failKey:false, calls:[]};
    const ok = data => ({code:0,data:{...base.status(),...data}});
    const respond = call => {
        state.calls.push(call);const {endpoint:e,params:p} = call;
        if(e==='ssh/hosts/list')return ok({hosts:state.hosts});
        if(e==='ssh/commands/list')return ok({commands:state.commands});
        if(e==='ssh/commands/add')return {code:4,message:'object_exists',data:base.status()};
        if(e==='ssh/commands/update'){Object.assign(state.commands[0],p,{orphan:!p.host_id});return ok({saved:true,id:p.id});}
        if(e==='ssh/hosts/update'){Object.assign(state.hosts[0],p);return ok({saved:true,id:p.id});}
        if(e==='automation/sources/list')return ok({sources:state.sources});
        if(e==='automation/sources/update'){Object.assign(state.sources[0],p);return ok({saved:true,id:p.id});}
        if(e==='automation/variables/list')return ok({variables:state.variables});
        if(e==='automation/variables/delete'){
            const impact={references_confirmed:true,requires_pause:false,rules:[{id:'reader',name:'Temperature reader',effect:'disable'}],actions:[],commands:[],sources:[]};
            if(p.preview)return ok({impact});
            assert.equal(p.name,'saved_value');assert(p.force || p.mode==='cascade');state.variables=[];return ok({saved:true,impact});
        }
        if(e==='key/delete'){
            if(p.preview)return ok({complete:true,hosts:['AGX'],runs:[{id:'old',name:'Existing task',host:'192.0.2.98'}]});
            return state.failKey?{code:7,message:'key_delete_incomplete'}:ok({deleted:true});
        }
        return base.respond(call);
    };
    return {state,respond};
}
for(const language of ['zh-CN','en-US']) {
    test(`${language}: orphan commands remain reachable without hosts and can be rebound`,async()=>{
        const d=lifecycleFixture();
        await kit.withPage(language,d.respond,async({page})=>{
            await page.evaluate(()=>loadCommandsPage());
            assert.equal(await page.locator('.orphan-group').count(),1);
            assert.match(await page.locator('#commands-list').innerText(),/Saved command/);
            await page.evaluate(()=>editCommand(0));
            assert.equal(await page.locator('#cmd-host-id').inputValue(),'');assert.equal(await page.locator('#cmd-enabled').isChecked(),false);
            await page.locator('#cmd-name').fill('Kept without a host');await page.locator('#command-modal .sf .primary').click();
            await page.waitForFunction(()=>document.getElementById('command-modal').classList.contains('hidden'));
            assert.equal(d.state.calls.filter(c=>c.endpoint==='ssh/commands/update').length,1);
            assert.equal(d.state.commands[0].host_id,'');
            d.state.hosts=[{id:'new-host',host:'192.0.2.1',username:'operator',port:22,auth_type:'key',keyid:'synthetic'}];
            await page.evaluate(()=>loadHostSelector());await page.evaluate(()=>editCommand(0));
            await page.locator('#cmd-host-id').selectOption('new-host');await page.locator('#cmd-enabled').check();
            await page.locator('#command-modal .sf .primary').click();await kit.toastIs(page,'success');
            assert.equal(d.state.commands[0].host_id,'new-host');assert.equal(d.state.commands[0].enabled,true);
        });
    });
    test(`${language}: new command reports an occupied ID and keeps the editor`,async()=>{
        const d=lifecycleFixture();d.state.hosts=[{id:'host',host:'192.0.2.1',username:'operator',port:22}];
        await kit.withPage(language,d.respond,async({page})=>{
            await page.evaluate(async()=>{await loadCommandsPage();selectHost('host');showAddCommandModal();});
            await page.locator('#cmd-edit-id').fill('orphan');await page.locator('#cmd-name').fill('Duplicate');await page.locator('#cmd-command').fill('printf changed');
            await page.locator('#command-modal .sf .primary').click();await kit.toastIs(page,'error');
            assert.equal(await page.locator('#command-modal').isVisible(),true);assert.equal(d.state.commands[0].command,'printf test');
            assert.equal(d.state.calls.filter(c=>c.endpoint==='ssh/commands/add').length,1);
        });
    });
    test(`${language}: source and host editors submit only explicit changes`,async()=>{
        const d=lifecycleFixture();d.state.hosts=[{id:'host',host:'192.0.2.1',username:'operator',port:22,auth_type:'key',keyid:'synthetic'}];
        await kit.withPage(language,d.respond,async({page})=>{
            await page.evaluate(()=>loadAutomationPage());await page.evaluate(()=>editSource('sensor'));
            assert.equal(await page.locator('#source-edit-rest_url').inputValue(),'http://example.invalid/old');
            await page.locator('#source-edit-rest_url').fill('http://example.invalid/new');await page.locator('#source-edit-poll_interval_ms').fill('7');
            await page.locator('#source-edit-modal .sf .primary').click();await kit.toastIs(page,'success');
            const save=d.state.calls.find(c=>c.endpoint==='automation/sources/update');assert(save);assert.equal(save.params.poll_interval_ms,7000);assert(!('mappings' in save.params));assert.equal(d.state.sources[0].mappings[0].var,'sensor.temperature');
            await page.evaluate(async()=>{await loadSecurityPage();await refreshSshHostsList();editSshHost('host');});
            assert.equal(await page.locator('#host-edit-keyid').inputValue(),'synthetic');await page.locator('#host-edit-host').fill('192.0.2.44');
            await page.locator('#host-edit-modal .sf .primary').click();await kit.toastIs(page,'success');
            assert.equal(d.state.calls.find(c=>c.endpoint==='ssh/hosts/update').params.host,'192.0.2.44');
        });
    });
    test(`${language}: saved variables use normal and force deletion with truthful results`,async()=>{
        const d=lifecycleFixture();
        await kit.withPage(language,d.respond,async({page})=>{
            await page.evaluate(()=>loadAutomationPage());await page.evaluate(()=>showUserVariables());
            assert.match(await page.locator('#user-variables-body').innerText(),/saved_value/);
            await page.evaluate(()=>deleteUserVariable('saved_value'));await page.locator('#configuration-delete-replacement').waitFor();
            assert.match(await page.locator('#configuration-delete-impact').innerText(),/Temperature reader/);
            assert.equal(await page.locator('#user-variables-modal').isVisible(),false);
            await page.locator('[data-delete-mode="force"]').click();await page.locator('[data-delete-submit]').click();
            await page.waitForFunction(()=>!document.getElementById('configuration-delete-modal'));
            assert.equal(await page.locator('#user-variables-modal').isVisible(),true);
            assert.doesNotMatch(await page.locator('#user-variables-body').innerText(),/saved_value/);
            assert.match(await kit.toastText(page),language==='zh-CN'?/已停用 1 项规则/:/Disabled 1 related items: Rules/);
        });
    });
    test(`${language}: key deletion explains host and run impact, and reports storage failure`,async()=>{
        const d=lifecycleFixture();d.state.failKey=true;
        await kit.withPage(language,d.respond,async({page})=>{
            await page.evaluate(()=>{void deleteKey('synthetic');});await page.locator('.confirm-sheet').waitFor();
            const text=await page.locator('.confirm-sheet').innerText();assert.match(text,/AGX/);assert.match(text,/Existing task/);assert.match(text,/192\.0\.2\.98/);
            await kit.confirmSheet(page);await kit.toastIs(page,'error');
            assert.match(await kit.toastText(page),language==='zh-CN'?/没有完整完成/:/did not finish completely/);
            assert.equal(d.state.calls.filter(c=>c.endpoint==='key/delete'&&!c.params.preview).length,1);
        });
    });
    test(`${language}: new management sheets fit phone and desktop`,async()=>{
        const d=lifecycleFixture();d.state.hosts=[{id:'host',host:'192.0.2.1',username:'operator',port:22,auth_type:'key',keyid:'synthetic'}];
        const out=path.resolve('output/playwright/lifecycle');fs.mkdirSync(out,{recursive:true});
        await kit.withPage(language,d.respond,async({page})=>{
            for(const width of [390,1440]) {
                await page.setViewportSize({width,height:1000});await page.evaluate(()=>loadAutomationPage());
                for(const [open,id] of [['editSource("sensor")','source-edit-modal'],['showUserVariables()','user-variables-modal']]) {
                    await page.evaluate(open=>eval(open),open);await page.locator('#'+id+' .sheet').waitFor();
                    assert(await page.locator('#'+id+' .sheet').evaluate(el=>el.scrollWidth<=el.clientWidth+1));
                    await page.screenshot({path:path.join(out,`${id}-${language}-${width}.png`)});await page.evaluate(id=>document.getElementById(id).remove(),id);
                }
                await page.evaluate(async()=>{await loadSecurityPage();await refreshSshHostsList();editSshHost('host');});
                assert(await page.locator('#host-edit-modal .sheet').evaluate(el=>el.scrollWidth<=el.clientWidth+1));
                await page.screenshot({path:path.join(out,`host-edit-${language}-${width}.png`)});await page.evaluate(()=>document.getElementById('host-edit-modal').remove());
            }
        });
    });
}
