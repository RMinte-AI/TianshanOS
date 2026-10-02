async (page) => {
    const origin=new URL(page.url()).origin;
    if(origin!=='http://127.0.0.1:18783')throw new Error('Local fixture only');
    const rows=[];
    for(const [route,selector,name] of [['/files','.file-name.clickable','folder-link'],['/files','.storage-info .mounted','mounted'],['/files','.storage-tabs .tab-btn.active','storage-tab'],['/network','.panel-tab.active','network-tab']]) {
        await page.setViewportSize({width:1440,height:1000});await page.goto(origin+'/?state=populated&lang=en-US#'+route);await page.reload();
        const el=page.locator(selector).first();await el.waitFor();await page.evaluate(()=>document.fonts.ready);await el.scrollIntoViewIfNeeded();
        for(const state of ['normal','hover','focus']) {
            await page.mouse.move(1400,990);await el.evaluate(e=>e.blur());
            if(state==='hover')await el.hover();
            if(state==='focus') {
                if(!await el.evaluate(e=>e.matches('button,a,input,select,textarea,[tabindex]')))continue;
                await el.focus();await page.keyboard.press('Tab');await page.keyboard.press('Shift+Tab');
            }
            await page.waitForTimeout(250);
            const data=await el.evaluate(e=>{const c=getComputedStyle(e),range=document.createRange();range.selectNodeContents(e);const r=range.getBoundingClientRect();return {color:c.color,size:c.fontSize,opacity:c.opacity,text:e.textContent,outline:c.outline,rect:{x:r.x,y:r.y,width:r.width,height:r.height}};});
            const id=name+'-'+state,base='output/macos27-20260929-native-rework/role-contrast/after/control-'+id;
            await el.evaluate(e=>e.setAttribute('data-contrast-target','true'));
            await page.screenshot({path:base+'.png'});
            await page.screenshot({path:base+'-background.png',style:'[data-contrast-target], [data-contrast-target] * {color:transparent !important;transition:none !important;}'});
            await el.evaluate(e=>e.removeAttribute('data-contrast-target'));rows.push({name,state,id,...data});
        }
    }
    require('node:fs').writeFileSync('output/macos27-20260929-native-rework/role-contrast/control-contrast.json',JSON.stringify(rows,null,2));
    return rows;
}
