async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Source fixture only');
    const results = [];
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    async function capture(name) {
        const state = await page.evaluate(() => {
            const visible = e => !!e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden';
            const box = e => { const r=e.getBoundingClientRect(); return {x:r.x,y:r.y,width:r.width,height:r.height,bottom:r.bottom,right:r.right}; };
            return {
                innerWidth,innerHeight,scale:visualViewport.scale,
                pageOverflow:document.documentElement.scrollWidth>innerWidth,
                dialogs:[...document.querySelectorAll('.modal:not(.hidden) .modal-content')].filter(visible).map(e=>({id:e.closest('.modal').id,rect:box(e),filter:getComputedStyle(e).backdropFilter,
                    title:[...e.querySelectorAll('h2')].map(h=>({text:h.textContent,size:getComputedStyle(h).fontSize,rect:box(h)})),
                    scrollers:[e,...e.querySelectorAll('*')].filter(n=>['auto','scroll'].includes(getComputedStyle(n).overflowY)&&n.scrollHeight>n.clientHeight+1).map(n=>({tag:n.tagName,class:n.className,id:n.id,scrollTop:n.scrollTop,scrollHeight:n.scrollHeight,height:n.clientHeight})),
                    buttons:[...e.querySelectorAll('button')].filter(visible).map(n=>({text:n.textContent.trim(),handler:n.getAttribute('onclick'),rect:box(n)}))
                }))
            };
        });
        const screenshot='output/macos27-20260929-native-rework/after/'+name+'.png';
        await page.screenshot({path:screenshot});
        results.push({name,screenshot,...state});
    }
    for (const lang of ['zh-CN','en-US']) for(const viewport of [{width:1440,height:500},{width:390,height:600},{width:844,height:390}]) {
        await page.setViewportSize(viewport);
        const suffix=lang+'-'+viewport.width+'x'+viewport.height;
        for(const [name,handler] of [['memory','showMemoryDetailModal()'],['led',"openLedModal('matrix', 'colorcorrection')"],['menu','toggleLanguageMenu()']]) {
            await page.goto(origin+'/?state=populated&lang='+lang+'#/');
            await page.reload();
            await page.waitForTimeout(500);
            await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
            await page.waitForTimeout(200);
            await capture('dialog-'+name+'-'+suffix);
        }
        await page.goto(origin+'/?state=populated&lang='+lang+'#/commands');
        await page.reload();
        await page.waitForTimeout(400);
        await page.locator('[data-host-id="fixture-host"]').click();
        await page.locator('[onclick="showAddCommandModal()"]').click();
        await page.locator('.advanced-options summary').click();
        await page.locator('#cmd-nohup').check();
        await page.locator('#cmd-service-mode').check();
        for (const [position,fraction] of [['top',0],['middle',0.5],['bottom',1]]) {
            await page.locator('#command-modal .modal-body').evaluate((e,f)=>{e.scrollTop=(e.scrollHeight-e.clientHeight)*f;},fraction);
            await capture('dialog-command-'+position+'-'+suffix);
        }
        const save=page.locator('[onclick="saveCommand()"]');
        const saveRect=await save.boundingBox();
        if(!saveRect || saveRect.y<0 || saveRect.y+saveRect.height>viewport.height) throw new Error('Save unreachable at bottom: '+suffix);
        await page.locator('#command-modal .modal-close').click();
        if(!await page.locator('#command-modal').evaluate(e=>e.classList.contains('hidden'))) throw new Error('Command close failed');
    }
    if(errors.length) throw new Error(JSON.stringify(errors));
    const failures=results.filter(r=>r.pageOverflow||r.dialogs.some(d=>d.rect.y<0||d.rect.bottom>r.innerHeight+1||d.rect.right>r.innerWidth+1));
    if(failures.length) throw new Error('Viewport bounds failed: '+failures.map(r=>r.name).join(', '));
    return {dataSource:'Local synthetic fixture; conditional fields changed locally, no save or device call',errors,results};
}
