async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    let width=1440;
    const results=[];
    const open=async(route,handler)=>{
        await page.goto(origin+'/?state=populated&lang=zh-CN#'+route);
        await page.reload(); await page.waitForTimeout(300);
        await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
        await page.waitForTimeout(200);
    };
    const capture=async(name)=>{
        const state=await page.evaluate(()=>({innerWidth,innerHeight,
            fields:[...document.querySelectorAll('.modal input,.modal select,.modal textarea')].map(e=>({id:e.id,name:e.name,type:e.type,value:e.value,min:e.min,max:e.max,step:e.step,disabled:e.disabled,visible:!!e.getClientRects().length})),
            handlers:[...document.querySelectorAll('.modal [onclick],.modal [onchange]')].map(e=>({id:e.id,click:e.getAttribute('onclick'),change:e.getAttribute('onchange'),visible:!!e.getClientRects().length})),
            dialogs:[...document.querySelectorAll('.modal')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,text:e.innerText})),
            smallText:[...document.querySelectorAll('.modal *')].filter(e=>e.getClientRects().length&&[...e.childNodes].some(n=>n.nodeType===3&&n.textContent.trim())&&parseFloat(getComputedStyle(e).fontSize)<12).map(e=>({id:e.id,class:e.className,text:e.textContent.slice(0,80),size:getComputedStyle(e).fontSize})),
            overflow:document.documentElement.scrollWidth>innerWidth
        }));
        await page.screenshot({path:'output/macos27-20260928/'+phase+'/nested-'+name+'-'+width+'.png'});
        results.push({name,...state});
    };
    for (width of [1440,390]) {
    await page.setViewportSize({width,height:width===1440?1000:844});
    for(const type of ['rest','websocket','socketio','variable']) {
        await open('/automation','showAddSourceModal()');
        await page.locator('[onclick='+JSON.stringify("switchSourceType('"+type+"')")+']').click();
        await capture('source-'+type);
    }
    for(const type of ['cli','ssh_cmd_ref','led','log','set_var','webhook']) {
        await open('/automation','showAddActionModal()');
        await page.locator('.action-type-card[data-type="'+type+'"]').click();
        await page.waitForTimeout(150);
        await capture('action-'+type);
        if(type==='led') {
            for (const device of ['board','touch','matrix']) {
                await page.locator('#action-led-device').selectOption(device);
                const select=page.locator(device==='matrix'?'#action-led-matrix-type':'#action-led-type');
                const options=await select.locator('option').evaluateAll(els=>els.map(e=>e.value));
                for (const value of options.filter(Boolean)) {
                    await select.selectOption(value);
                    await page.waitForTimeout(100);
                    await capture('action-led-'+device+'-'+value);
                }
            }
        }
    }
    for(const type of ['ring','gauge','temp','number','bar','text','status','icon','dual','percent','log']) {
        await open('/','showWidgetManager()');
        await page.locator('[onclick='+JSON.stringify("showWidgetEditPanel('fixture-"+type+"')")+']').click();
        await capture('widget-'+type);
    }
    await open('/automation','showAddRuleModal()');
    await page.locator('[onclick="addConditionRow()"]').click();
    await page.locator('[onclick="addActionTemplateRow()"]').click();
    await capture('rule-conditions-actions');
    await open('/network',"switchNetTab('wifi')");
    await capture('network-wifi');
    }
    return results;
}
