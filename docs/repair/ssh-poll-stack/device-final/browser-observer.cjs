const {chromium}=require('/Users/massif/TianshanOS/tests/prompts/node_modules/playwright');
const fs=require('fs');
(async()=>{
 const out='/Users/massif/TianshanOS/output/playwright/pr42-final',origin='http://10.10.99.97';
 const browser=await chromium.launch({channel:'chrome',headless:false});
 const context=await browser.newContext({serviceWorkers:'block',viewport:{width:1280,height:900}});
 const requests=[],errors=[],frames=[];
 await context.route('**/*',r=>new URL(r.request().url()).origin===origin?r.continue():r.abort());
 const page=await context.newPage();
 page.on('request',r=>requests.push({url:r.url(),origin:new URL(r.url()).origin}));
 page.on('pageerror',e=>errors.push(e.message));
 page.on('websocket',ws=>{ws.on('framereceived',f=>{try{const m=JSON.parse(f.payload);if(!['ssh_output','output'].includes(m.type))frames.push({direction:'received',...m});}catch{}});ws.on('framesent',f=>{try{const m=JSON.parse(f.payload);delete m.password;if(m.type==='terminal_input'&&m.data?.includes('--password'))m.data='[SSH connection command redacted]';frames.push({direction:'sent',...m});}catch{}});});
 await page.goto(origin+'/#/terminal');
 console.log('BROWSER_READY');
 setInterval(async()=>{try{const state=await page.evaluate(()=>({url:location.href,login:!document.getElementById('login-modal')?.classList.contains('hidden'),Terminal:typeof Terminal,FitAddon:typeof FitAddon,terminal:typeof webTerminal!=='undefined'&&!!webTerminal?.terminal,connected:typeof webTerminal!=='undefined'&&webTerminal?.connected,ssh:typeof webTerminal!=='undefined'&&webTerminal?.sshMode,text:document.querySelector('#terminal-container')?.innerText}));fs.writeFileSync(out+'/live.json',JSON.stringify({state,requests,errors,frames},null,2));}catch{}},1000);
})();
