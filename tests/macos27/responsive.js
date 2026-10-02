async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    for(const width of [320,390,768,1024,1440,844]) for(const lang of ['zh-CN','en-US']) for(const route of ['/','/network','/files','/terminal','/automation','/commands','/security','/ota','/logs']) {
        await page.setViewportSize({width,height:width===844?390:900});
        await page.goto(origin+'/?state=populated&lang='+lang+'#'+route);await page.reload();
        await page.waitForTimeout(route==='/'?550:250);await page.evaluate(()=>scrollTo(0,0));
        const data=await page.evaluate(()=>({innerWidth,innerHeight,overflow:document.documentElement.scrollWidth>innerWidth,
            overflowElements:[...document.querySelectorAll('#page-content *')].filter(e=>e.getClientRects().length&&e.getBoundingClientRect().right>innerWidth+1&&!e.closest('.file-list,.data-table,.card-content')).slice(0,12).map(e=>({id:e.id,class:e.className,rect:e.getBoundingClientRect().toJSON()})),
            smallText:[...document.querySelectorAll('#page-content *')].filter(e=>e.getClientRects().length&&[...e.childNodes].some(n=>n.nodeType===3&&n.textContent.trim())&&parseFloat(getComputedStyle(e).fontSize)<12&&!e.closest('.xterm')).map(e=>({id:e.id,class:e.className,text:e.textContent.slice(0,60),size:getComputedStyle(e).fontSize})),
            controls:[...document.querySelectorAll('#page-content button,#page-content input,#page-content select')].map(e=>({id:e.id,handler:e.getAttribute('onclick'),type:e.type,disabled:e.disabled,visible:!!e.getClientRects().length})),
            nav:[...document.querySelectorAll('.nav-link')].map(e=>({href:e.getAttribute('href'),visible:!!e.getClientRects().length}))}));
        results.push({route,lang,...data});
        await page.screenshot({path:'output/macos27-20260928/'+phase+'/responsive-'+(route.slice(1)||'system')+'-'+lang+'-'+width+'.png'});
    }
    return {results,errors};
}
