async (page) => {
    const origin=new URL(page.url()).origin;
    if(origin!=='http://127.0.0.1:18783')throw new Error('Local fixture only');
    const results=[];
    for(const mode of ['normal','contrast','transparency','motion','forced','no-filter'])for(const width of [390,1440]) {
        const context=await page.context().browser().newContext({viewport:{width,height:600}});
        const p=await context.newPage(),cdp=await context.newCDPSession(p),errors=[];
        p.on('pageerror',error=>errors.push(String(error)));
        const features={contrast:{name:'prefers-contrast',value:'more'},transparency:{name:'prefers-reduced-transparency',value:'reduce'},motion:{name:'prefers-reduced-motion',value:'reduce'},forced:{name:'forced-colors',value:'active'}};
        if(features[mode])await cdp.send('Emulation.setEmulatedMedia',{features:[features[mode]]});
        if(mode==='no-filter')await p.route('**/*',async route=>{
            const request=route.request();
            if(!['document','stylesheet'].includes(request.resourceType()))return route.continue();
            const response=await route.fetch();const text=await response.text();
            const condition='@supports ((backdrop-filter: blur(1px)) or (-webkit-backdrop-filter: blur(1px)))';
            await route.fulfill({response,body:text.replaceAll(condition,'@supports (codex-unsupported-property: 1)')});
        });
        try {
            for(const entry of ['memory','led','timezone']) {
                await p.goto(origin+'/?state=populated&lang=en-US#/');await p.reload();
                const handler={memory:'showMemoryDetailModal()',led:"openLedModal('matrix', 'colorcorrection')",timezone:'showTimezoneModal()'}[entry];
                await p.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();await p.evaluate(()=>document.fonts.ready);
                const modal=p.locator({memory:'#memory-detail-modal',led:'#led-modal',timezone:'#timezone-modal'}[entry]);
                if(entry==='memory')await modal.locator('.memory-history').waitFor();
                if(entry==='led')await p.waitForFunction(()=>ccInitialConfig!==null);
                if(entry==='timezone')await modal.locator('#timezone-select').waitFor();
                const readiness=await modal.evaluate(e=>({text:e.textContent,loading:!!e.querySelector('.loading')}));
                if(readiness.loading)throw new Error('Unresolved modal loading state: '+entry);
                for(const position of ['top','middle','bottom']) {
                    await modal.evaluate((e,position)=>{const scrollers=[...e.querySelectorAll('*')].filter(n=>['auto','scroll'].includes(getComputedStyle(n).overflowY)&&n.scrollHeight>n.clientHeight+1);for(const n of scrollers)n.scrollTop=(n.scrollHeight-n.clientHeight)*({top:0,middle:0.5,bottom:1}[position]);},position);
                    const state=await modal.evaluate(e=>{
                        const shell=e.querySelector('.modal-content'),s=getComputedStyle(shell);
                        return {innerWidth,innerHeight,overflow:document.documentElement.scrollWidth>innerWidth,background:s.backgroundColor,filter:s.backdropFilter,color:s.color,
                            media:{contrast:matchMedia('(prefers-contrast: more)').matches,transparency:matchMedia('(prefers-reduced-transparency: reduce)').matches,motion:matchMedia('(prefers-reduced-motion: reduce)').matches,forced:matchMedia('(forced-colors: active)').matches},
                            buttons:[...e.querySelectorAll('button')].filter(n=>n.getClientRects().length).map(n=>({text:n.textContent,handler:n.getAttribute('onclick'),rect:n.getBoundingClientRect().toJSON(),transition:getComputedStyle(n).transitionDuration,color:getComputedStyle(n).color,background:getComputedStyle(n).backgroundColor}))};
                    });
                    const screenshot='output/macos27-20260928/after/fallback-'+mode+'-'+entry+'-'+width+'-'+position+'.png';await p.screenshot({path:screenshot,animations:'disabled'});results.push({mode,entry,width,position,screenshot,contentReady:true,...state});
                    if(['contrast','transparency','no-filter'].includes(mode)&&state.background!=='rgb(245, 245, 247)')throw new Error('Fallback surface must be opaque: '+mode+' '+state.background);
                    if(state.overflow)throw new Error('Fallback overflow '+JSON.stringify({mode,entry,width}));
                    if(mode==='motion'&&state.buttons.some(b=>b.transition.split(',').some(t=>parseFloat(t)!==0)))throw new Error('Control transition remains under reduced motion');
                    if(features[mode]&&!state.media[mode])throw new Error('Preference emulation did not apply: '+mode);
                    if(['contrast','transparency','forced','no-filter'].includes(mode)&&state.filter!=='none')throw new Error('Opaque fallback still filters: '+mode);
                }
                const close=modal.locator('.modal-close');if(await close.count()){await close.click();await modal.waitFor({state:'hidden'});}
            }
            if(errors.length)throw new Error('Browser runtime errors: '+JSON.stringify(errors));
        } finally {await context.close();}
    }
    return {scope:'Isolated Chromium media emulation; no OS preference changes. No-filter forces the identical supports condition false in both HTML critical CSS and main CSS; opaque surface asserted. Not a genuine old-engine run. Synthetic data, no saves.',results};
}
