// Real production pages, local synthetic APIs only. Geometry proves the reported defects.
const {test, before, after} = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path'), fs = require('node:fs');
const {chromium} = require('../prompts/node_modules/playwright');
const L = require('../macos27-r2/lib.cjs');
const out = path.join(L.REPO, 'output/playwright/ui-correction');
let browser, fixture;
before(async () => {
    fs.mkdirSync(out, {recursive:true});
    fixture = await L.startFixture(process.env.TS_UI_LAYOUT_WEB || path.join(L.REPO,'components/ts_webui/web'), {extra:path.join(__dirname,'ui-correction-profile.json')});
    browser = await chromium.launch({channel:'chrome',headless:true});
});
after(async () => {await browser?.close();await fixture?.stop();});
async function tableTracks(page, root) {
    const tables = await page.locator(root+' .data-table').evaluateAll(tables=>tables.map(table=>{
        const rows=[...table.querySelectorAll('.tr')];
        const controls=[...table.querySelectorAll('.act')].flatMap(cell=>{const a=cell.getBoundingClientRect();return [...cell.querySelectorAll('button')].map(b=>{const r=b.getBoundingClientRect();return r.left>=a.left-.1&&r.right<=a.right+.1;});});
        return {controls,viewport:table.clientWidth,scroll:table.scrollWidth,rows:rows.map(row=>[...row.children].map(cell=>({x:cell.getBoundingClientRect().x,w:cell.getBoundingClientRect().width})))};
    }));
    assert(tables.length,root+' has tables');
    for(const table of tables) {
        assert(table.controls.every(Boolean),root+' action buttons fit their own column');
        assert(table.rows.length>=2,root+' includes header and data');
        for(const row of table.rows.slice(1)) for(let i=0;i<row.length;i++) {
            assert(Math.abs(row[i].x-table.rows[0][i].x)<.1,root+' column '+i+' x');
            assert(Math.abs(row[i].w-table.rows[0][i].w)<.1,root+' column '+i+' width');
        }
    }
}
for (const language of ['zh-CN','en-US']) test(language+': responsive shared components and modal ownership', async()=>{
    const page=await browser.newPage();page.setDefaultTimeout(5000);const errors=[];page.on('pageerror',e=>errors.push(e.message));
    const go=async route=>{await page.goto(`${fixture.origin}/?state=populated&lang=${language}#${route}`);await page.locator('.loading').waitFor({state:'detached'});};
    const shot=async name=>page.screenshot({path:path.join(out,`after-${name}-${language}.png`)});
    try {
        for(const width of [390,600,601,819,820,1200,1201,1440]) {
            await page.setViewportSize({width,height:1000});await go('/automation');
            await page.locator('#actions-list .tr:not(.th)').first().waitFor();
            const header=await page.evaluate(()=>({header:document.querySelector('.header').getBoundingClientRect().right,user:document.querySelector('.user-menu').getBoundingClientRect().right,body:document.documentElement.scrollWidth,width:innerWidth}));
            assert(header.user<=header.header-10,'user controls retain header padding at '+width);
            assert.equal(header.body,header.width,'tables scroll inside page at '+width);
            for(const id of ['sources-list','rules-list','actions-list']) await tableTracks(page,'#'+id);
            if(width===390) {
                await page.locator('#sources-list .data-table').evaluate(e=>{e.scrollLeft=e.scrollWidth;});
                await tableTracks(page,'#sources-list');
                assert(await page.locator('#sources-list .act button').last().isVisible(),'operations remain in the scrollable table');
                await page.locator('#sources-list .data-table').evaluate(e=>{e.scrollLeft=0;});
            }
            if(width===390||width===1440)await shot('automation-'+width);
            await page.locator('[onclick="showAddActionModal()\"]').click();
            await page.locator('#action-id').waitFor();
            const heights=await page.locator('.action-type-grid>.tile').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().height));
            assert(Math.max(...heights)-Math.min(...heights)<.1,'uniform action tile heights at '+width);
            if(width===390){
                for(const id of ['action-id','action-cli-command','action-cli-var']) {
                    const gap=await page.locator('#'+id).evaluate(e=>e.closest('.row').getBoundingClientRect().bottom-e.getBoundingClientRect().bottom);
                    assert(gap>=9,'wrapped control has bottom space: '+id);
                }
                await page.locator('#action-name').fill('A long action name / 很长的动作名称 '.repeat(8));
                await shot('action-'+width);
            }
            await page.keyboard.press('Escape');await page.locator('#action-modal').waitFor({state:'hidden'});
            await go('/commands');await page.locator('[data-host-id="fixture-host"]').click();
            const centered=await page.locator('.command-icon,.orphan-group .inline-icon').evaluateAll(es=>es.every(e=>{const a=e.getBoundingClientRect(),b=e.querySelector('svg').getBoundingClientRect();return Math.abs(b.top+b.height/2-a.top-a.height/2)<.1;}));
            assert(centered,'table and warning icons have no baseline offset');
            await page.locator('[onclick="showAddCommandModal()\"]').click();
            await page.locator('#command-modal:not(.hidden)').waitFor();
            const modal=await page.evaluate(()=>{
                const e=document.querySelector('#command-modal'), title=e.querySelector('.st'),r=title.getBoundingClientRect();
                return {parent:e.parentElement.tagName,hit:e.contains(document.elementFromPoint(r.x+8,r.y+8)),focus:e.contains(document.activeElement),icons:[...e.querySelectorAll('#icon-emoji-picker button')].map(e=>({x:e.getBoundingClientRect().x,y:e.getBoundingClientRect().y}))};
            });
            assert.equal(modal.parent,'BODY');assert(modal.hit,'title is above header');assert(modal.focus,'focus starts in modal');
            assert.equal(new Set(modal.icons.map(e=>e.y)).size,2);assert.equal(new Set(modal.icons.map(e=>e.x)).size,6);
            if(width===390||width===1440)await shot('command-'+width);
            await page.keyboard.press('Escape');await page.locator('#command-modal').waitFor({state:'hidden'});
            await page.locator('a[href="#/automation"]').click();await page.locator('#rules-list').waitFor();
            assert.equal(await page.locator('#command-modal').count(),0,'page-owned modal is removed on navigation');
        }
        await page.setViewportSize({width:390,height:1000});await go('/automation');
        await page.locator('[onclick="showAddSourceModal()\"]').click();await page.locator('#source-enabled').waitFor();
        assert.equal(await page.locator('#add-source-modal .sf #source-enabled').count(),0);
        const source=await page.locator('#source-type-tabs button').evaluateAll(es=>es.map(e=>({w:e.getBoundingClientRect().width,h:e.getBoundingClientRect().height})));
        assert(source.every(e=>e.w>140&&e.h>=34),'source segment labels have space');
        await shot('source-390');await page.keyboard.press('Escape');
        await page.locator('#add-source-modal').waitFor({state:'hidden'});
        await page.locator('[onclick="showAutomationConfig()\"]').click();await page.locator('[data-config-operation="backup"]').waitFor();
        assert.equal(await page.locator('.config-section').count(),2);
        assert.equal(await page.locator('[data-config-operation]').count(),4);
        await shot('config-390');await page.keyboard.press('Escape');
        await go('/network');
        const offsets=await page.locator('.tile-i').evaluateAll(es=>es.map(e=>{const a=e.getBoundingClientRect(),b=e.querySelector('svg').getBoundingClientRect();return b.top+b.height/2-a.top-a.height/2;}));
        assert(offsets.length===3&&offsets.every(x=>Math.abs(x)<.1),'network icons centered in containers');
        await shot('network-390');
        await page.evaluate(()=>showApStations());await tableTracks(page,'#ap-stations-section');
        await page.evaluate(()=>hideApStations());
        await page.evaluate(()=>showDhcpClients());await page.locator('#dhcp-clients-results .tr:not(.table-message)').first().waitFor();
        await tableTracks(page,'#dhcp-clients-section');await page.evaluate(()=>hideDhcpClients());
        await go('/files');await page.locator('#file-list .tr.th').waitFor();await tableTracks(page,'#file-list');
        await go('/security');await page.locator('#pack-info-details .kv').first().waitFor();
        for(const id of ['keys-table-body','ssh-hosts-table-body']) {
            const columns=await page.locator('#'+id).evaluate(body=>{const head=body.parentElement.querySelector('.th');return [...body.querySelectorAll('.tr:not(.table-message)')].map(row=>[...row.children].every((cell,i)=>Math.abs(cell.getBoundingClientRect().x-head.children[i].getBoundingClientRect().x)<.1));});assert(columns.length&&columns.every(Boolean),'security header/data alignment');
            const contained=await page.locator('#'+id+' .act').evaluateAll(es=>es.every(e=>{const a=e.getBoundingClientRect();return [...e.querySelectorAll('button')].every(b=>{const r=b.getBoundingClientRect();return r.left>=a.left-.1&&r.right<=a.right+.1;});}));assert(contained,'security actions do not overlap neighbouring values');
        }
        const values=await page.locator('#pack-info-details .kv').evaluateAll(es=>es.map(e=>{const s=e.lastElementChild,r=document.createRange();r.selectNodeContents(s);return {text:s.textContent,lines:r.getClientRects().length};}));
        assert(values.filter(v=>['Developer','fixture.invalid'].includes(v.text)).every(v=>v.lines===1),'short security values stay whole');
        await page.locator('#pack-info-details').scrollIntoViewIfNeeded();await shot('security-390');
        await go('/');await page.evaluate(()=>openLedModal('matrix','text'));await page.locator('#modal-text-x').waitFor();
        const xy=await page.locator('#modal-text-x,#modal-text-y').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().x));assert.equal(xy[0],xy[1]);
        await shot('led-390');
        await go('/automation');await page.locator('[onclick="showAddActionModal()"]').click();
        await page.evaluate(()=>configurationEditorRead('action-modal')({active_version:'different-version'}));
        assert(await page.locator('#action-modal .configuration-edit-note').isVisible(),'configuration-change warning is visible');
        assert(await page.locator('#action-modal .sf .primary').isDisabled(),'stale editor cannot overwrite a changed configuration');
        await go('/network');
        await page.evaluate(()=>{const body=openSourceVarsSheet('Variables');renderSourceVarsTable(body,[{name:'short',value:1,type:'number'},{name:'long.variable.name',value:'Long value',type:'string'}]);});
        await tableTracks(page,'#source-variables-body');
        await go('/automation');await page.locator('#rules-list').waitFor();
        assert(await page.locator('#source-variables-modal').isHidden(),'variable window closes with its page');
        await page.evaluate(()=>document.fonts.ready);
        const material=await page.evaluate(()=>({blur:getComputedStyle(document.querySelector('.m-surface'),'::before').backdropFilter,weight:getComputedStyle(document.querySelector('.foot .q')).fontWeight,loaded:document.fonts.check('400 13px Quantify')}));
        assert.equal(material.blur,'none');assert.equal(material.weight,'400');assert(material.loaded);
        assert.deepEqual(errors,[]);
    } finally {await page.close();}
});
for(const language of ['zh-CN','en-US']) test(language+': all eight pages at desktop and phone sizes',async()=>{
    const page=await browser.newPage();page.setDefaultTimeout(5000);const errors=[];page.on('pageerror',e=>errors.push(e.message));
    try {
        for(const width of [390,1440]) for(const route of ['/','/network','/files','/terminal','/automation','/commands','/security','/ota']) {
            await page.setViewportSize({width,height:1000});
            await page.goto(`${fixture.origin}/?state=populated&lang=${language}#${route}`);
            await page.locator('.loading').waitFor({state:'detached'});
            if(route==='/ota'&&width===390)assert(await page.locator('#ota-server-input').evaluate(e=>e.getBoundingClientRect().width>280),'OTA URL remains a full-width field');
            if(route==='/')await page.waitForFunction(()=>!document.getElementById('quick-actions-grid').textContent.includes(t('common.loading')));
            await page.screenshot({path:path.join(out,`after-page-${route.slice(1)||'system'}-${width}-${language}.png`)});
        }
        assert.deepEqual(errors,[]);
    } finally {await page.close();}
});
