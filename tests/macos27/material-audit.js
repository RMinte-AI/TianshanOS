async (page) => {
    const results=[];
    await page.setViewportSize({width:1440,height:1000});
    await page.goto('http://127.0.0.1:18783/?state=populated&lang=zh-CN#/');
    await page.reload(); await page.waitForTimeout(600);
    await page.locator('[onclick="showMemoryDetailModal()"]').click();
    await page.waitForTimeout(250);
    const selectors=['.memory-detail-modal .modal-header h2','.memory-detail-modal .memory-static-title','.memory-detail-modal .static-desc','.memory-detail-modal .static-label'];
    const text=await page.evaluate(selectors=>selectors.flatMap(selector=>[...document.querySelectorAll(selector)].filter(e=>e.getBoundingClientRect().bottom<innerHeight&&e.getBoundingClientRect().top>0).map(e=>{
        const s=getComputedStyle(e),r=e.getBoundingClientRect();return {selector,text:e.textContent,color:s.color,size:s.fontSize,weight:s.fontWeight,rect:{x:r.x,y:r.y,width:r.width,height:r.height}};
    })),selectors);
    await page.screenshot({path:'output/macos27-20260928/stage1/contrast-normal.png'});
    await page.screenshot({path:'output/macos27-20260928/stage1/contrast-background.png',style:selectors.join(',')+' {color: transparent !important;}'});
    results.push({name:'normal',text});
    for(const mode of ['more','no-preference']) {
        await page.emulateMedia({contrast:mode});
        const surface=await page.locator('.memory-detail-modal').evaluate(e=>({filter:getComputedStyle(e).backdropFilter,background:getComputedStyle(e).background}));
        results.push({name:'contrast-'+mode,surface});
        await page.screenshot({path:'output/macos27-20260928/stage1/fallback-'+mode+'.png'});
    }
    await page.emulateMedia({reducedMotion:'reduce',forcedColors:'active'});
    results.push({name:'forced-colors-reduced-motion',surface:await page.locator('.memory-detail-modal').evaluate(e=>({filter:getComputedStyle(e).backdropFilter,background:getComputedStyle(e).background,color:getComputedStyle(e).color,transition:getComputedStyle(e).transition}))});
    await page.screenshot({path:'output/macos27-20260928/stage1/fallback-forced.png'});
    await page.emulateMedia({reducedMotion:'no-preference',forcedColors:'none'});
    await page.setViewportSize({width:390,height:844});
    await page.reload(); await page.waitForTimeout(350);
    await page.locator('[onclick='+JSON.stringify("openLedModal('matrix', 'colorcorrection')")+']').click();
    await page.locator('.led-modal button').last().scrollIntoViewIfNeeded();
    results.push({name:'mobile-led-bottom',controls:await page.locator('.led-modal button').evaluateAll(els=>els.map(e=>({text:e.textContent,rect:e.getBoundingClientRect().toJSON(),disabled:e.disabled})))});
    await page.screenshot({path:'output/macos27-20260928/stage1/led-bottom-390.png'});
    await page.locator('.led-modal .modal-close').click();
    results.push({name:'mobile-led-close',visible:await page.locator('.led-modal').isVisible()});
    return results;
}
