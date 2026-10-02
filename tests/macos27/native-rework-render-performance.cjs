// Alternating A/B scroll traces on the original and current built artifacts.
// Trace CPU durations are diagnostic; they do not measure physical GPU cost.
const fs=require('node:fs'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out=process.argv[2];
if(!out||!out.startsWith('output/macos27-20260929-native-rework/')||fs.existsSync(out))throw Error('New evidence directory required');
fs.mkdirSync(out,{recursive:true});
const configs={A:{port:18784,root:'output/macos27-20260928/baseline/web_optimized'},B:{port:18791,root:'output/macos27-20260929-native-rework/build-v2/web_optimized'}};
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const identity=()=>Object.fromEntries(Object.entries(configs).map(([label,c])=>[label,Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,hash(c.root+'/'+p)]))]));
const result={started:new Date().toISOString(),configs,before:identity(),rows:[],errors:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(result,null,2));
(async()=>{
 const harnesses=[];
 for(const [label,c]of Object.entries(configs)){
  const origin='http://127.0.0.1:'+c.port;
  const html=await(await fetch(origin+'/')).text();harnesses.push(html.match(/<head><script>([\s\S]*?)<\/script>/)?.[1]);
  for(const p of ['js/app.js','css/style.css'])if(!Buffer.from(await(await fetch(origin+'/'+p)).arrayBuffer()).equals(fs.readFileSync(c.root+'/'+p)))throw Error('Artifact mismatch '+label+' '+p);
 }
 if(!harnesses[0]||harnesses[0]!==harnesses[1])throw Error('Fixture harness mismatch');
 const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();
 try{
  for(const surface of ['page','memory'])for(let trial=0;trial<10;trial++)for(const label of(trial%2?['B','A']:['A','B'])){
   const page=await browser.newPage({viewport:{width:1440,height:800},deviceScaleFactor:1});
   page.on('pageerror',e=>result.errors.push({label,surface,trial,error:String(e)}));
   try{
    const cdp=await page.context().newCDPSession(page);await cdp.send('Performance.enable');
    await page.goto('http://127.0.0.1:'+configs[label].port+'/?state=populated&lang=zh-CN#/');
    await page.locator('#brightness-matrix').waitFor();
    if(surface==='memory')await page.locator('[onclick="showMemoryDetailModal()"]').click();
    await page.waitForTimeout(350);
    const material=await page.evaluate(()=>Object.fromEntries(['.header::before','#memory-detail-modal .modal-content'].map(selector=>{
     const pseudo=selector.endsWith('::before');const e=document.querySelector(pseudo?'.header':selector);if(!e)return[selector,null];const s=getComputedStyle(e,pseudo?'::before':null);return[selector,{background:s.backgroundColor,backdrop:s.backdropFilter}];
    })));
    if(label==='B'&&surface==='memory'&&!material['#memory-detail-modal .modal-content']?.backdrop.includes('blur'))throw Error('Normal glass not active');
    const start=(await cdp.send('Performance.getMetrics')).metrics;const events=[];
    cdp.on('Tracing.dataCollected',x=>events.push(...x.value));
    await cdp.send('Tracing.start',{categories:'devtools.timeline,blink,cc',transferMode:'ReportEvents'});
    const frames=await page.evaluate(surface=>new Promise((resolve,reject)=>{
     const e=surface==='page'?document.scrollingElement:document.querySelector('#memory-detail-body');
     if(!e||e.scrollHeight-e.clientHeight<20){reject(Error('Insufficient actual scroll range'));return;}
     const deltas=[],positions=[];let n=0,previous;
     function step(t){if(previous!==undefined)deltas.push(t-previous);previous=t;e.scrollTop=(n%60)/59*(e.scrollHeight-e.clientHeight);positions.push(e.scrollTop);if(++n<120)requestAnimationFrame(step);else resolve({deltas,positions,scrollHeight:e.scrollHeight,clientHeight:e.clientHeight,viewport:{width:innerWidth,height:innerHeight}});}
     requestAnimationFrame(step);
    }),surface);
    const done=new Promise(resolve=>cdp.once('Tracing.tracingComplete',resolve));await cdp.send('Tracing.end');await done;
    if(new Set(frames.positions).size<20)throw Error('Scroll did not move through content');
    const end=(await cdp.send('Performance.getMetrics')).metrics;const summary={};
    for(const e of events){if(!/^(Paint|PrePaint|CompositeLayers|Layerize|UpdateLayerTree|DrawFrame|RasterTask|Layout|RecalculateStyles|Commit|SubmitCompositorFrame)$/.test(e.name))continue;const v=summary[e.name]??={count:0,durationUs:0};v.count++;if(e.ph==='X')v.durationUs+=e.dur||0;}
    const file=out+'/trace-'+surface+'-'+label+'-'+trial+'.json';fs.writeFileSync(file,JSON.stringify({traceEvents:events}));
    result.rows.push({label,surface,trial,material,frames,start,end,summary,trace:{path:file,sha256:hash(file)}});save();
   }finally{await page.close();}
  }
  result.after=identity();result.configurationUnchanged=JSON.stringify(result.before)===JSON.stringify(result.after);result.completed=true;save();
 }finally{await browser.close();}
})().catch(e=>{result.errors.push({fatal:String(e)});save();console.error(e);process.exitCode=1;});
