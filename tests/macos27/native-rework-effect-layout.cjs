// Local fixture only: inspect the actual animation parameter branch for every LED device.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const version=process.argv[2]||'v11',ports={v11:18802,v12:18803};assert(ports[version]);
const origin='http://127.0.0.1:'+ports[version],web='output/macos27-20260929-native-rework/build-'+version+'/web_optimized';
const suffix=process.argv[3]||'';assert(/^[a-z0-9-]*$/.test(suffix));
const out='output/macos27-20260929-native-rework/slider-track-fix/effects-'+version+(suffix?'-'+suffix:'');assert(!fs.existsSync(out));fs.mkdirSync(out);
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const result={scope:'Synthetic active rainbow animation on board/touch/matrix. Bilingual, three widths, 600px height. Layout and close only; no controls changed or device writes.',before:identity(),rows:[],errors:[],writes:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{for(const p of ['css/style.css','js/app.js']){const r=await fetch(origin+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
for(const language of ['en-US','zh-CN'])for(const width of [320,390,1440]){
 const page=await browser.newPage({viewport:{width,height:600}});page.on('pageerror',e=>result.errors.push(String(e)));
 await page.route('**/api/**',async route=>{const request=route.request(),url=new URL(request.url());assert.equal(url.origin,origin);
  if(url.pathname==='/api/v1/led/list') {assert.equal(request.method(),'GET');const response=await route.fetch();const body=await response.json();body.data.devices.forEach(d=>{d.current.animation='rainbow';d.current.speed=50;d.effects=['rainbow'];});return route.fulfill({response,json:body});}
  if(request.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(url.pathname)){result.writes.push({method:request.method(),url:request.url()});return route.abort();}return route.continue();
 });
 try{await page.goto(origin+'/?state=populated&lang='+language+'#/');
 for(const device of ['board','touch','matrix']){
  await page.locator('[onclick="openLedModal(\''+device+'\', \'effect\')"]').click();
  const modal=page.locator('#led-modal'),input=page.locator('#modal-effect-speed-'+device);await input.scrollIntoViewIfNeeded();
  const layout=await input.evaluate(n=>{const row=n.closest('.config-row'),label=row.querySelector('label').getBoundingClientRect(),value=row.querySelector('span').getBoundingClientRect(),range=n.getBoundingClientRect();return {viewport:{width:innerWidth,height:innerHeight},range:range.toJSON(),value:value.toJSON(),label:label.toJSON(),aligned:Math.abs((range.top+range.bottom-value.top-value.bottom)/2)<=1&&Math.abs((range.top+range.bottom-label.top-label.bottom)/2)<=1,overflow:document.documentElement.scrollWidth>innerWidth};});
  const screenshot=out+'/'+language+'-'+width+'-'+device+'.png';await page.screenshot({path:screenshot,animations:'disabled'});result.rows.push({language,width,device,...layout,screenshot});save();
  const actions=modal.locator('.cc-actions');await actions.scrollIntoViewIfNeeded();
  const bottom=await actions.evaluate(n=>[...n.querySelectorAll('button')].map(b=>{const r=b.getBoundingClientRect();return {text:b.textContent.trim(),visible:r.left>=0&&r.right<=innerWidth&&r.top>=0&&r.bottom<=innerHeight};}));assert.equal(bottom.length,2);assert(bottom.every(b=>b.visible));
  const bottomScreenshot=out+'/'+language+'-'+width+'-'+device+'-bottom.png';await page.screenshot({path:bottomScreenshot,animations:'disabled'});Object.assign(result.rows.at(-1),{bottom,bottomScreenshot});save();
  await modal.locator('.modal-close').click();await modal.waitFor({state:'hidden'});
 }}finally{await page.close();}
}
assert.equal(result.rows.length,18);assert.equal(result.errors.length,0);assert.equal(result.writes.length,0);result.after=identity();assert.deepEqual(result.before,result.after);result.completed=true;save();assert(result.rows.every(r=>r.aligned&&!r.overflow),'Animation parameter row is misaligned or overflowing');
}catch(e){result.errors.push(String(e));throw e;}finally{save();await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
