async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Source fixture only');
    const out = 'output/macos27-20260929-native-rework/after/';
    const results = [];
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    async function record(name) {
        const state = await page.evaluate(() => {
            const visible = e => !!e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden';
            const rect = e => { const r = e.getBoundingClientRect(); return {x:r.x,y:r.y,width:r.width,height:r.height}; };
            const controls = [...document.querySelectorAll('.account-password-form input,.account-security-actions button,.modal:not(.hidden) input,.modal:not(.hidden) select,.modal:not(.hidden) button')].filter(visible);
            return {
                innerWidth,innerHeight,scrollY,scale:visualViewport.scale,
                pageOverflow:document.documentElement.scrollWidth > innerWidth,
                tables:[...document.querySelectorAll('.security-table-scroll')].map(e=>({width:e.clientWidth,scrollWidth:e.scrollWidth,tableDisplay:getComputedStyle(e.querySelector('table')).display})),
                controls:controls.map(e=>({id:e.id,text:e.textContent.trim(),type:e.type,handler:e.getAttribute('onclick'),rect:rect(e),font:getComputedStyle(e).font,fontSize:getComputedStyle(e).fontSize,radius:getComputedStyle(e).borderRadius,background:getComputedStyle(e).backgroundColor})),
                dialogs:[...document.querySelectorAll('.modal:not(.hidden) .modal-content')].filter(visible).map(e=>({rect:rect(e),filter:getComputedStyle(e).backdropFilter,background:getComputedStyle(e).backgroundColor,scrollHeight:e.scrollHeight,clientHeight:e.clientHeight}))
            };
        });
        const screenshot = out + name + '.png';
        await page.screenshot({path:screenshot});
        results.push({name,screenshot,...state});
    }
    for (const lang of ['zh-CN','en-US']) {
        for (const width of [320,390,768,780,781,820,900,1024,1440]) {
            await page.setViewportSize({width,height:900});
            await page.goto(origin+'/?state=provisioned&lang='+lang+'#/security');
            await page.reload();
            await page.waitForTimeout(400);
            await page.evaluate(()=>scrollTo(0,0));
            await record('sample-account-'+lang+'-'+width);
            await page.locator('#keys-table-body').scrollIntoViewIfNeeded();
            await record('sample-tables-'+lang+'-'+width);
        }
        for (const width of [390,1440]) {
            await page.setViewportSize({width,height:600});
            await page.goto(origin+'/?state=populated&lang='+lang+'#/');
            await page.reload();
            await page.waitForTimeout(400);
            await page.locator('[onclick="showTimezoneModal()"]').click();
            await record('sample-timezone-'+lang+'-'+width);
            const options = await page.locator('#timezone-select option').evaluateAll(es=>es.map(e=>e.value));
            if (JSON.stringify(options)!==JSON.stringify(['CST-8','JST-9','KST-9','UTC0','GMT0','EST5EDT','PST8PDT','CET-1CEST'])) throw new Error('Timezone options changed');
            await page.locator('[onclick="hideTimezoneModal()"]').click();
            if (!await page.locator('#timezone-modal').evaluate(e=>e.classList.contains('hidden'))) throw new Error('Cancel did not close');
        }
    }
    if (errors.length) throw new Error(JSON.stringify(errors));
    if (results.some(r=>r.pageOverflow)) throw new Error('Page overflow: '+results.filter(r=>r.pageOverflow).map(r=>r.name).join(', '));
    return {dataSource:'Local synthetic fixture; no device access',errors,results};
}
