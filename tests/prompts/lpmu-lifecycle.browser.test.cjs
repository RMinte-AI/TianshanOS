const {test, before, after} = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const {root:sourceRoot} = require('./harness.cjs');
const root = process.env.LPMU_WEB_ROOT ? path.resolve(process.env.LPMU_WEB_ROOT) : sourceRoot;

let browser, server, base;
before(async () => {
    server = http.createServer((req, res) => {
        const url = new URL(req.url, 'http://local');
        const file = path.resolve(root, '.' + (url.pathname === '/' ? '/index.html' : url.pathname));
        if (!file.startsWith(root + '/')) { res.writeHead(403).end(); return; }
        fs.readFile(file, (error, data) => {
            if (error) { res.writeHead(404).end(); return; }
            res.setHeader('Content-Type', ({'.js':'application/javascript', '.html':'text/html', '.css':'text/css'})[path.extname(file)] || 'application/octet-stream');
            res.end(data);
        });
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    base = `http://127.0.0.1:${server.address().port}`;
    browser = await chromium.launch({channel:'chrome', headless:true});
});
after(async () => { await browser?.close(); await new Promise(resolve => server?.close(resolve)); });

async function open(language) {
    const context = await browser.newContext({locale:language});
    await context.addInitScript(language => {
        localStorage.setItem('ts_language', language);
        class Socket { static OPEN=1; static CONNECTING=0; constructor(){this.readyState=0;} send(){} close(){this.readyState=3;} }
        window.WebSocket = Socket;
        document.addEventListener('DOMContentLoaded', () => {
            const banner = document.createElement('div');
            banner.textContent = 'LOCAL SIMULATION / 本地模拟测试 — 未连接真实设备';
            banner.style.cssText = 'position:fixed;top:0;left:0;z-index:100000;background:#b00;color:white;padding:4px';
            document.body.appendChild(banner);
        });
    }, language);
    const page = await context.newPage(), errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/*', async route => {
        const url = new URL(route.request().url());
        if (url.origin !== base) { await route.abort(); return; }
        if (url.pathname.startsWith('/api/')) {
            const data = url.pathname.endsWith('/lpmu_access/status') ? {run_id:0, stage:'idle', running:false} : {};
            await route.fulfill({json:{code:0, data}}); return;
        }
        await route.continue();
    });
    await page.goto(base + '/#/network');
    await page.waitForFunction(() => i18n.isReady());
    await page.evaluate(async () => {
        closeLoginModal(); api.token='fixture'; api.username='root'; api.level=2;
        updateAuthUI(); router.appReady=true;
        const navigate=router.navigate.bind(router);
        router.navigate=(...args) => window.navigationWork=navigate(...args);
        await router.navigate();
        window.resultWindows=0;
        const show=showNetworkLpmuAccessResult;
        showNetworkLpmuAccessResult=info => { resultWindows++; show(info); };
    });
    await page.locator('#network-lpmu-access-btn').waitFor();
    return {page, context, errors};
}
async function navigate(page, target) {
    await page.evaluate(target => { window.location.hash='#/'+target; }, target);
    await page.waitForFunction(target => router.currentPage === (target==='network' ? loadNetworkPage : loadFilesPage), target);
    await page.evaluate(() => navigationWork);
    await page.locator(target==='network' ? '#network-lpmu-access-btn' : '#file-list').waitFor();
}

for (const language of ['zh-CN', 'en-US']) {
    test(`${language}: keyboard trap and Escape clear the password without submission`, async () => {
        const {page,context,errors}=await open(language);
        try {
            await page.locator('#network-lpmu-access-btn').click();
            await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(()=>{window.keyboardPassword=document.getElementById('network-lpmu-sudo-password');window.keyboardPosts=0;api.lpmuAccessStart=async()=>{keyboardPosts++;};});
            await page.keyboard.press('Shift+Tab');assert.equal(await page.locator('#network-lpmu-password-modal .primary').evaluate(el=>el===document.activeElement),true);
            await page.keyboard.press('Tab');assert.equal(await page.locator('#network-lpmu-sudo-password').evaluate(el=>el===document.activeElement),true);
            await page.keyboard.press('Escape');assert.equal(await page.locator('#network-lpmu-password-modal').count(),0);
            assert.equal(await page.evaluate(()=>keyboardPassword.value),'');assert.equal(await page.evaluate(()=>keyboardPosts),0);
            assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: Enter submits once and Escape closes the finished result`, async () => {
        const {page,context,errors}=await open(language);
        try {
            await page.locator('#network-lpmu-access-btn').click();await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(()=>{
                window.keyboardPosts=0;
                api.lpmuAccessStart=()=>{keyboardPosts++;return new Promise(resolve=>{window.releaseKeyboardStart=resolve;});};
                api.lpmuAccessStatus=async()=>({code:0,data:{run_id:40,stage:'success',running:false}});
            });
            await page.keyboard.press('Enter');await page.waitForFunction(()=>typeof releaseKeyboardStart==='function');
            await page.keyboard.press('Enter');assert.equal(await page.evaluate(()=>keyboardPosts),1);
            await page.evaluate(()=>releaseKeyboardStart({code:0,data:{run_id:40,stage:'queued',running:true}}));
            await page.locator('#network-lpmu-result-modal').waitFor();await page.keyboard.press('Escape');
            assert.equal(await page.locator('#network-lpmu-result-modal').count(),0);assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: logs are lazy, use byte offsets and retain the complete cached transcript`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                window.logParts=['$ synthetic-command\n=== 原始标题 ===\n'+ '网络记录🙂\n'.repeat(24),'最后一段网络记录\n'];
                window.logOffsets=[];
                api.lpmuAccessLog=async (run,offset) => {
                    logOffsets.push(offset);const part=logParts[logOffsets.length-1];
                    return {code:0,data:{run_id:run,output:part,next_offset:offset+new TextEncoder().encode(part).length,done:logOffsets.length===2}};
                };
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:30});
            });
            assert.equal(await page.evaluate(()=>logOffsets.length),0);
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent===logParts.join(''));
            assert.deepEqual(await page.evaluate(()=>logOffsets),[0,await page.evaluate(()=>new TextEncoder().encode(logParts[0]).length)]);
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.locator('#network-lpmu-result-modal summary').click();
            assert.equal(await page.evaluate(()=>logOffsets.length),2);assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: logs stop after a no-progress page and do not retry on reopen`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                window.logCalls=0;
                api.lpmuAccessLog=async run => {
                    if(++logCalls===4)throw new Error('bounded test stop');
                    return {code:0,data:{run_id:run,output:'',next_offset:0,done:false}};
                };
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:31});
            });
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent.includes(t('networkPage.lpmuAccessLogFailed')));
            assert.equal(await page.evaluate(()=>logCalls),1);
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.locator('#network-lpmu-result-modal summary').click();
            assert.equal(await page.evaluate(()=>logCalls),1);assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: logs pause when folded and resume from the next byte offset`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                window.logOffsets=[];
                api.lpmuAccessLog=(run,offset) => {
                    logOffsets.push(offset);
                    if(logOffsets.length===1)return new Promise(resolve=>window.releaseFirstPage=()=>resolve({code:0,data:{run_id:run,output:'第一页🙂\n',next_offset:14,done:false}}));
                    return Promise.resolve({code:0,data:{run_id:run,output:'第二页\n',next_offset:24,done:true}});
                };
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:32});
            });
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>typeof releaseFirstPage==='function');
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.evaluate(()=>releaseFirstPage());
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent==='第一页🙂\n');
            assert.deepEqual(await page.evaluate(()=>logOffsets),[0]);
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent==='第一页🙂\n第二页\n');
            assert.deepEqual(await page.evaluate(()=>logOffsets),[0,14]);assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    for (const failure of ['stale','sd','wrongRun']) test(`${language}: logs ${failure} cannot replace the configuration result`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(failure => {
                window.logCalls=0;
                api.lpmuAccessLog=async run => {
                    logCalls++;
                    if(failure==='wrongRun')return {code:0,data:{run_id:run+1,output:'another run',next_offset:11,done:true}};
                    return {code:failure==='sd'?7:2,error:'synthetic log unavailable'};
                };
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:33});
            },failure);
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent.includes(t('networkPage.lpmuAccessLogFailed')));
            assert.equal(await page.evaluate(()=>logCalls),1);
            assert.equal(await page.locator('#network-lpmu-result-modal .state.ok').count(),2);
            assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: logs allow an empty terminal page`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                api.lpmuAccessLog=async run=>({code:0,data:{run_id:run,output:'',next_offset:0,done:true}});
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:34});
            });
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent===t('networkPage.lpmuAccessLogEmpty'));
            assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: logs failure summary shows the last diagnosis and keeps technical status separate`, async () => {
        const {page, context, errors} = await open(language);
        try {
            const result=await page.evaluate(() => {
                const error='setup-smart-route failed (exit=1): remote script failed';
                const diagnosis='RTNETLINK answers: Network is unreachable';
                const info=normalizeNetworkLpmuAccessStatus({run_id:35,stage:'failed',running:false,exit_code:1,last_error:error,
                    output_tail:'route diagnostic line '.repeat(80)+'\n\u001b[31m'+diagnosis+'\u001b[0m\n=== 调试结束 ===\n'});
                const unconfirmed=normalizeNetworkLpmuAccessStatus({stage:'failed',running:false,exit_code:0,internet_confirmed:false,configuration_complete:true,last_error:'confirmation missing'});
                showNetworkLpmuAccessResult(info);
                return {summary:info.summary,technicalError:info.technicalError,error,diagnosis,unconfirmed,expected:t('networkPage.lpmuAccessUnconfirmed')};
            });
            assert(!result.summary.includes(result.error));assert(result.summary.includes(result.diagnosis));
            assert.equal(result.technicalError,result.error);
            assert(!result.summary.includes('==='));assert(!result.summary.includes('\u001b'));
            assert.equal(result.unconfirmed.success,false);assert.equal(result.unconfirmed.summary,result.expected);assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: explicit no-internet error survives a following route table and raw logs remain available`, async () => {
        const {page, context, errors} = await open(language);
        try {
            const result=await page.evaluate(() => {
                const error='setup-smart-route failed (exit=1): remote script failed';
                const transcript='=== LPMU 智能路由配置 ===\n2. 测试主接口 enp2s0...\n  ✗ enp2s0 无法访问互联网\n3. 主接口不可用，测试备用接口...\n\u001b[31m✗ 错误：没有找到可用的互联网连接\u001b[0m\n\n当前路由表：\ndefault via 10.10.99.100 dev enp2s0 proto static metric 1000\n172.19.0.0/16 dev br-a6e7a7c05f06 proto kernel scope link src 172.19.0.1\n192.168.122.0/24 dev virbr0 proto kernel scope link src 192.168.122.1 linkdown\n';
                api.lpmuAccessLog=async run=>({code:0,data:{run_id:run,output:transcript,next_offset:new TextEncoder().encode(transcript).length,done:true}});
                const info=normalizeNetworkLpmuAccessStatus({run_id:37,stage:'failed',running:false,exit_code:1,last_error:error,output_tail:transcript});
                const password=normalizeNetworkLpmuAccessStatus({stage:'failed',running:false,exit_code:1,last_error:error,output_tail:'Sorry, try again.\n\nsudo: no password was provided\nsudo: 1 incorrect password attempt\n'});
                const transport=normalizeNetworkLpmuAccessStatus({stage:'failed',running:false,last_error:'SSH connect failed: connection timeout',output_tail:transcript});
                showNetworkLpmuAccessResult(info);
                return {info,error,transcript,password,transport,expected:t('networkPage.lpmuAccessNoInternet'),passwordExpected:t('networkPage.lpmuAccessPasswordIncorrect')};
            });
            assert.equal(result.info.summary,result.expected);assert.equal(result.info.success,false);
            assert.equal(result.password.summary,result.passwordExpected);
            assert(result.transport.summary.startsWith('SSH connect failed: connection timeout'));
            assert(!await page.locator('#network-lpmu-result-modal .t-body').textContent().then(s=>s.includes(result.error)));
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent.includes('linkdown'));
            const log=await page.locator('#network-lpmu-result-modal pre').textContent();
            assert(log.includes(result.error));assert(log.endsWith(result.transcript));assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: log group follows dialog typography and fits desktop and narrow viewports`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                api.lpmuAccessLog=async run=>({code:0,data:{run_id:run,output:'网络诊断\n'.repeat(40),next_offset:520,done:true}});
                showNetworkLpmuAccessResult(normalizeNetworkLpmuAccessStatus({run_id:36,stage:'failed',running:false,last_error:'setup-smart-route failed (exit=1): remote script failed',output_tail:'RTNETLINK answers: Network is unreachable'}));
            });
            for (const width of [1280,390]) {
                await page.setViewportSize({width,height:844});
                for (const expanded of [false,true,false]) {
                    const toggle=page.locator('#network-lpmu-result-modal summary');
                    if(await page.locator('#network-lpmu-result-modal details').evaluate(el=>el.open)!==expanded) await toggle.click();
                    if(expanded) await page.waitForFunction(()=>document.querySelector('#network-lpmu-result-modal pre').textContent.endsWith('网络诊断\n'.repeat(40)));
                    const style=await page.locator('#network-lpmu-result-modal').evaluate(el=>{
                        const css=selector=>getComputedStyle(el.querySelector(selector));
                        const group=css('details'),label=css('summary .rl'),reason=css('.sb>.t-body'),log=css('.logv');
                        const sheet=el.querySelector('.sheet');
                        return {left:sheet.getBoundingClientRect().left,right:sheet.getBoundingClientRect().right,scroll:sheet.scrollWidth,width:sheet.clientWidth,
                            groupBackground:group.backgroundColor,groupRadius:group.borderRadius,rowHeight:el.querySelector('summary').getBoundingClientRect().height,
                            labelColor:label.color,labelFont:label.fontSize,labelLine:label.lineHeight,
                            reasonFont:reason.fontSize,reasonLine:reason.lineHeight,logFont:log.fontSize,logLine:log.lineHeight};
                    });
                    assert(style.left>=0 && style.right<=width);assert(style.scroll<=style.width+1);
                    assert.equal(style.groupBackground,'rgba(255, 255, 255, 0.62)');assert.equal(style.groupRadius,'12px');assert.equal(style.rowHeight,46);
                    assert.equal(style.labelColor,'rgb(29, 29, 31)');assert.equal(style.labelFont,'14px');assert.equal(style.labelLine,'20px');
                    assert.equal(style.reasonFont,'14px');assert.equal(style.reasonLine,'20px');assert.equal(style.logFont,'12px');assert.equal(style.logLine,'18px');
                    if(expanded) {
                        const dir=path.resolve('output/lpmu-log-style-20261009',process.env.LPMU_WEB_ROOT?'built-visuals':'source-visuals');
                        fs.mkdirSync(dir,{recursive:true});
                        await page.screenshot({path:path.join(dir,`${language}-${width}.png`)});
                    }
                }
            }
            assert.deepEqual(errors,[]);
        } finally {await context.close();}
    });

    test(`${language}: running remains authoritative until cleanup completes; result appears once`, async () => {
        const {page, context, errors} = await open(language);
        try {
            const result = await page.evaluate(async () => {
                const pendingSuccess=normalizeNetworkLpmuAccessStatus({stage:'success', running:true});
                const pendingFailure=normalizeNetworkLpmuAccessStatus({stage:'failed', running:true, last_error:'script failed'});
                renderNetworkLpmuAccessStatus({run_id:11, stage:'running_script', running:true});
                api.lpmuAccessStatus=async () => ({code:0, data:{run_id:11, stage:'success', running:true}});
                await refreshNetworkLpmuAccessStatus();
                const pending={windows:resultWindows, disabled:document.getElementById('network-lpmu-access-btn').disabled};
                api.lpmuAccessStatus=async () => ({code:0, data:{run_id:11, stage:'success', running:false}});
                await refreshNetworkLpmuAccessStatus(); await refreshNetworkLpmuAccessStatus();
                return {pendingSuccess,pendingFailure,pending,windows:resultWindows,disabled:document.getElementById('network-lpmu-access-btn').disabled};
            });
            assert.equal(result.pendingSuccess.running,true); assert.equal(result.pendingSuccess.success,false);
            assert.equal(result.pendingFailure.running,true); assert.equal(result.pendingFailure.failed,false);
            assert.deepEqual(result.pending,{windows:0,disabled:true});
            assert.equal(result.windows,1); assert.equal(result.disabled,false); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: navigation clears the password and cannot submit from Files`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.locator('#network-lpmu-access-btn').click();
            await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(() => {
                window.oldPassword=document.getElementById('network-lpmu-sudo-password'); window.startCalls=0;
                api.lpmuAccessStart=async () => { startCalls++; return {code:0,data:{run_id:12,stage:'queued',running:true}}; };
            });
            await navigate(page,'files');
            assert.equal(await page.locator('#network-lpmu-password-modal').count(),0);
            assert.equal(await page.evaluate(() => oldPassword.value),'');
            await page.evaluate(() => submitNetworkLpmuAccess());
            assert.equal(await page.evaluate(() => startCalls),0);
            await navigate(page,'network');
            assert.equal(await page.locator('#network-lpmu-password-modal').count(),0); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    for (const failed of [false,true]) test(`${language}: late start ${failed?'rejection':'response'} cannot update a new navigation`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.locator('#network-lpmu-access-btn').click();
            await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(() => {
                window.startCalls=0;
                api.lpmuAccessStart=() => { startCalls++; return new Promise((resolve,reject) => { window.releaseStart=resolve; window.rejectStart=reject; }); };
                window.startWork=submitNetworkLpmuAccess();
            });
            await navigate(page,'files'); await navigate(page,'network');
            const before=await page.locator('#network-lpmu-access-current').textContent();
            const timer=await page.evaluate(() => networkLpmuAccessPollTimer);
            assert.equal(await page.locator('#network-lpmu-access-btn').isDisabled(),true);
            await page.evaluate(() => startNetworkLpmuAccess());
            await page.evaluate(async failed => {
                if(failed) rejectStart(new Error('synthetic late failure'));
                else releaseStart({code:0,data:{run_id:13,stage:'queued',running:true}});
                await startWork;
            },failed);
            assert.equal(await page.locator('#network-lpmu-access-current').textContent(),before);
            assert.equal(await page.evaluate(() => networkLpmuAccessPollTimer),timer);
            assert.equal(await page.evaluate(() => startCalls),1); assert.equal(await page.evaluate(() => resultWindows),0);
            assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: status failure preserves remote outcome and never starts another command`, async () => {
        const {page, context, errors} = await open(language);
        try {
            const result=await page.evaluate(async () => {
                renderNetworkLpmuAccessStatus({run_id:14,stage:'running_script',running:true});
                window.startCalls=0; api.lpmuAccessStart=async () => { startCalls++; };
                api.lpmuAccessStatus=async () => { throw new Error('synthetic offline'); };
                await refreshNetworkLpmuAccessStatus();
                const pending={...networkLpmuAccessLastInfo};
                const unknown=document.getElementById('network-lpmu-access-current').textContent;
                await startNetworkLpmuAccess();
                const blocked=!document.getElementById('network-lpmu-password-modal');
                api.lpmuAccessStatus=async () => ({code:0,data:{run_id:14,stage:'failed',running:false,last_error:'sudo rejected'}});
                await refreshNetworkLpmuAccessStatus();
                return {pending,unknown,expected:t('networkPage.lpmuAccessStatusUnknown'),blocked,startCalls,windows:resultWindows,final:{...networkLpmuAccessLastInfo}};
            });
            assert.equal(result.pending.running,true); assert.equal(result.pending.failed,false);
            assert.equal(result.unknown,result.expected); assert.equal(result.blocked,true);
            assert.equal(result.startCalls,0); assert.equal(result.windows,1); assert.equal(result.final.failed,true); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: late manual status refresh cannot restart polling in a new navigation`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                renderNetworkLpmuAccessStatus({run_id:19,stage:'running_script',running:true});
                showNetworkLpmuAccessStatusUnknown('synthetic offline');
                api.lpmuAccessStatus=()=>new Promise(resolve => {window.releaseRefresh=resolve;});
                window.refreshWork=startNetworkLpmuAccess();
            });
            await navigate(page,'files');
            await page.evaluate(() => {api.lpmuAccessStatus=async()=>({code:0,data:{run_id:20,stage:'running_script',running:true}});});
            await navigate(page,'network');
            const timer=await page.evaluate(() => networkLpmuAccessPollTimer);
            assert.notEqual(timer,null);
            await page.evaluate(async () => {
                releaseRefresh({code:0,data:{run_id:19,stage:'success',running:false}});await refreshWork;
            });
            assert.equal(await page.evaluate(() => networkLpmuAccessPollTimer),timer);
            assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.runId),20);
            assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    for (const outcome of ['success','rejected','lostResponse']) test(`${language}: native API ${outcome} has one POST and accurate outcome`, async () => {
        const {page, context, errors} = await open(language);
        const posts=[];
        let status=outcome==='rejected' ? {run_id:21,stage:'failed',running:false,last_error:'failed to create LPMU access task'} : {run_id:21,stage:'running_script',running:true};
        try {
            await page.route('**/api/v1/network/lpmu_access/start', async route => {
                posts.push(JSON.parse(route.request().postData()));
                if(outcome==='lostResponse') {await route.abort('failed');return;}
                await route.fulfill({json:outcome==='rejected' ? {code:6,error:'Failed to create LPMU access task'} : {code:0,data:{run_id:21,stage:'queued',running:true}}});
            });
            await page.route('**/api/v1/network/lpmu_access/status',route=>route.fulfill({json:{code:0,data:status}}));
            await page.locator('#network-lpmu-access-btn').click();
            await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(() => {window.startWork=submitNetworkLpmuAccess();});
            await page.evaluate(() => startWork);
            assert.deepEqual(posts,[{sudo_password:'SYNTHETIC_PASSWORD'}]);
            assert.equal(await page.locator('#network-lpmu-password-modal').count(),0);
            if(outcome==='rejected') {
                assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.failed),true);
                assert.equal(await page.locator('#network-lpmu-access-btn').isDisabled(),false);
            } else {
                if(outcome==='lostResponse') {
                    assert.equal(await page.evaluate(() => networkLpmuAccessUncertain),true);
                    assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.failed),false);
                    assert.equal(await page.locator('#network-lpmu-access-btn').textContent(),await page.evaluate(()=>t('networkPage.lpmuAccessRefreshStatus')));
                    await page.evaluate(() => startNetworkLpmuAccess());
                }
                assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.running),true);
                status={run_id:21,stage:'success',running:false,internet_confirmed:true,configuration_complete:true};
                await page.evaluate(() => refreshNetworkLpmuAccessStatus());
                assert.equal(await page.locator('#network-lpmu-result-modal').count(),1);
                assert.equal(await page.evaluate(() => networkLpmuAccessPollTimer),null);
                assert.equal(await page.locator('#network-lpmu-access-btn').isDisabled(),false);
            }
            assert.equal(posts.length,1);assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: rejected duplicate start adopts the existing task instead of reporting script failure`, async () => {
        const {page, context, errors} = await open(language);
        const posts=[];
        try {
            await page.route('**/api/v1/network/lpmu_access/start', async route => {
                posts.push(JSON.parse(route.request().postData()));
                await route.fulfill({json:{code:4,error:'LPMU access task is already running'}});
            });
            await page.route('**/api/v1/network/lpmu_access/status',route=>route.fulfill({json:{code:0,data:{run_id:22,stage:'running_script',running:true}}}));
            await page.locator('#network-lpmu-access-btn').click();
            await page.locator('#network-lpmu-sudo-password').fill('SYNTHETIC_PASSWORD');
            await page.evaluate(async () => {await submitNetworkLpmuAccess();});
            assert.equal(posts.length,1);
            assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.failed),false);
            assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.runId),22);
            assert.equal(await page.locator('#network-lpmu-access-btn').isDisabled(),true);
            assert.notEqual(await page.evaluate(() => networkLpmuAccessPollTimer),null);
            assert.equal(await page.evaluate(() => resultWindows),0);assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: old status cannot overwrite a newer execution or release its in-flight guard`, async () => {
        const {page, context, errors} = await open(language);
        try {
            const result=await page.evaluate(async () => {
                renderNetworkLpmuAccessStatus({run_id:15,stage:'running_script',running:true});
                api.lpmuAccessStatus=() => new Promise(resolve => {window.releaseOldStatus=resolve;});
                const old=refreshNetworkLpmuAccessStatus();
                stopNetworkLpmuAccessPolling();
                renderNetworkLpmuAccessStatus({run_id:16,stage:'queued',running:true});
                api.lpmuAccessStatus=() => new Promise(resolve => {window.releaseNewStatus=resolve;});
                const current=refreshNetworkLpmuAccessStatus();
                releaseOldStatus({code:0,data:{run_id:15,stage:'success',running:false}}); await old;
                const afterOld={...networkLpmuAccessLastInfo,requesting:!!networkLpmuAccessRequesting,windows:resultWindows};
                releaseNewStatus({code:0,data:{run_id:16,stage:'running_script',running:true}}); await current;
                return {afterOld,afterNew:{...networkLpmuAccessLastInfo},requesting:!!networkLpmuAccessRequesting};
            });
            assert.equal(result.afterOld.runId,16); assert.equal(result.afterOld.running,true);
            assert.equal(result.afterOld.requesting,true); assert.equal(result.afterOld.windows,0);
            assert.equal(result.afterNew.runId,16); assert.equal(result.requesting,false); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: leaving closes a result dialog and ignores its late log response`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                api.lpmuAccessLog=() => new Promise(resolve => {window.releaseLog=resolve;});
                showNetworkLpmuAccessResult({success:true,summary:'SIMULATED RESULT',runId:17});
                window.oldLog=document.querySelector('#network-lpmu-result-modal pre');
            });
            await page.locator('#network-lpmu-result-modal summary').click();
            await page.waitForFunction(() => typeof releaseLog==='function');
            const oldText=await page.evaluate(() => oldLog.textContent);
            await navigate(page,'files');
            assert.equal(await page.locator('#network-lpmu-result-modal').count(),0);
            await page.evaluate(async () => {releaseLog({code:0,data:{output:'late log',next_offset:8,done:true}});await new Promise(resolve=>setTimeout(resolve,0));});
            assert.equal(await page.evaluate(() => oldLog.textContent),oldText); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });

    test(`${language}: returning to a completed run does not pop the result again`, async () => {
        const {page, context, errors} = await open(language);
        try {
            await page.evaluate(() => {
                renderNetworkLpmuAccessStatus({run_id:18,stage:'running_script',running:true});
                startNetworkLpmuAccessPolling();
            });
            await navigate(page,'files');
            assert.equal(await page.evaluate(() => networkLpmuAccessPollTimer),null);
            await page.evaluate(() => {api.lpmuAccessStatus=async()=>({code:0,data:{run_id:18,stage:'success',running:false}});});
            await navigate(page,'network');
            assert.equal(await page.evaluate(() => resultWindows),0);
            assert.equal(await page.evaluate(() => networkLpmuAccessLastInfo.success),true); assert.deepEqual(errors,[]);
        } finally { await context.close(); }
    });
}
