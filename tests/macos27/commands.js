async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        await page.setViewportSize({width,height:width===1440?1000:844});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/commands');await page.reload();await page.waitForTimeout(450);
        const capture=async name=>{
            results.push({name,lang,...await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
                fields:[...document.querySelectorAll('#command-modal input,#command-modal select,#command-modal textarea')].map(e=>({id:e.id,type:e.type,value:e.value,checked:e.checked,disabled:e.disabled,visible:!!e.getClientRects().length})),
                controls:[...document.querySelectorAll('.command-card button')].map(e=>({onclick:e.getAttribute('onclick'),disabled:e.disabled})),
                smallText:[...document.querySelectorAll('#command-modal *')].filter(e=>e.getClientRects().length&&[...e.childNodes].some(n=>n.nodeType===3&&n.textContent.trim())&&parseFloat(getComputedStyle(e).fontSize)<12).map(e=>({id:e.id,class:e.className,text:e.textContent.slice(0,80),size:getComputedStyle(e).fontSize}))}))});
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/commands-'+name+'-'+lang+'-'+width+'.png'});
        };
        await page.locator('[data-host-id="fixture-host"]').click();await capture('host');
        await page.locator('[onclick="showAddCommandModal()"]').click();await capture('add');
        await page.locator('.advanced-options summary').click();await page.locator('#cmd-expect-pattern').fill('fixture');await capture('matching');
        await page.locator('#cmd-nohup').check();await page.locator('#cmd-service-mode').check();await page.locator('#cmd-ready-interval').scrollIntoViewIfNeeded();await capture('service');
        await page.locator('[onclick="closeCommandModal()"]').filter({visible:true}).first().click();
        await page.locator('[onclick="editCommand(0)"]').click();await capture('edit');
        await page.locator('[onclick="closeCommandModal()"]').filter({visible:true}).first().click();
        await page.locator('[data-host-id="__orphan__"]').click();await capture('orphan');
    }
    return results;
}
