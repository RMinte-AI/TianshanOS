async (page) => {
    const origin=new URL(page.url()).origin;
    if(origin!=='http://127.0.0.1:18783')throw new Error('Local fixture only');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[],errors=[];page.on('pageerror',e=>errors.push(e.message));
    for(const width of [320,390,768,1024,1440])for(const lang of ['zh-CN','en-US'])for(const route of ['/network','/files']) {
        await page.setViewportSize({width,height:800});
        await page.goto(origin+'/?state=populated&lang='+lang+'#'+route);await page.reload();
        const selector=route==='/network'?'.panel-tab':'.storage-tabs .tab-btn';
        await page.locator(selector).first().waitFor();await page.evaluate(()=>document.fonts.ready);
        for(const index of [0,1]) {
            const tab=page.locator(selector).nth(index);await tab.click();
            await page.waitForFunction(({selector,index})=>document.querySelectorAll(selector)[index]?.classList.contains('active'),{selector,index});
            await tab.focus();await page.keyboard.press('Tab');await page.keyboard.press('Shift+Tab');
            // Existing 200ms control transitions must settle before style/screenshot sampling.
            await page.waitForTimeout(300);
            const state=await tab.evaluate(e=>{const c=getComputedStyle(e);return {text:e.textContent,font:c.fontFamily,bodyFont:getComputedStyle(document.body).fontFamily,size:c.fontSize,height:e.getBoundingClientRect().height,outline:c.outline,focusVisible:e.matches(':focus-visible'),active:e.classList.contains('active'),overflow:document.documentElement.scrollWidth>innerWidth};});
            const focusClips=await tab.evaluate(e=>{
                const c=getComputedStyle(e),r=e.getBoundingClientRect(),extent=parseFloat(c.outlineWidth)+parseFloat(c.outlineOffset),clips=[];
                for(let p=e.parentElement;p;p=p.parentElement){const q=p.getBoundingClientRect(),s=getComputedStyle(p);if((/hidden|clip|scroll|auto/.test(s.overflowX)&&(r.left-extent<q.left-1||r.right+extent>q.right+1))||(/hidden|clip|scroll|auto/.test(s.overflowY)&&(r.top-extent<q.top-1||r.bottom+extent>q.bottom+1)))clips.push(p.className||p.tagName);}
                return clips;
            });
            if(phase!=='before'&&focusClips.length)throw new Error('Focus outline clipped: '+JSON.stringify(focusClips));
            const screenshot='output/macos27-20260928/'+phase+'/tabs-'+route.slice(1)+'-'+lang+'-'+width+'-'+index+'.png';
            await page.screenshot({path:screenshot});results.push({route,lang,width,index,screenshot,focusClips,...state});
            if(!state.active||state.overflow)throw new Error(JSON.stringify(state));
            if(phase!=='before'&&(state.size!=='13px'||state.height!==32||!state.focusVisible||state.outline!=='rgb(0, 100, 210) solid 2px'))throw new Error('Tab role mismatch: '+JSON.stringify(state));
            if(route==='/network'&&index===1) {
                const mode=await page.locator('.wifi-mode-selector select').evaluate(e=>({height:e.getBoundingClientRect().height,size:getComputedStyle(e).fontSize,value:e.value,options:[...e.options].map(o=>({value:o.value,text:o.textContent}))}));
                results[results.length-1].mode=mode;
                if(phase!=='before'&&(mode.height<38||mode.size!=='14px'))throw new Error('WiFi select role mismatch: '+JSON.stringify(mode));
            }
        }
    }
    if(errors.length)throw new Error(JSON.stringify(errors));
    return {scope:'Local synthetic network/file tab navigation, keyboard focus and computed style. No real device calls.',results,errors};
}
