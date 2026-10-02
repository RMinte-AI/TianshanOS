// Compare rendered control tracks on immutable local builds; never write device state.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out='output/macos27-20260929-native-rework/led-surfaces-v14-centered';
assert(!fs.existsSync(out));fs.mkdirSync(out);
const result={scope:'Three LED brightness tracks, six isolated media/filter modes, two widths; card appearance and keyboard focus. No value changes, OS preferences or device writes',rows:[],errors:[],writes:[],completed:false};
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
(async()=>{const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
for(const [build,port]of [['build-v13',18805],['build-v14',18806]]){
const root='output/macos27-20260929-native-rework/'+build+'/web_optimized';
const identity=Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,hash(root+'/'+p)]));
for(const p of ['css/style.css','js/app.js']){const r=await fetch('http://127.0.0.1:'+port+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(root+'/'+p)));}
for(const mode of ['normal','contrast','transparency','motion','forced','no-filter'])for(const width of [320,1440]){
const context=await browser.newContext({viewport:{width,height:600},deviceScaleFactor:1}),page=await context.newPage();page.on('pageerror',e=>result.errors.push(String(e)));
try{
const feature={contrast:['prefers-contrast','more'],transparency:['prefers-reduced-transparency','reduce'],motion:['prefers-reduced-motion','reduce'],forced:['forced-colors','active']}[mode];
if(feature){const cdp=await context.newCDPSession(page);await cdp.send('Emulation.setEmulatedMedia',{features:[{name:feature[0],value:feature[1]}]});}
if(mode==='no-filter')await page.route('**/*',async r=>{if(!['document','stylesheet'].includes(r.request().resourceType()))return r.continue();const response=await r.fetch();await r.fulfill({response,body:(await response.text()).replaceAll('@supports ((backdrop-filter: blur(1px)) or (-webkit-backdrop-filter: blur(1px)))','@supports (codex-unsupported-property: 1)')});});
await page.route('**/api/**',r=>{const q=r.request();if(q.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(new URL(q.url()).pathname)){result.writes.push(q.url());return r.abort();}return r.continue();});
await page.goto('http://127.0.0.1:'+port+'/?state=populated&lang=en-US#/');
await page.locator('.led-brightness-slider').first().waitFor();await page.evaluate(()=>document.fonts.ready);
for(const device of ['board','touch','matrix']){
const card=page.locator('.led-device-card[data-device="'+device+'"]');const range=card.locator('.led-brightness-slider');
// Pixel sampling needs both sides of the track. "If needed" can leave a
// fractional edge at the viewport bottom, so center only the test viewport.
await range.evaluate(n=>n.scrollIntoView({block:'center',inline:'nearest',behavior:'instant'}));
await range.screenshot({animations:'disabled'});
const state=await range.evaluate(e=>{const b=e.getBoundingClientRect(),s=getComputedStyle(e);return {rect:b.toJSON(),background:s.backgroundColor,value:e.value,min:e.min,max:e.max,disabled:e.disabled,innerWidth,innerHeight};});
assert(!state.disabled);
if(!(state.rect.x>=0&&state.rect.right<=width&&state.rect.y>=0&&state.rect.bottom<=600)){
result.failedGeometry={build,mode,width,device,...state};await page.screenshot({path:out+'/failed-geometry.png',animations:'disabled'});
throw new Error('Track outside screenshot viewport: '+JSON.stringify(result.failedGeometry));
}
const screenshot=out+'/'+build+'-'+mode+'-'+width+'-'+device+'.png';await page.screenshot({path:screenshot,animations:'disabled'});
const appearance=await card.evaluate(n=>{const c=getComputedStyle(n),r=getComputedStyle(n.querySelector('.led-brightness-row'));return{border:c.borderColor,shadow:c.boxShadow,rowBackground:r.backgroundColor,rowBorder:r.borderWidth,colors:[...n.querySelectorAll('.modern-color-dot')].map(e=>e.getAttribute('style'))};});
await range.focus();await page.keyboard.press('Tab');await page.keyboard.press('Shift+Tab');const focus=await range.evaluate(n=>({active:document.activeElement===n,visible:n.matches(':focus-visible'),outline:getComputedStyle(n).outlineStyle,width:getComputedStyle(n).outlineWidth}));
const focusScreenshot=out+'/'+build+'-'+mode+'-'+width+'-'+device+'-focus.png';await page.screenshot({path:focusScreenshot,animations:'disabled'});
result.rows.push({build,mode,width,device,identity,screenshot,focusScreenshot,appearance,focus,...state});
if(build==='build-v14')assert(focus.active&&focus.visible&&focus.outline!=='none');
await range.evaluate(n=>n.blur());
}
}finally{await context.close();}
}
assert.deepEqual(identity,Object.fromEntries(Object.keys(identity).map(p=>[p,hash(root+'/'+p)])));
}
assert.equal(result.errors.length,0);assert.equal(result.rows.length,72);assert.equal(result.writes.length,0);
for(const before of result.rows.filter(r=>r.build==='build-v13')){const after=result.rows.find(r=>r.build==='build-v14'&&r.mode===before.mode&&r.width===before.width&&r.device===before.device);assert.deepEqual(after.appearance.colors,before.appearance.colors);assert.equal(after.value,before.value);}result.completed=true;
}catch(e){result.errors.push(String(e));throw e;}finally{fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
