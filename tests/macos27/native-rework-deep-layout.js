async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local fixture only');
    const results=[],errors=[];
    const profile=require('./profiles.cjs');
    page.on('pageerror',e=>errors.push(e.message));
    const imageName='fixture-'+('long-image-name-'.repeat(6))+'.png';
    const variables=Array.from({length:20},(_,i)=>({name:i?'fixture.'+'long_variable_name_'.repeat(5)+i:'fixture.selected',source_id:'fixture-source',type:'number',value:i}));
    await page.route(origin+'/api/v1/automation/variables/list*',r=>r.fulfill({json:{code:0,data:{variables}}}));
    await page.route(origin+'/api/v1/storage/list*',r=>r.fulfill({json:{code:0,data:{entries:[{name:imageName,type:'file',size:4096},...Array.from({length:20},(_,i)=>({name:'fixture-'+i+'.png',type:'file',size:128}))]}}}));
    await page.route(origin+'/api/v1/ssh/exec*',r=>r.fulfill({json:{code:0,data:{stdout:Array.from({length:100},(_,i)=>'LOCAL SYNTHETIC LOG '+i+' '+('long line '.repeat(8))).join('\n')}}}));
    await page.route(origin+'/api/v1/ssh/commands/list*',r=>r.fulfill({json:{code:0,data:{commands:[{...profile['ssh/commands/list'].commands[0],nohup:true,serviceMode:true,varName:'fixture'}]}}}));
    await page.route(origin+'/api/v1/automation/rules/get*',r=>r.fulfill({json:{code:0,data:{...profile['automation/rules/get'].rule,actions:[{type:'ssh_cmd_ref',cmd_id:'fixture-command'}]}}}));
    await page.route(origin+'/api/v1/automation/services/status*',r=>r.fulfill({json:{code:0,data:{state:'running',source:'local-fixture',confirmed_ms:1000}}}));
    const click=async handler=>page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
    for(const [width,height] of [[320,600],[390,600],[844,390],[1440,600]])for(const lang of ['en-US','zh-CN'])for(const name of ['widget-variable','rule-variable','action-variable','image','file-picker','quick-log']) {
        await page.setViewportSize({width,height});
        const route=name==='file-picker'?'/commands':(['rule-variable','action-variable','image'].includes(name)?'/automation':'/');
        await page.goto(origin+'/?state=populated&lang='+lang+'#'+route);await page.reload();
        let modal,scroller;
        if(name==='widget-variable') {
            await click('showWidgetManager()');await click("showWidgetEditPanel('fixture-ring')");await click('selectVariableForWidget()');
        } else if(name==='rule-variable') {
            await click('showAddRuleModal()');await click('addConditionRow()');await page.locator('[onclick^="openConditionVarSelector("]').last().click();
        } else if(name==='action-variable') {
            await click('showAddRuleModal()');await click('addActionTemplateRow()');await page.locator('.action-has-condition').last().check();await page.locator('.action-condition-var-btn').last().click();
        } else if(name==='image') {
            await click('showAddActionModal()');await page.locator('.action-type-card[data-type="led"]').click();await page.locator('#action-led-device').selectOption('matrix');await page.locator('#action-led-matrix-type').selectOption('image');await click('browseActionImages()');
        } else if(name==='file-picker') {
            await page.locator('[data-host-id="fixture-host"]').click();await click('showAddCommandModal()');await click("switchCmdIconType('image')");await click('browseCmdIconImage()');
        } else {
            await page.locator('[onclick^="quickActionViewLog("]').click();
        }
        if(name.endsWith('variable')) {
            modal=page.locator('#variable-select-modal');
            await modal.locator('.var-group-header').first().waitFor();
            await page.waitForFunction(()=>document.activeElement?.id==='var-search');
            await modal.locator('.var-group-header').first().click();
            await modal.locator('.var-select-item').first().waitFor();scroller=modal.locator('.modal-body');
        } else if(name==='image') {
            modal=page.locator('#image-select-modal');await modal.locator('.image-select-item').first().waitFor();scroller=modal.locator('.modal-body');
        } else if(name==='file-picker') {
            modal=page.locator('#file-picker-modal');await modal.locator('.file-picker-item.file').first().waitFor();
            await modal.locator('.file-picker-item').filter({hasText:imageName}).click();scroller=modal.locator('.file-picker-list');
        } else {
            modal=page.locator('#quick-log-modal');await page.waitForFunction(()=>document.getElementById('quick-log-content')?.textContent.includes('LOCAL SYNTHETIC LOG'));scroller=modal.locator('#quick-log-content');
        }
        for(const position of ['top','bottom']) {
            await scroller.evaluate((el,bottom)=>el.scrollTop=bottom?el.scrollHeight:0,position==='bottom');
            const state=await modal.evaluate(el=>{
                const content=el.querySelector('.modal-content'),box=content.getBoundingClientRect(),title=el.querySelector('h2,h3');
                return {innerWidth,innerHeight,overflow:document.documentElement.scrollWidth>innerWidth,
                    box:box.toJSON(),titleSize:getComputedStyle(title).fontSize,
                    unreachable:[...el.querySelectorAll('.modal-close,.modal-footer button,.form-actions button')].filter(e=>e.getClientRects().length).filter(e=>{
                        const r=e.getBoundingClientRect();return r.top<0||r.bottom>innerHeight;
                    }).map(e=>({id:e.id,text:e.textContent.slice(0,60)})),
                    clipped:[...el.querySelectorAll('input,select,button')].filter(e=>e.getClientRects().length).filter(e=>{
                        const r=e.getBoundingClientRect();return r.left<box.left-1||r.right>box.right+1;
                    }).map(e=>({id:e.id,text:e.textContent.slice(0,60)}))};
            });
            const screenshot='output/macos27-20260928/after/deep-layout-'+name+'-'+lang+'-'+width+'x'+height+'-'+position+'.png';
            const scroll=await scroller.evaluate(el=>({scrollTop:el.scrollTop,scrollHeight:el.scrollHeight,clientHeight:el.clientHeight}));
            await page.screenshot({path:screenshot});results.push({name,lang,position,screenshot,scroll,...state});
            if(state.overflow||state.clipped.length||state.unreachable.length||scroll.clientHeight<24)throw new Error(JSON.stringify({name,lang,width,scroll,...state}));
        }
        if(name.endsWith('variable')) {
            await modal.locator('#var-search').fill('fixture.selected');await modal.locator('.var-select-item[data-name="fixture.selected"]').click();await modal.waitFor({state:'detached'});
            if(name==='widget-variable'&&await page.locator('#edit-expression').inputValue()!=='${fixture.value}${fixture.selected}')throw new Error('Widget variable insertion callback failed');
        } else if(name==='image') {
            await modal.locator('.image-select-item').filter({hasText:imageName}).click();await modal.waitFor({state:'detached'});
            if(!((await page.locator('#action-led-image-path').inputValue()).endsWith(imageName)))throw new Error('Image callback failed');
        } else if(name==='file-picker') {
            await modal.locator('#file-picker-confirm').click();await modal.waitFor({state:'hidden'});
        } else {
            await modal.locator('.modal-close').click();await modal.waitFor({state:'detached'});
        }
    }
    if(errors.length)throw new Error(JSON.stringify(errors));
    return {results,errors,scope:'Local long-data fixtures; nested selector callbacks and close. No configuration saves, deployments or device/SSH access.'};
}
