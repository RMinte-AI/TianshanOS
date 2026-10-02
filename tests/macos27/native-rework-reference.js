async (page) => {
    const results = [];
    const url = 'http://127.0.0.1:18781/docs/macos27/reference/approved-macos27-v3.html';
    for (const width of [1440,390]) {
        await page.setViewportSize({width,height:900});
        await page.goto(url);
        for (const name of ['page','内存详情','LED 设置']) {
            if (name!=='page') await page.locator('[data-action="'+name+'"]').first().click();
            const id = name==='page'?'page':name==='内存详情'?'memory':'led';
            const screenshot = 'output/macos27-20260928/after/reference-'+id+'-'+width+'.png';
            await page.screenshot({path:screenshot});
            results.push({name,url,screenshot,...await page.evaluate(()=>({innerWidth,innerHeight,devicePixelRatio,
                scale:visualViewport.scale,scrollY,overflow:document.documentElement.scrollWidth>innerWidth,
                dialog:document.querySelector('dialog')?.open,
                dialogBounds:document.querySelector('dialog')?.getBoundingClientRect().toJSON()}))});
            if (name!=='page') await page.locator('dialog [data-close]').first().click();
        }
    }
    return {results,scope:'Frozen visual reference; not production feature coverage'};
}
