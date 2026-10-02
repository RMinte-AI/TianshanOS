async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw Error('Local fixture required');
    const results = [], errors = [], writes = [], failures = [];
    page.on('pageerror', e => errors.push(e.message));
    page.on('request', r => {
        if (r.url().startsWith(origin + '/api/') && r.method() !== 'GET' && !r.url().endsWith('/auth/status')) writes.push({url:r.url(),method:r.method()});
    });
    const tables = ['keys-table-body','ssh-hosts-table-body','known-hosts-table-body'];
    let scenario = 'empty';
    const long = 'long_fixture_name_长名称_'.repeat(5);
    const keys = Array.from({length:4},(_,i)=>({id:'fixture-key-'+i,type:'ed25519',comment:long+i,alias:i===1?long:'',hidden:i===1,exportable:i!==2,has_pubkey:i!==3,created:1790611200}));
    const hosts = Array.from({length:4},(_,i)=>({id:'fixture-host-'+i+'-'+long,host:'192.0.2.'+(30+i),port:22,username:'fixture-user-'+i,keyid:'fixture-key-'+i}));
    const known = Array.from({length:4},(_,i)=>({host:'fixture-'+i+'.'+('long-host-label.'.repeat(4))+'invalid',port:22,type:'ssh-ed25519',fingerprint:'SHA256:'+'aB0/'.repeat(16),added:1790611200}));
    await page.route(origin+'/api/v1/**', async route => {
        const key = new URL(route.request().url()).pathname.replace('/api/v1/','');
        const replacements = {'key/list':{keys:scenario==='empty'?[]:keys},'ssh/hosts/list':{hosts:scenario==='empty'?[]:hosts},'hosts/list':{hosts:scenario==='empty'?[]:known}};
        if (Object.hasOwn(replacements,key)) return route.fulfill({json:{code:0,data:replacements[key]}});
        return route.continue();
    });
    for (scenario of ['long']) for (const lang of ['en-US']) for (const width of [320,390,1440]) {
        await page.setViewportSize({width,height:900});
        await page.goto(origin+'/?state=populated&role=root&lang='+lang+'#/security');
        await page.reload();
        await page.locator('#known-hosts-table-body tr').first().waitFor();
        await page.evaluate(()=>document.fonts.ready);
        const prefix='output/macos27-20260929-native-rework/after/security-keyboard-'+scenario+'-'+lang+'-'+width;
        const capture=[390,781,1440].includes(width);
        await page.evaluate(()=>scrollTo(0,0));
        const account=await page.evaluate(()=>({innerWidth,innerHeight,dpr:devicePixelRatio,scale:visualViewport.scale,scrollY,
            overflow:document.documentElement.scrollWidth>innerWidth,
            forms:[...document.querySelectorAll('.account-password-form')].map(e=>({columns:getComputedStyle(e).gridTemplateColumns,
                inputs:[...e.querySelectorAll('input')].map(n=>({id:n.id,rect:n.getBoundingClientRect().toJSON(),font:getComputedStyle(n).fontSize})),
                submit:[...e.querySelectorAll('.account-security-actions button')].map(n=>({handler:n.getAttribute('onclick'),rect:n.getBoundingClientRect().toJSON(),font:getComputedStyle(n).fontSize,background:getComputedStyle(n).backgroundColor}))}))}));
        if(capture)await page.screenshot({path:prefix+'-account.png'});
        for(const f of account.forms){
            const controls=[...f.inputs,...f.submit];
            if(controls.some(x=>x.rect.left<0||x.rect.right>width+1||parseFloat(x.font)<12))failures.push({scenario,lang,width,issue:'account control horizontal bounds or font'});
            for(let i=0;i<controls.length;i++)for(let j=i+1;j<controls.length;j++){
                const a=controls[i].rect,b=controls[j].rect;
                if(Math.min(a.right,b.right)-Math.max(a.left,b.left)>1&&Math.min(a.bottom,b.bottom)-Math.max(a.top,b.top)>1)failures.push({scenario,lang,width,issue:'account controls overlap'});
            }
        }
        const records=[];
        for(const id of tables){
            const body=page.locator('#'+id),owner=body.locator('xpath=ancestor::div[contains(@class,"security-table-scroll")]');
            await owner.evaluate(e=>e.scrollLeft=0);await body.locator('tr').first().scrollIntoViewIfNeeded();
            const state=await owner.evaluate(e=>{
                const t=e.querySelector('table'),row=t.tBodies[0].rows[0],r=e.getBoundingClientRect();
                return {scrollY,owner:r.toJSON(),clientWidth:e.clientWidth,scrollWidth:e.scrollWidth,tableDisplay:getComputedStyle(t).display,
                    rowCount:t.tBodies[0].rows.length,emptyColSpan:row.cells.length===1?row.cells[0].colSpan:null,
                    columns:row.cells.length===1?[]:[...t.tHead.rows[0].cells].map((th,i)=>({head:th.getBoundingClientRect().x,body:row.cells[i].getBoundingClientRect().x})),
                    controls:[...t.querySelectorAll('button')].map(n=>({handler:n.getAttribute('onclick'),disabled:n.disabled,text:n.textContent.trim(),font:getComputedStyle(n).fontSize})),
                    pageOverflow:document.documentElement.scrollWidth>innerWidth};
            });
            if(capture)await page.screenshot({path:prefix+'-'+id+'-left.png'});
            const actions=body.locator('button:enabled');state.reachable=[];
            for(let i=0;i<await actions.count();i++){
                const button=actions.nth(i);if(i===0)await button.focus();else await page.keyboard.press('Tab');await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
                state.reachable.push(await button.evaluate(n=>{const r=n.getBoundingClientRect(),o=n.closest('.security-table-scroll').getBoundingClientRect();return {rect:r.toJSON(),owner:o.toJSON(),scrollLeft:n.closest('.security-table-scroll').scrollLeft,handler:n.getAttribute('onclick'),visible:r.left>=o.left-1&&r.right<=o.right+1&&r.top>=0&&r.bottom<=innerHeight,focused:document.activeElement===n,outline:getComputedStyle(n).outline};}));
            }
            state.reverse=[];
            for(let i=await actions.count()-2;i>=0;i--){
                await page.keyboard.press('Shift+Tab');
                await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
                const reverse=await actions.nth(i).evaluate(n=>{const r=n.getBoundingClientRect(),o=n.closest('.security-table-scroll').getBoundingClientRect(),hit=document.elementFromPoint(r.x+r.width/2,r.y+r.height/2);return {handler:n.getAttribute('onclick'),focused:document.activeElement===n,rect:r.toJSON(),owner:o.toJSON(),unoccluded:hit===n||n.contains(hit),outlineVisible:r.left>=o.left+3&&r.right<=o.right-3&&r.top>=3&&r.bottom<=innerHeight-3,outline:getComputedStyle(n).outline};});
                state.reverse.push(reverse);
                if(!reverse.focused||!reverse.unoccluded||!reverse.outlineVisible)failures.push({scenario,lang,width,id,issue:'reverse Tab focus, outline or occlusion',reverse});
                if((id==='keys-table-body'&&i===2)||(id==='known-hosts-table-body'&&i===1))await page.screenshot({path:prefix+'-'+id+'-reverse-focus.png'});
            }

            await owner.evaluate(e=>e.scrollLeft=e.scrollWidth);
            await body.locator('tr').first().scrollIntoViewIfNeeded();
            // scrollIntoView may restore the start of a very wide row; restore local right position after vertical positioning.
            await owner.evaluate(e=>e.scrollLeft=e.scrollWidth);
            state.scrollRight=await owner.evaluate(e=>e.scrollLeft);
            if(capture)await page.screenshot({path:prefix+'-'+id+'-right.png'});
            if(state.pageOverflow||state.tableDisplay!=='table'||state.columns.some(c=>Math.abs(c.head-c.body)>1)||state.reachable.some(c=>!c.visible||!c.focused))failures.push({scenario,lang,width,id,issue:'table geometry or action reachability'});
            records.push({id,...state});
        }
        if(account.overflow)failures.push({scenario,lang,width,issue:'page overflow'});
        results.push({scenario,lang,width,prefix,captured:capture,account,tables:records});
    }
    await page.unrouteAll({behavior:'wait'});
    return {scope:'Candidate local synthetic empty SSH/host lists and four-row long/hidden/nonexportable/no-public-key states. Existing HTTPS missing-key row retained. Nine widths, two languages, root role. Focus exercises without invoking actions; visual review separate.',results,errors,writes,failures,status:errors.length||writes.length||failures.length?'FAIL':'PASS'};
}
