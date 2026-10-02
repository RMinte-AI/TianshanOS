async (page) => {
    const origin=new URL(page.url()).origin;
    if(origin!=='http://127.0.0.1:18783') throw new Error('Local fixture only');
    const results=[],errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    for(const [width,height] of [[320,600],[390,600],[768,600],[1440,600],[844,390]]) for(const lang of ['en-US','zh-CN']) {
        await page.setViewportSize({width,height});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/');await page.reload();
        await page.locator('.dw-card').first().waitFor();
        await page.locator('[onclick="showWidgetManager()"]').click();
        const body=page.locator('.dw-manager-body'),modal=page.locator('.dw-manager-modal');
        for(const name of ['add','ring','gauge','temp','number','bar','text','status','icon','dual','percent','log']) {
            const handler=name==='add'?'showAddWidgetPanel()':"showWidgetEditPanel('fixture-"+name+"')";
            await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
            const pane=page.locator('#dw-manager-main');
            await pane.scrollIntoViewIfNeeded();
            const geometry=await pane.evaluate(el=>{
                const parent=el.closest('.modal-content').getBoundingClientRect();
                return {innerWidth,innerHeight,pane:el.getBoundingClientRect().toJSON(),modal:parent.toJSON(),
                    overflow:document.documentElement.scrollWidth>innerWidth,
                    clipped:[...el.querySelectorAll('input:not([type="hidden"]),select,textarea,button')].filter(e=>e.getClientRects().length).filter(e=>{const r=e.getBoundingClientRect();return r.left<parent.left-1||r.right>parent.right+1}).map(e=>({id:e.id,text:e.textContent.slice(0,50)}))};
            });
            if(geometry.overflow||geometry.clipped.length||geometry.pane.right>geometry.modal.right+1)throw new Error('Widget clipping '+JSON.stringify({name,lang,...geometry}));
            for(const position of ['editor','bottom']) {
                if(position==='bottom')await body.evaluate(el=>el.scrollTop=el.scrollHeight);
                const screenshot='output/macos27-20260928/after/widget-layout-'+name+'-'+lang+'-'+width+'x'+height+'-'+position+'.png';
                await page.screenshot({path:screenshot});
                results.push({name,lang,position,screenshot,...geometry,scrollTop:await body.evaluate(el=>el.scrollTop)});
            }
        }
        await modal.locator('.modal-close').click();
        await modal.waitFor({state:'detached',timeout:2000});
    }
    if(errors.length)throw new Error(JSON.stringify(errors));
    return {results,errors,scope:'Local add and all eleven existing fixture widget editor layouts, original entry clicks and close. No widget save, reorder or preference change.'};
}
