async (page) => {
    const results = [];
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const allowed = /^(toggleLanguageMenu|show(?:ServicesModal|MemoryDetailModal|ShutdownSettingsModal|TimezoneModal|WidgetManager|FanCurveModal|FanAutoHelpModal|DhcpClients|UploadDialog|NewFolderDialog|AddSourceModal|ImportSourceModal|SourceVariables|ExportSourceModal|AddRuleModal|ImportRuleModal|ExportRuleModal|AddActionModal|ImportActionModal|ExportActionModal|ImportSshCommandModal|AddCommandModal|GenerateKeyModal|DeployKeyModal|RevokeKeyModal|ImportSshHostModal|CertGenKeyModal|CertCSRModal|CertInstallModal|CertInstallCAModal|CertViewModal|ConfigPackExportCertModal|ConfigPackImportModal|ConfigPackExportModal|ConfigPackListModal)|openLedModal|editRule|editAction)\(/;
    for (const route of ['/', '/network', '/files', '/automation', '/commands', '/security']) {
        await page.setViewportSize({width:1440,height:1000});
        const url = origin+'/?state=populated&lang=zh-CN#'+route;
        await page.goto(url);
        await page.waitForTimeout(2300);
        const handlers = await page.locator('[onclick]').evaluateAll(els=>[...new Set(els.filter(e=>e.getClientRects().length).map(e=>e.getAttribute('onclick')))]);
        for (const handler of handlers.filter(h=>allowed.test(h))) {
            await page.goto(url);
            await page.reload();
            await page.waitForTimeout(250);
            const exact = page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first();
            const id = 'surface-'+String(results.length+1).padStart(3,'0');
            try {
                await exact.click({timeout:4000});
                await page.waitForTimeout(350);
                const state = await page.evaluate(()=>({innerWidth,innerHeight,overflow:document.documentElement.scrollWidth>innerWidth,
                    modals:[...document.querySelectorAll('.modal')].filter(e=>e.getClientRects().length).map(e=>({id:e.id,text:e.innerText,
                        fields:[...e.querySelectorAll('input,select,textarea')].map(f=>({id:f.id,type:f.type,value:f.value,min:f.min,max:f.max,step:f.step,required:f.required,disabled:f.disabled,options:f.tagName==='SELECT'?[...f.options].map(o=>({text:o.text,value:o.value})):undefined})),
                        handlers:[...e.querySelectorAll('[onclick],[onchange]')].map(f=>({id:f.id,text:f.textContent.trim(),onclick:f.getAttribute('onclick'),onchange:f.getAttribute('onchange')}))})),
                    menu:document.getElementById('lang-menu')?.innerText}));
                await page.screenshot({path:'output/macos27-20260928/'+phase+'/'+id+'.png'});
                results.push({id,route,handler,status:'CAPTURED',...state});
            } catch(e) {results.push({id,route,handler,status:'FAIL',error:e.message});}
        }
    }
    return results;
}
