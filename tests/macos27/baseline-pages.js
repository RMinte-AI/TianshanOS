async (page) => {
    const results = [];
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    for (const language of ['zh-CN', 'en-US']) {
        for (const route of ['/', '/network', '/files', '/terminal', '/automation', '/commands', '/security', '/ota', '/logs']) {
            await page.setViewportSize({width:1440, height:1000});
            await page.goto(origin+'/?state=populated&lang='+language+'#'+route);
            await page.waitForTimeout(2300);
            const name = (route.slice(1)||'system')+'-'+language;
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/'+name+'-1440.png'});
            const state = await page.evaluate(() => ({url:location.href, innerWidth, innerHeight,
                overflow:document.documentElement.scrollWidth>innerWidth,
                headings:[...document.querySelectorAll('h1,h2,h3')].filter(e=>e.getClientRects().length).map(e=>e.textContent),
                controls:[...document.querySelectorAll('button,a,input,select,textarea')].filter(e=>e.getClientRects().length).map(e=>({tag:e.tagName,id:e.id,text:e.textContent.trim(),type:e.type,handler:e.getAttribute('onclick'),href:e.getAttribute('href'),disabled:e.disabled})),
                resources:performance.getEntriesByType('resource').map(e=>({url:e.name,bytes:e.transferSize})),
                language:document.documentElement.lang
            }));
            results.push({name,...state});
        }
    }
    return {results,errors};
}
