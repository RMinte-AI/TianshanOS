async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        const open=async()=>{await page.setViewportSize({width,height:width===1440?1000:844});await page.goto(origin+'/?state=populated&lang='+lang+'#/files');await page.reload();await page.waitForTimeout(350);await page.evaluate(()=>scrollTo(0,0));};
        const capture=async name=>{
            results.push({name,lang,...await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
                columns:[...document.querySelectorAll('.file-table th')].map(e=>({text:e.textContent,visible:!!e.getClientRects().length})),
                rows:[...document.querySelectorAll('.file-row')].map(e=>({path:e.dataset.path,type:e.dataset.type,operations:[...e.querySelectorAll('button')].map(b=>b.className)})),
                inputs:[...document.querySelectorAll('.modal input')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,type:e.type,value:e.value})),
                localScroll:[...document.querySelectorAll('.file-list')].map(e=>({width:e.clientWidth,scrollWidth:e.scrollWidth}))}))});
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/files-'+name+'-'+lang+'-'+width+'.png'});
        };
        await open();await capture('list');
        await page.locator('#select-all-cb').check();await capture('selection');
        await page.locator('#select-all-cb').uncheck();
        await page.locator('[onclick="showUploadDialog()"]').click();await capture('upload');
        await open();await page.locator('[onclick="showNewFolderDialog()"]').click();await capture('new-folder');
        await open();await page.locator('.file-row .btn-rename').first().click();await capture('rename');
        await open();
        await page.locator('[onclick='+JSON.stringify("navigateToPath('/spiffs')")+']').click();await capture('spiffs');
    }
    return results;
}
