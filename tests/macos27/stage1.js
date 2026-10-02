async (page) => {
    const evidence = [];
    for (const width of [1440,390]) {
        await page.setViewportSize({width,height:width===1440?1000:844});
        await page.goto('http://127.0.0.1:18783/?state=populated&lang=zh-CN#/');
        await page.reload();
        await page.waitForTimeout(2300);
        await page.evaluate(()=>scrollTo(0,0));
        for (const [name,handler] of [['system',null],['menu','toggleLanguageMenu()'],['memory','showMemoryDetailModal()'],['led',"openLedModal('matrix', 'colorcorrection')"]]) {
            if (handler) {
                await page.reload();
                await page.waitForTimeout(300);
                await page.evaluate(()=>scrollTo(0,0));
                await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
                await page.waitForTimeout(250);
            }
            await page.screenshot({path:'output/macos27-20260928/stage1/'+name+'-'+width+'.png'});
            evidence.push({name,...await page.evaluate(()=>({innerWidth,innerHeight,overflow:document.documentElement.scrollWidth>innerWidth,
                surfaces:[...document.querySelectorAll('.header,.modal-content,.lang-menu')].filter(e=>e.getClientRects().length).map(e=>({class:e.className,background:getComputedStyle(e).background,filter:getComputedStyle(e).backdropFilter,rect:{width:e.getBoundingClientRect().width,height:e.getBoundingClientRect().height}})),
                navigation:[...document.querySelectorAll('.nav-link')].map(e=>({text:e.textContent,visible:!!e.getClientRects().length})),
                overflowing:[...document.querySelectorAll('body *')].filter(e=>e.getClientRects().length&&e.getBoundingClientRect().right>innerWidth+1).slice(0,25).map(e=>({tag:e.tagName,id:e.id,class:e.className}))
            }))});
        }
    }
    return evidence;
}
