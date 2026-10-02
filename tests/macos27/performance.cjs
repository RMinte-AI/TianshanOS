// Local A/B on actual minified/gzipped assets. Synthetic backend; not hardware proof.
const fs=require('node:fs'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out=process.argv[2]||'output/macos27-20260928/performance';fs.mkdirSync(out,{recursive:true});if(fs.existsSync(out+'/raw.json'))throw Error('Preserve existing run');
const result={environment:{started:new Date().toISOString(),node:process.version,platform:process.platform,viewport:{width:1440,height:1000},headless:true,backend:'strict local synthetic fixture',parallelResidency:true},cold:[],interactions:[],residency:{},errors:[]};
const artifactRoot=process.argv[3]||'output/macos27-20260928';
const configPaths=['components/ts_webui/web/index.html','components/ts_webui/web/css/style.css','components/ts_webui/web/js/app.js',process.argv[5]||artifactRoot+'/after/www.bin','tests/macos27/fixture-server.cjs','tests/macos27/profiles.cjs','tests/macos27/observation.js'];const configHashes=()=>Object.fromEntries(configPaths.map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex')]));const artifactDirectories=process.argv[6]?process.argv[6].split(','):[];for(const directory of artifactDirectories){for(const relative of fs.readdirSync(directory,{recursive:true})){const file=directory+'/'+relative;if(fs.statSync(file).isFile())configPaths.push(file);}}
result.configuration=configHashes();const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
const endpoints={A:18784,B:Number(process.argv[4]||18785)};result.environment.endpoints=endpoints;
const wait=ms=>new Promise(r=>setTimeout(r,ms));
async function ready(page){await page.locator('#brightness-matrix').waitFor({timeout:20000});}
async function snapshot(page,cdp){return {at:Date.now(),...await page.evaluate(()=>({dom:document.querySelectorAll('*').length,heap:performance.memory?.usedJSHeapSize,observation:window.__macos27Observation,resources:performance.getEntriesByType('resource').map(x=>({name:x.name,encoded:x.encodedBodySize,transfer:x.transferSize,duration:x.duration})),longs:window.__perfLongs})),counters:await cdp.send('Memory.getDOMCounters'),metrics:(await cdp.send('Performance.getMetrics')).metrics};}
(async()=>{
 if(artifactDirectories.length===2){
 const harnesses=[];for(const [index,label] of ['A','B'].entries()){const origin='http://127.0.0.1:'+endpoints[label];const html=await(await fetch(origin+'/')).text();const match=html.match(/<head><script>([\s\S]*?)<\/script>/);if(!match)throw Error('Missing explicit fixture '+label);harnesses.push(match[1]);for(const file of ['js/app.js','css/style.css']){const served=Buffer.from(await(await fetch(origin+'/'+file)).arrayBuffer());if(!served.equals(fs.readFileSync(artifactDirectories[index]+'/'+file)))throw Error('Served artifact mismatch '+label+' '+file);}}if(harnesses[0]!==harnesses[1])throw Error('Different fixture harnesses');result.environment.sameHarness=true;
 }
 const browser=await chromium.launch({channel:'chrome',headless:true});result.environment.browser=browser.version();
 try{
 for(let i=0;i<10;i++)for(const label of (i%2?['B','A']:['A','B'])){
  const context=await browser.newContext({viewport:result.environment.viewport});const page=await context.newPage();const cdp=await context.newCDPSession(page);await cdp.send('Network.enable');await cdp.send('Network.setCacheDisabled',{cacheDisabled:true});
  const errors=[];page.on('pageerror',e=>errors.push(String(e)));
  const start=Date.now();await page.goto('http://127.0.0.1:'+endpoints[label]+'/?state=populated&lang=zh-CN#/');await ready(page);const readyMs=Date.now()-start;await page.waitForTimeout(300);
  result.cold.push({label,iteration:i,readyMs,errors,...await page.evaluate(()=>({innerWidth,paint:performance.getEntriesByType('paint').map(x=>({name:x.name,start:x.startTime})),resources:performance.getEntriesByType('resource').map(x=>({name:x.name,encoded:x.encodedBodySize,transfer:x.transferSize})),navigation:performance.getEntriesByType('navigation').map(x=>({encoded:x.encodedBodySize,transfer:x.transferSize,duration:x.duration}))}))});await context.close();save();
 }
 const residents={};
 for(const label of ['A','B']){
  const context=await browser.newContext({viewport:result.environment.viewport});const page=await context.newPage();const cdp=await context.newCDPSession(page);await cdp.send('Performance.enable');
  await page.addInitScript(()=>{window.__perfLongs=[];new PerformanceObserver(l=>window.__perfLongs.push(...l.getEntries().map(e=>({start:e.startTime,duration:e.duration})))).observe({type:'longtask',buffered:true});});
  page.on('pageerror',e=>result.errors.push({label,error:String(e)}));await page.goto('http://127.0.0.1:'+endpoints[label]+'/?state=populated&lang=zh-CN&observe#/');await ready(page);
  residents[label]={context,page,cdp,start:Date.now()};result.residency[label]={start:Date.now(),samples:[await snapshot(page,cdp)],cycles:[]};save();
 }
 for(let cycle=0;cycle<50;cycle++)for(const label of (cycle%2?['B','A']:['A','B'])){
  const {page,cdp}=residents[label];const events=[];
  for(const [route,selector]of [['/network','#net-eth-ip'],['/files','#file-list'],['/automation','#actions-list'],['/','#brightness-matrix']]){
   const t=Date.now();await page.locator('.nav-link[href="#'+route+'"]').click();await page.locator(selector).waitFor();await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));events.push({type:'route',route,ms:Date.now()-t});
  }
  const t=Date.now();await page.locator('[onclick="showMemoryDetailModal()"]').click();await page.locator('#memory-detail-modal').waitFor();await page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));events.push({type:'modal',ms:Date.now()-t});
  await page.locator('#memory-detail-modal .modal-close').first().click();await page.locator('#memory-detail-modal').waitFor({state:'hidden'});
  result.interactions.push(...events.map(e=>({label,cycle,...e})));result.residency[label].cycles.push({cycle,at:Date.now()});
  if(cycle%5===0)result.residency[label].samples.push(await snapshot(page,cdp));save();
 }
 while(Object.values(residents).some(x=>Date.now()-x.start<1800000)){
  await wait(30000);for(const label of ['A','B'])result.residency[label].samples.push(await snapshot(residents[label].page,residents[label].cdp));save();
 }
 for(const label of ['A','B']){result.residency[label].end=Date.now();result.residency[label].durationMs=Date.now()-residents[label].start;}
 result.configurationUnchanged=JSON.stringify(result.configuration)===JSON.stringify(configHashes());result.completed=true;save();
 }catch(e){result.errors.push({fatal:String(e),stack:e.stack});save();throw e;}finally{await browser.close();}
})();
