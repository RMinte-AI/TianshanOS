// Exercise every actual filter selection without applying changes; strict local fixture only.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const candidate=process.argv[2]||'v11';assert(['v8','v9','v10','v11'].includes(candidate));
const out='output/macos27-20260929-native-rework/slider-track-fix/filter-'+candidate+'-columns';assert(!fs.existsSync(out));fs.mkdirSync(out);
const web='output/macos27-20260929-native-rework/build-'+candidate+'/web_optimized',origin='http://127.0.0.1:'+({v8:18799,v9:18800,v10:18801,v11:18802}[candidate]);
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(web+'/'+p)).digest('hex')]));
const result={scope:'Fourteen actual filter buttons, bilingual at 320/390/1440; controls, value updates, local containment and close only. No Apply/Stop or device requests.',before:identity(),rows:[],errors:[],writes:[],readPosts:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{for(const p of ['css/style.css','js/app.js']){const r=await fetch(origin+'/'+p);assert(r.ok);assert(Buffer.from(await r.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();try{
for(const language of ['en-US','zh-CN'])for(const width of [320,390,1440]){
 const page=await browser.newPage({viewport:{width,height:600}});page.on('pageerror',e=>result.errors.push(String(e)));
 await page.route('**/api/**',r=>{
 const request=r.request(),url=new URL(request.url());assert.equal(url.origin,origin);
 if(request.method()!=='GET'){
  const record={url:request.url(),method:request.method(),body:request.postData()};
  if(request.method()==='POST'&&['/api/v1/auth/status','/api/v1/device/ping'].includes(url.pathname)){result.readPosts.push(record);return r.continue();}
  result.writes.push(record);return r.abort();
 }
 return r.continue();
 });
 try{
 await page.goto(origin+'/?state=populated&lang='+language+'#/');await page.locator('[onclick="openLedModal(\'matrix\', \'filter\')"]').click();
 const modal=page.locator('#led-modal'),apply=modal.locator('#modal-apply-filter-btn');assert(await apply.isDisabled());
 const filters=await modal.locator('[data-filter]').evaluateAll(es=>es.map(e=>e.dataset.filter));assert.equal(filters.length,14);assert.equal(new Set(filters).size,14);
 for(const filter of filters){
  await modal.locator('[data-filter="'+filter+'"]').click();assert(await apply.isEnabled());
  const sliders=modal.locator('#modal-filter-params input[type="range"]'),count=await sliders.count();const controls=[];
  for(let i=0;i<count;i++){
   const e=sliders.nth(i);await e.scrollIntoViewIfNeeded();const before=await e.evaluate(n=>({id:n.id,value:Number(n.value),min:Number(n.min),max:Number(n.max),step:Number(n.step)||1}));
   await e.focus();await page.keyboard.press('ArrowRight');const expected=Math.min(before.max,before.value+before.step);assert.equal(Number(await e.inputValue()),expected);
   const label=await page.locator('#'+before.id+'-val').textContent();assert(label.startsWith(String(expected)));
   await page.keyboard.press('ArrowLeft');assert.equal(Number(await e.inputValue()),expected-before.step);
   const state=await e.evaluate(n=>{const b=n.getBoundingClientRect(),shell=n.closest('.modal-content').getBoundingClientRect();return{rect:b.toJSON(),background:getComputedStyle(n).backgroundColor,inside:b.left>=shell.left&&b.right<=shell.right&&b.top>=0&&b.bottom<=innerHeight}});assert(state.inside);
   controls.push({...before,after:Number(await e.inputValue()),labelAfterRight:label,...state});
  }
  const columns=await modal.locator('#modal-filter-params .config-row').evaluateAll(rows=>rows.map(row=>{
   const [label,range,value]=[...row.children].map(n=>n.getBoundingClientRect());
   const text=document.createRange();text.selectNodeContents(row.children[0]);const textRight=Math.max(...[...text.getClientRects()].map(r=>r.right));
   return {rangeLeft:range.left,rangeRight:range.right,valueRight:value.right,textRight,noOverlap:textRight<=range.left-4,aligned:Math.abs((range.top+range.bottom-value.top-value.bottom)/2)<=1&&Math.abs((range.top+range.bottom-label.top-label.bottom)/2)<=1};
  }));
  assert(columns.every(r=>r.aligned),'Parameter label, range and value must share a row');
  assert(columns.every(r=>r.noOverlap),'Parameter text must remain separated from its track');
  if(columns.length)assert(columns.every(r=>Math.abs(r.rangeLeft-columns[0].rangeLeft)<=1&&Math.abs(r.rangeRight-columns[0].rangeRight)<=1),'Parameter tracks must align across rows');
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth),width);
  await apply.scrollIntoViewIfNeeded();const shot=out+'/'+language+'-'+width+'-'+filter+'.png';await page.screenshot({path:shot,animations:'disabled'});
  result.rows.push({language,width,filter,controls,columns,screenshot:shot});save();
 }
 await modal.locator('.modal-close').click();await modal.waitFor({state:'hidden'});
 }finally{await page.close();}
}
assert.equal(result.rows.length,84);assert.equal(result.errors.length,0);assert.equal(result.writes.length,0);result.after=identity();assert.deepEqual(result.before,result.after);result.completed=true;
}catch(e){result.errors.push(String(e));throw e;}finally{save();await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
