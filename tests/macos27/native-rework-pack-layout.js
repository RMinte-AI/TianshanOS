async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local fixture only');
    const results=[],errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    for (const [width,height] of [[320,600],[390,600],[844,390],[1440,500]]) for (const lang of ['en-US','zh-CN']) {
        await page.setViewportSize({width,height});
        for (const [name,handler,id] of [['export','showConfigPackExportModal()','pack-export-modal'],['list','showConfigPackListModal()','pack-list-modal']]) {
            await page.goto(origin+'/?state=provisioned&lang='+lang+'#/security');
            await page.reload();
            await page.locator('[onclick='+JSON.stringify(handler)+']').click();
            const modal=page.locator('#'+id+' .modal-content');
            if (name==='list') await page.locator('#pack-list-table').waitFor();
            else await page.locator('#pack-export-file-list .loading').waitFor({state:'hidden'});
            for (const position of ['top','middle','bottom']) {
                await modal.evaluate((el,p)=>el.scrollTop=p==='top'?0:p==='middle'?(el.scrollHeight-el.clientHeight)/2:el.scrollHeight,position);
                const sample=await modal.evaluate(el=>({innerWidth,innerHeight,scrollY,scrollTop:el.scrollTop,
                    scrollHeight:el.scrollHeight,clientHeight:el.clientHeight,
                    rect:el.getBoundingClientRect().toJSON(),overflow:document.documentElement.scrollWidth>innerWidth,
                    actions:[...el.querySelectorAll('.form-actions button')].map(b=>({text:b.textContent,disabled:b.disabled,rect:b.getBoundingClientRect().toJSON()})),
                    path:el.querySelector('#pack-export-browse-path')?.getBoundingClientRect().toJSON()}));
                if (sample.overflow || sample.rect.top<0 || sample.rect.bottom>height+1 || (position==='bottom' && sample.actions.some(a=>a.rect.top<0||a.rect.bottom>height))) throw new Error('Pack layout failed '+JSON.stringify({name,position,...sample}));
                const screenshot='output/macos27-20260928/after/pack-layout-'+name+'-'+lang+'-'+width+'x'+height+'-'+position+'.png';
                await page.screenshot({path:screenshot});
                results.push({name,lang,position,screenshot,...sample});
            }
            if (name==='list') {
                const wrapper=page.locator('#pack-list-table').locator('..');
                await wrapper.evaluate(el=>el.scrollLeft=el.scrollWidth);
                const sample=await wrapper.evaluate(el=>({scrollLeft:el.scrollLeft,scrollWidth:el.scrollWidth,clientWidth:el.clientWidth,tableDisplay:getComputedStyle(el.querySelector('table')).display}));
                results.push({name,lang,width,height,position:'table-right',...sample});
            }
            await modal.locator('.form-actions button').first().click();
            if (await modal.isVisible()) throw new Error('Close failed: '+name);
        }
    }
    if(errors.length) throw new Error(JSON.stringify(errors));
    return {results,errors,scope:'Local layout and close checks; no configuration export, upload, import or device write'};
}
