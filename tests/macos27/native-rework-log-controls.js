async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local fixture only');
    const results = [];
    for (const width of [320,390,1440]) for (const lang of ['en-US','zh-CN']) {
        await page.setViewportSize({width,height:800});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/');
        await page.reload();
        const card = page.locator('.dw-card[data-widget-id="fixture-log"]');
        await card.waitFor();
        const toggle = card.locator('.dw-log-collapse-btn');
        for (let step=0;step<3;step++) {
            if(step) await toggle.click();
            await card.scrollIntoViewIfNeeded();
            const state=await card.evaluate(el=>{
                const button=el.querySelector('.dw-log-collapse-btn');
                const icon=button.querySelector('i');
                const style=getComputedStyle(icon,'::before');
                const box=el.getBoundingClientRect();
                return {innerWidth,innerHeight,collapsed:el.querySelector('.dw-log-container').classList.contains('dw-log-collapsed'),
                    icon:icon.className,transform:style.transform,border:style.borderRightWidth,iconWidth:style.width,
                    title:button.title,overflow:document.documentElement.scrollWidth>innerWidth,
                    clipped:[...el.querySelectorAll('.dw-log-toolbar button,.dw-log-status')].filter(e=>{
                        const r=e.getBoundingClientRect();return r.left<box.left||r.right>box.right;
                    }).map(e=>e.outerHTML)};
            });
            if(state.overflow||state.clipped.length||state.border==='0px'||state.iconWidth==='0px')throw new Error(JSON.stringify(state));
            const screenshot='output/macos27-20260928/after/log-controls-'+lang+'-'+width+'-'+step+'.png';
            await card.screenshot({path:screenshot});
            results.push({lang,width,step,screenshot,...state});
        }
        const observed=results.filter(r=>r.lang===lang&&r.width===width);
        if(new Set(observed.map(r=>r.collapsed)).size!==2)throw new Error('Both log states not reached');
    }
    return {results,scope:'Local fixture only. Existing toggle and local persistence path exercised; no SSH reading and no device writes.'};
}
