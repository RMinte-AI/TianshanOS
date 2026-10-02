async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for (const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        await page.setViewportSize({width,height:width===1440?1000:844});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/network');
        await page.reload();await page.waitForTimeout(450);await page.evaluate(()=>scrollTo(0,0));
        for(const [name,handler] of [['ethernet',null],['wifi',"switchNetTab('wifi')"],['scan','showWifiScan()'],['stations','showApStations()'],['ap-config','showApConfig()']]) {
            if(handler) await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
            await page.waitForTimeout(150);
            if(name==='scan')await page.locator('#wifi-scan-section').scrollIntoViewIfNeeded();
            if(name==='stations')await page.locator('#ap-stations-section').scrollIntoViewIfNeeded();
            const state=await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
                fields:[...document.querySelectorAll('#page-content input,#page-content select')].map(e=>({id:e.id,type:e.type,value:e.value,disabled:e.disabled,visible:!!e.getClientRects().length})),
                overflowElements:[...document.querySelectorAll('#page-content *')].filter(e=>e.getClientRects().length&&e.getBoundingClientRect().right>innerWidth+1).slice(0,15).map(e=>({id:e.id,class:e.className,rect:e.getBoundingClientRect().toJSON()}))}));
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/network-'+name+'-'+lang+'-'+width+'.png'});
            results.push({name,lang,...state});
        }
    }
    return results;
}
