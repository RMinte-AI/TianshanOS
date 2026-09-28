const {test}=require('node:test');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const root=path.resolve(process.env.PROJECT_WEB_ROOT||'components/ts_webui/web');
for(const language of ['en-US','zh-CN'])test(`${language}: cold offline real xterm and terminal lifecycle`,async()=>{
 const browser=await chromium.launch({channel:'chrome',headless:true});
 const context=await browser.newContext({serviceWorkers:'block',viewport:{width:1280,height:900}});
 const requests=[],errors=[];const origin='http://fixture.local';
 try{
  await context.route('**/*',async route=>{const u=new URL(route.request().url());
   if(u.origin!==origin){await route.abort();return;}
   if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{}}});return;}
   const file=path.join(root,u.pathname==='/'?'index.html':u.pathname);
   try{await route.fulfill({body:fs.readFileSync(file),contentType:({'.js':'application/javascript','.css':'text/css','.html':'text/html','.woff2':'font/woff2'})[path.extname(file)]||'application/octet-stream'});}catch{await route.abort();}
  });
  await context.addInitScript(language=>{
   localStorage.setItem('ts_language',language);window.sent=[];
   window.WebSocket=class{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;setTimeout(()=>{this.readyState=1;this.onopen?.();},0);}send(s){sent.push(JSON.parse(s));}close(){this.readyState=3;}};
  },language);
  const page=await context.newPage();page.on('request',r=>requests.push({url:r.url(),origin:new URL(r.url()).origin}));page.on('pageerror',e=>errors.push(e.message));
  await page.goto(origin);await page.waitForFunction(()=>i18n.isReady());
  assert.equal(await page.evaluate(()=>typeof Terminal),'undefined');
  await page.evaluate(()=>{closeLoginModal();return loadTerminalPage();});
  await page.waitForFunction(()=>webTerminal?.ws?.readyState===1);
  assert(await page.evaluate(()=>typeof Terminal==='function'&&typeof FitAddon.FitAddon==='function'&&webTerminal.terminal instanceof Terminal&&!!webTerminal.fitAddon));
  assert.equal(await page.locator('.xterm').count(),1);
  assert(!(await page.locator('#terminal-container').innerText()).includes('could not be loaded'));
  const frame=m=>page.evaluate(m=>webTerminal.ws.onmessage({data:JSON.stringify(m)}),m);
  await frame({type:'connected'});
  await page.locator('.xterm-helper-textarea').focus();await page.keyboard.type('help');await page.keyboard.press('Enter');
  assert(await page.evaluate(()=>sent.some(m=>m.type==='terminal_input'&&m.data==='help')));
  await page.evaluate(()=>webTerminal.startSshShell({host:'192.0.2.1',user:'fixture',password:'fake'}));
  await frame({type:'ssh_status',status:'connected'});
  await page.evaluate(()=>{window.writes=[];const t=webTerminal.terminal;for(const name of ['write','writeln']){const original=t[name].bind(t);t[name]=(...a)=>{writes.push(a[0]);return original(...a);};}});
  await frame({type:'power_event',event:'low_voltage',voltage:10.5});
  assert(await page.evaluate(()=>webTerminal.sshMode&&writes.length>0&&!writes.join('').includes('tianshan>')));
  await page.locator('.xterm-helper-textarea').focus();await page.keyboard.type('z');
  assert(await page.evaluate(()=>sent.at(-1).type==='ssh_input'&&sent.at(-1).data==='z'));
  await page.setViewportSize({width:1100,height:800});await page.evaluate(()=>webTerminal.fit());
  assert(await page.evaluate(()=>webTerminal.terminal.cols>0));
  await page.keyboard.press('Control+c');assert(await page.evaluate(()=>sent.at(-1).type==='ssh_input'&&sent.at(-1).data==='\x03'));
  await page.keyboard.press('Control+Backslash');assert(await page.evaluate(()=>sent.at(-1).type==='ssh_disconnect'&&webTerminal.sshMode));
  await frame({type:'ssh_status',status:'closed'});assert(await page.evaluate(()=>!webTerminal.sshMode&&writes.join('').includes('tianshan>')));
  await page.evaluate(()=>{window.oldTerminal=webTerminal;destroyWebTerminal();});
  assert(await page.evaluate(()=>oldTerminal.destroyed&&oldTerminal.terminal===null));
  await page.evaluate(()=>loadTerminalPage());await page.waitForFunction(()=>webTerminal?.terminal&&webTerminal.ws?.readyState===1);
  assert.equal(requests.filter(r=>r.url.includes('/vendor/xterm/')).length,3);
  assert(requests.every(r=>r.origin===origin));assert.deepEqual(errors,[]);
  const out=process.env.OFFLINE_RESULTS||'docs/repair/xterm-local';fs.mkdirSync(out,{recursive:true});
  fs.writeFileSync(path.join(out,`requests-${language}.json`),JSON.stringify({requests,errors,realXterm:true,externalRequests:0},null,2));
 }finally{await context.close();await browser.close();}
});
