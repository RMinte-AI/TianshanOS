async (page) => {
    const origin=new URL(page.url()).origin;
    if(origin!=='http://127.0.0.1:18783')throw new Error('Local fixture only');
    const results=[];
    for(const lang of ['en-US','zh-CN'])for(const [width,height] of [[390,600],[844,390]]) {
        await page.setViewportSize({width,height});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/');await page.reload();
        await page.locator('#system-led-cc-btn').click();
        await page.evaluate(()=>document.fonts.ready);
        const modal=page.locator('#led-modal'),body=modal.locator('.modal-body');
        const close=modal.locator('.modal-close');
        const icon=await close.evaluate(e=>{
            const i=e.querySelector('i'),c=getComputedStyle(i),p=getComputedStyle(i,'::before');
            return {button:e.getBoundingClientRect().toJSON(),icon:i.getBoundingClientRect().toJSON(),font:c.fontFamily,size:c.fontSize,color:c.color,display:c.display,visibility:c.visibility,opacity:c.opacity,before:{content:p.content,font:p.fontFamily,size:p.fontSize,display:p.display,color:p.color}};
        });
        await close.screenshot({path:'output/macos27-20260928/after/led-close-'+lang+'-'+width+'.png'});
        for(const [position,fraction] of [['top',0],['middle',0.5],['bottom',1]]) {
            await body.evaluate((e,f)=>e.scrollTop=(e.scrollHeight-e.clientHeight)*f,fraction);
            const buttons=await modal.locator('button').evaluateAll(es=>es.map(e=>({text:e.textContent,handler:e.getAttribute('onclick'),rect:e.getBoundingClientRect().toJSON()})));
            const screenshot='output/macos27-20260928/after/led-scroll-'+lang+'-'+width+'-'+position+'.png';
            await page.screenshot({path:screenshot});results.push({lang,width,height,position,screenshot,icon,buttons});
            if(position==='bottom') {
                const save=buttons.find(b=>b.handler==='applyColorCorrection()');
                if(!save||save.rect.y<0||save.rect.bottom>height)throw new Error('LED save unreachable at bottom');
            }
        }
        await close.click();await modal.waitFor({state:'hidden'});
    }
    return {scope:'Local synthetic LED state; no sliders changed or hardware/configuration writes. Scroll, icon metrics, closure and save reachability only.',results};
}
