async (page) => {
    const origin = new URL(page.url()).origin;
    if (origin !== 'http://127.0.0.1:18783') throw new Error('Local source fixture only');
    const results = [], errors = [];
    page.on('pageerror', e => errors.push(e.message));
    for (const width of [320, 390, 768, 1024, 1440]) for (const lang of ['zh-CN', 'en-US']) {
        await page.setViewportSize({width, height:900});
        await page.goto(origin + '/?state=populated&lang=' + lang + '#/automation');
        for (const id of ['sources-list', 'rules-list', 'actions-list']) {
            const container = page.locator('#' + id);
            await container.locator('tbody tr').first().waitFor();
            await container.scrollIntoViewIfNeeded();
            const record = await container.evaluate(el => {
                const table = el.querySelector('table');
                return {id:el.id, innerWidth, innerHeight, scrollY,
                    pageOverflow:document.documentElement.scrollWidth > innerWidth,
                    tableDisplay:getComputedStyle(table).display,
                    containerOverflow:getComputedStyle(el).overflowX,
                    clientWidth:el.clientWidth, scrollWidth:el.scrollWidth,
                    rows:table.tBodies[0].rows.length,
                    handlers:[...table.querySelectorAll('button')].map(b => b.getAttribute('onclick')),
                    columns:[...table.tHead.rows[0].cells].map((th,i) => ({
                        headerX:th.getBoundingClientRect().x,
                        bodyX:table.tBodies[0].rows[0].cells[i].getBoundingClientRect().x
                    }))};
            });
            const prefix = 'output/macos27-20260928/after/automation-table-' + id + '-' + lang + '-' + width;
            if ([390,1440].includes(width)) await page.screenshot({path:prefix + '-left.png'});
            // Scroll the existing owner, not the table; no API write is invoked.
            await container.evaluate(el => el.scrollLeft = el.scrollWidth);
            const lastButton = container.locator('tbody tr').first().locator('button').last();
            await lastButton.focus();
            record.actionReachable = await lastButton.evaluate(b => {
                const r=b.getBoundingClientRect(), owner=b.closest('.card-content').getBoundingClientRect();
                return r.left >= owner.left && r.right <= owner.right + 1 && r.top >= 0 && r.bottom <= innerHeight;
            });
            record.scrollRight = await container.evaluate(el => el.scrollLeft);
            if (width===390) await page.screenshot({path:prefix + '-right.png'});
            if (record.pageOverflow || record.tableDisplay !== 'table' || !record.actionReachable || record.columns.some(c => Math.abs(c.headerX-c.bodyX)>1)) {
                throw new Error('Table layout failed: ' + JSON.stringify(record));
            }
            results.push({lang,...record});
        }
    }
    if (errors.length) throw new Error(JSON.stringify(errors));
    return {results,errors,scope:'Local fixture table geometry and keyboard reachability; screenshots require separate visual review'};
}
