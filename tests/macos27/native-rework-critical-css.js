async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local fixture only');
    const results=[];
    for (const width of [320,390,768,1024,1440]) {
        const context=await page.context().browser().newContext({viewport:{width,height:900}});
        const probe=await context.newPage();
        let release;
        const gate=new Promise(resolve=>release=resolve);
        await probe.route('**/js/**',route=>new URL(route.request().url()).pathname.startsWith('/js/lang/') ? route.continue() : route.abort());
        await probe.route('**/css/style.css*',async route=>{await gate;await route.continue();});
        try {
            await probe.goto(origin+'/?state=populated#/',{waitUntil:'domcontentloaded'});
            await probe.evaluate(()=>window.languageReady);
            await probe.evaluate(()=>document.fonts.ready);
            if(await probe.locator('#language-status').isVisible())throw new Error('Language initialization failed in isolated shell');
            const read=()=>probe.evaluate(()=>Object.fromEntries(['.header','.logo','.nav','.nav-link','.user-menu','.lang-btn','#login-btn','.main','#page-content','.footer','.loading'].map(selector=>{
                const e=document.querySelector(selector),r=e.getBoundingClientRect(),c=getComputedStyle(e);
                return [selector,{x:r.x,y:r.y,width:r.width,height:r.height,fontSize:c.fontSize,lineHeight:c.lineHeight,padding:c.padding,background:c.backgroundColor,color:c.color}];
            })));
            const before=await read();
            const prefix='output/macos27-20260928/after/critical-css-'+width;
            await probe.screenshot({path:prefix+'-before.png'});
            release();
            await probe.waitForFunction(()=>[...document.styleSheets].some(s=>s.href?.includes('/css/style.css')));
            await probe.waitForTimeout(250);
            const after=await read();
            await probe.screenshot({path:prefix+'-after.png'});
            const changes=[];
            for(const selector of Object.keys(before))for(const key of Object.keys(before[selector]))if(before[selector][key]!==after[selector][key])changes.push({selector,key,before:before[selector][key],after:after[selector][key]});
            results.push({width,height:900,before,after,changes});
            if(changes.length)throw new Error('Critical shell style drift: '+JSON.stringify({width,changes}));
        } finally {release();await context.close();}
    }
    return {scope:'Isolated shell first-paint experiment: app JS intentionally blocked; real language loader allowed, main stylesheet delayed then released. Does not prove dynamic pages or loading performance.',results};
}
