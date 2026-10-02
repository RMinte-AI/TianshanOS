async (page) => {
    const origin=page.url().split('/').slice(0,3).join('/');
    const phase=origin.endsWith('18783')?'after':'baseline';
    const results=[];
    for(const width of [1440,390,320]) for(const lang of ['zh-CN','en-US']) {
        await page.setViewportSize({width,height:width===1440?1000:844});
        await page.goto(origin+'/?state=populated&lang='+lang+'#/terminal');await page.reload();
        await page.locator('.xterm-helper-textarea').waitFor();await page.waitForTimeout(400);
        await page.locator('.xterm-helper-textarea').focus();await page.keyboard.type('echo fixture-only');await page.keyboard.press('Enter');await page.waitForTimeout(150);
        const state=await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,
            terminal:typeof webTerminal==='undefined'?null:{connected:webTerminal.connected,input:webTerminal.inputBuffer,theme:webTerminal.terminal.options.theme,sent:webTerminal.ws.sent,
            lines:Array.from({length:webTerminal.terminal.buffer.active.length},(_,i)=>webTerminal.terminal.buffer.active.getLine(i).translateToString()).join('\n')}}));
        results.push({name:'input-output',width,lang,...state});
        await page.screenshot({path:'output/macos27-20260928/'+phase+'/terminal-input-'+lang+'-'+width+'.png'});
        await page.locator('[onclick*="window.showTerminalLogsModal"]').click();await page.waitForTimeout(300);
        results.push({name:'logs',width,lang,...await page.evaluate(()=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,fields:[...document.querySelectorAll('#terminal-logs-modal input,#terminal-logs-modal select')].map(e=>({id:e.id,type:e.type,value:e.value}))}))});
        await page.screenshot({path:'output/macos27-20260928/'+phase+'/terminal-logs-'+lang+'-'+width+'.png'});
        await page.locator('[onclick="closeTerminalLogsModal()"]').first().click();
    }
    return results;
}
