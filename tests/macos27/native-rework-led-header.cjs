const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const version=process.argv[2]||'v13',ports={v12:18803,v13:18805};assert(ports[version]);
const origin='http://127.0.0.1:'+ports[version],web='output/macos27-20260929-native-rework/build-'+version+'/web_optimized';
const out='output/macos27-20260929-native-rework/led-header-'+version;assert(!fs.existsSync(out));fs.mkdirSync(out);
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const result={scope:'Synthetic LED header layout in static/off/rainbow/long-effect states. No LED writes or control changes.',before:identity(),rows:[],errors:[],writes:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{for(const p of ['css/style.css','js/app.js']){const r=await fetch(origin+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
for(const language of ['en-US','zh-CN'])for(const width of [320,390,768,1024,1440])for(const state of ['static','off','rainbow','long']){
 const page=await browser.newPage({viewport:{width,height:700}});page.on('pageerror',e=>result.errors.push(String(e)));
 await page.route('**/api/**',async route=>{const request=route.request(),url=new URL(request.url());assert.equal(url.origin,origin);
  if(url.pathname==='/api/v1/led/list'){assert.equal(request.method(),'GET');const response=await route.fetch(),body=await response.json();body.data.devices.forEach(d=>{d.current.on=state!=='off';d.current.animation=state==='rainbow'?'rainbow':state==='long'?'fixture-long-animation-name-without-breaks-'.repeat(3):'';});return route.fulfill({response,json:body});}
  if(request.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(url.pathname)){result.writes.push({url:request.url(),method:request.method()});return route.abort();}return route.continue();
 });
 try{await page.goto(origin+'/?state=populated&lang='+language+'#/');await page.locator('.led-device-card').first().waitFor();await page.evaluate(()=>document.fonts.ready);
 const cards=page.locator('.led-device-card');assert.equal(await cards.count(),3);
 for(let i=0;i<3;i++){
  const card=cards.nth(i),header=card.locator('.led-card-header');await header.scrollIntoViewIfNeeded();
  const layout=await header.evaluate(n=>{const bounds=n.getBoundingClientRect(),info=n.querySelector('.led-device-info').getBoundingClientRect(),status=n.querySelector('.led-device-status').getBoundingClientRect(),stop=n.querySelector('.led-stop-btn').getBoundingClientRect(),desc=n.querySelector('.led-device-desc'),d=desc.getBoundingClientRect();return{device:n.parentElement.dataset.device,header:bounds.toJSON(),info:info.toJSON(),status:status.toJSON(),stop:stop.toJSON(),description:desc.textContent,descriptionLines:d.height/parseFloat(getComputedStyle(desc).lineHeight),infoOwnRow:info.bottom<=Math.min(status.top,stop.top),contained:[info,status,stop].every(b=>b.left>=bounds.left&&b.right<=bounds.right),children:[...n.children].map(c=>c.className),stopEvent:n.querySelector('.led-stop-btn').getAttribute('onclick')};});
  const screenshot=out+'/'+language+'-'+width+'-'+state+'-'+layout.device+'.png';await header.screenshot({path:screenshot,animations:'disabled'});result.rows.push({language,width,state,...layout,screenshot});save();
 }
 assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth),width);
 }finally{await page.close();}
}
result.after=identity();assert.deepEqual(result.before,result.after);assert.equal(result.rows.length,120);assert.equal(result.errors.length,0);assert.equal(result.writes.length,0);result.completed=true;save();assert(result.rows.every(r=>r.infoOwnRow&&r.contained),'Device information is compressed beside status/actions or exceeds its header');
}catch(e){result.errors.push(String(e));throw e;}finally{save();await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
