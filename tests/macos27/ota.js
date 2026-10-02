async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        await page.setViewportSize({width,height:width===1440?1000:844});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/ota');await page.reload();await page.waitForTimeout(450);await page.evaluate(()=>scrollTo(0,0));
        for(const name of ['overview','manual']) {
            if(name==='manual')await page.locator('details.ota-section').last().locator('summary').click();
            if(name==='manual')await page.locator('#ota-file-input').scrollIntoViewIfNeeded();
            results.push({name,lang,...await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
                fields:[...document.querySelectorAll('.page-ota input')].map(e=>({id:e.id,type:e.type,value:e.value,checked:e.checked,disabled:e.disabled,visible:!!e.getClientRects().length})),
                controls:[...document.querySelectorAll('.page-ota button')].map(e=>({id:e.id,handler:e.getAttribute('onclick'),disabled:e.disabled})),
                text:document.querySelector('.page-ota').innerText}))});
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/ota-'+name+'-'+lang+'-'+width+'.png'});
        }
    }
    return results;
}
