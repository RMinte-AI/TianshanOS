async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local fixture only');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const handlers = ['showGenerateKeyModal()', "showDeployKeyModal('fixture-key')", "showRevokeKeyModal('fixture-key')",
        'showImportSshHostModal()', 'showCertGenKeyModal()', 'showCertCSRModal()', 'showCertInstallModal()',
        'showCertInstallCAModal()', 'showCertViewModal()', 'showConfigPackExportCertModal()',
        'showConfigPackImportModal()', 'showConfigPackExportModal()', 'showConfigPackListModal()'];
    const results = [], errors = [];
    page.on('pageerror',e=>errors.push(e.message));
    for (const width of [1440,390]) for (const handler of handlers) {
        await page.setViewportSize({width,height:900});
        await page.goto(origin+'/?state=provisioned&lang=en-US#/security');
        await page.reload();
        const button=page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first();
        await button.click();
        const modal=page.locator('.modal:visible').last();
        await modal.waitFor();
        await page.waitForTimeout(100);
        const record = await modal.evaluate(el=>({id:el.id,parent:el.parentElement.id||el.parentElement.tagName,
            withinSecurity:!!el.closest('.page-security'),innerWidth,innerHeight,scrollY,
            pageOverflow:document.documentElement.scrollWidth>innerWidth,
            content:el.querySelector('.modal-content').getBoundingClientRect().toJSON(),
            fields:[...el.querySelectorAll('h2,label,input,select,textarea,.form-group-hint')].filter(e=>e.getClientRects().length).map(e=>({
                tag:e.tagName,id:e.id,class:e.className,text:e.tagName==='INPUT'?'':e.textContent.slice(0,70),
                font:getComputedStyle(e).fontFamily,size:getComputedStyle(e).fontSize,lineHeight:getComputedStyle(e).lineHeight,
                oldScopedMatch:e.matches('.page-security .modal .modal-content input[type="text"], .page-security .modal .modal-content label, .page-security .modal .modal-content .form-group div, .page-security .modal .modal-content textarea')
            }))}));
        const screenshot='output/macos27-20260928/'+phase+'/security-scope-'+record.id+'-'+width+'.png';
        await page.screenshot({path:screenshot});
        results.push({handler,screenshot,...record});
    }
    return {results,errors,scope:'V20 actual mounting and computed typography; no submission or real device access'};
}
