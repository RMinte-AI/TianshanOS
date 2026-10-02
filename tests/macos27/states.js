async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const role of ['root','admin','guest']) for(const lang of ['zh-CN','en-US']) for(const route of ['/','/network','/files','/terminal','/automation','/commands','/security','/ota']) {
        await page.setViewportSize({width:390,height:844});
        await page.goto(origin+'/?state=empty&role='+role+'&lang='+lang+'#'+route);await page.reload();await page.waitForTimeout(400);await page.evaluate(()=>scrollTo(0,0));
        const state=await page.evaluate(()=>({innerWidth,url:location.hash,overflow:document.documentElement.scrollWidth>innerWidth,
            nav:[...document.querySelectorAll('.nav-link')].map(e=>({href:e.getAttribute('href'),visible:!!e.getClientRects().length})),
            login:document.getElementById('login-modal')?.getAttribute('class'),username:document.getElementById('user-name')?.innerText,
            headings:[...document.querySelectorAll('#page-content h1,#page-content h2,#page-content h3')].map(e=>e.textContent),
            controls:[...document.querySelectorAll('#page-content button,#page-content input,#page-content select')].map(e=>({id:e.id,handler:e.getAttribute('onclick'),disabled:e.disabled,visible:!!e.getClientRects().length})),
            modals:[...document.querySelectorAll('.modal')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,text:e.innerText}))}));
        results.push({role,lang,route,...state});
        await page.screenshot({path:'output/macos27-20260928/'+phase+'/state-'+role+'-'+(route.slice(1)||'system')+'-'+lang+'-390.png'});
    }
    return results;
}
