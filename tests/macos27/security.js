async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        for(const [name,handler]of [['provisioned',null],['replace-key','showCertGenKeyModal()'],['csr','showCertCSRModal()'],['install','showCertInstallModal()'],['ca','showCertInstallCAModal()'],['view','showCertViewModal()'],['pack-list','showConfigPackListModal()']]) {
            await page.setViewportSize({width,height:width===1440?1000:844});
            await page.goto(origin+'/?state=provisioned&lang='+lang+'#/security');await page.reload();await page.waitForTimeout(450);await page.evaluate(()=>scrollTo(0,0));
            if(handler)await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
            await page.waitForTimeout(150);
            if(name==='install')await page.locator('#cert-install-submit').click(); // empty validation; no API write
            const state=await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
                dialogs:[...document.querySelectorAll('.modal')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,text:e.innerText})),
                fields:[...document.querySelectorAll('.modal input,.modal textarea,.modal select')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,type:e.type,value:e.value,disabled:e.disabled})),
                controls:[...document.querySelectorAll('.page-security button')].filter(e=>!e.closest('.modal')).map(e=>({id:e.id,handler:e.getAttribute('onclick'),disabled:e.disabled}))}));
            results.push({name,lang,...state});
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/security-'+name+'-'+lang+'-'+width+'.png'});
        }
    }
    return results;
}
