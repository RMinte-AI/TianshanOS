// Compare rendered control tracks on immutable local builds; never write device state.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const candidate=process.argv[2]||'build-v7';assert(['build-v7','build-v8'].includes(candidate));
const out='output/macos27-20260929-native-rework/slider-track-fix/'+(candidate==='build-v7'?'rendered':'rendered-v8');
assert(!fs.existsSync(out));fs.mkdirSync(out);
const result={scope:'First color-correction track, six isolated media/filter modes, two widths; no OS preferences or save actions',rows:[],errors:[],completed:false};
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
(async()=>{const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
for(const [build,port]of [['build-v6',18796],[candidate,candidate==='build-v7'?18798:18799]]){
const root='output/macos27-20260929-native-rework/'+build+'/web_optimized';
const identity=Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,hash(root+'/'+p)]));
for(const p of ['css/style.css','js/app.js']){const r=await fetch('http://127.0.0.1:'+port+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(root+'/'+p)));}
for(const mode of ['normal','contrast','transparency','motion','forced','no-filter'])for(const width of [390,1440]){
const context=await browser.newContext({viewport:{width,height:600},deviceScaleFactor:1}),page=await context.newPage();page.on('pageerror',e=>result.errors.push(String(e)));
try{
const feature={contrast:['prefers-contrast','more'],transparency:['prefers-reduced-transparency','reduce'],motion:['prefers-reduced-motion','reduce'],forced:['forced-colors','active']}[mode];
if(feature){const cdp=await context.newCDPSession(page);await cdp.send('Emulation.setEmulatedMedia',{features:[{name:feature[0],value:feature[1]}]});}
if(mode==='no-filter')await page.route('**/*',async r=>{if(!['document','stylesheet'].includes(r.request().resourceType()))return r.continue();const response=await r.fetch();await r.fulfill({response,body:(await response.text()).replaceAll('@supports ((backdrop-filter: blur(1px)) or (-webkit-backdrop-filter: blur(1px)))','@supports (codex-unsupported-property: 1)')});});
await page.goto('http://127.0.0.1:'+port+'/?state=populated&lang=en-US#/');
await page.locator('[onclick="openLedModal(\'matrix\', \'colorcorrection\')"]').click();await page.waitForFunction(()=>ccInitialConfig!==null);await page.evaluate(()=>document.fonts.ready);
const range=page.locator('#led-modal .cc-section input[type="range"]').first();await range.scrollIntoViewIfNeeded();
const state=await range.evaluate(e=>{const b=e.getBoundingClientRect(),s=getComputedStyle(e);return {rect:b.toJSON(),background:s.backgroundColor,value:e.value,min:e.min,max:e.max,disabled:e.disabled,innerWidth,innerHeight};});
assert(!state.disabled);assert(state.rect.x>=0&&state.rect.right<=width&&state.rect.y>=0&&state.rect.bottom<=600);
const screenshot=out+'/'+build+'-'+mode+'-'+width+'.png';await page.screenshot({path:screenshot,animations:'disabled'});
result.rows.push({build,mode,width,identity,screenshot,...state});
}finally{await context.close();}
}
assert.deepEqual(identity,Object.fromEntries(Object.keys(identity).map(p=>[p,hash(root+'/'+p)])));
}
assert.equal(result.errors.length,0);assert.equal(result.rows.length,24);result.completed=true;
}catch(e){result.errors.push(String(e));throw e;}finally{fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
