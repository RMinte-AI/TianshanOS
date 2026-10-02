async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    const long='LongName_长名称_'.repeat(18);
    for(const scenario of ['error','loading','long']) {
        await page.route('**/api/v1/**',async route=>{
            const url=route.request().url();
            if(/auth\/|config\/get/.test(url))return route.continue();
            if(scenario==='error')return route.fulfill({status:503,contentType:'application/json',body:JSON.stringify({code:7,message:'LOCAL FIXTURE: explicit unavailable response'})});
            if(scenario==='loading'){await page.waitForTimeout(1800);return route.continue();}
            const response=await route.fetch();
            if(!response.ok())return route.fulfill({response});
            const json=await response.json();
            const rewrite=obj=>{if(!obj||typeof obj!=='object')return;for(const [key,value]of Object.entries(obj)){if(typeof value==='string'&&['name','label','comment','description','version','subject_cn','ssid'].includes(key))obj[key]=long;else if(typeof value==='object')rewrite(value);}};
            rewrite(json);return route.fulfill({response,json});
        });
        for(const route of ['/','/network','/files','/automation','/commands','/security','/ota']) {
            await page.setViewportSize({width:390,height:844});
            await page.goto(origin+'/?state=populated&lang=en-US#'+route);await page.reload();
            await page.waitForTimeout(scenario==='loading'?150:400);await page.evaluate(()=>scrollTo(0,0));
            const state=await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,text:document.getElementById('page-content').innerText,
                errors:[...document.querySelectorAll('.error,.form-error,.result-box.error')].filter(e=>e.getClientRects().length).map(e=>e.textContent)}));
            results.push({scenario,route,...state});
            await page.screenshot({path:'output/macos27-20260928/'+phase+'/scenario-'+scenario+'-'+(route.slice(1)||'system')+'-390.png'});
            if(scenario==='loading')await page.waitForTimeout(2100);
        }
        await page.unrouteAll({behavior:'wait'});
    }
    return results;
}
