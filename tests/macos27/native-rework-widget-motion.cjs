// Check the real widget manager entries on immutable local release files.
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const root='output/macos27-20260929-native-rework',web=root+'/build-v19/web_optimized',out=root+'/widget-motion-v19-entries',origin='http://127.0.0.1:18812';
assert(!fs.existsSync(out));fs.mkdirSync(out);
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const identity=()=>Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p=>[p,hash(web+'/'+p)]));
const report={started:new Date().toISOString(),before:identity(),rows:[],errors:[],blocked:[],completed:false};
const save=()=>fs.writeFileSync(out+'/raw.json',JSON.stringify(report,null,2));
(async()=>{let browser;try{
 for(const p of ['css/style.css','js/app.js']){const response=await fetch(origin+'/'+p);assert(response.ok);assert(Buffer.from(await response.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)));}
 browser=await chromium.launch({channel:'chrome',headless:true});report.browser=browser.version();
 for(const mode of ['normal','motion','forced'])for(const width of [320,1440])for(const language of ['en-US','zh-CN']){
  const context=await browser.newContext({viewport:{width,height:600},deviceScaleFactor:1}),page=await context.newPage();
  try{
   page.on('pageerror',e=>report.errors.push(String(e)));
   await page.route('**/*',r=>{const q=r.request(),u=new URL(q.url());if(u.origin!==origin||(q.method()!=='GET'&&!['/api/v1/auth/status','/api/v1/device/ping'].includes(u.pathname))){report.blocked.push({method:q.method(),url:q.url()});return r.abort();}return r.continue();});
   const feature=mode==='motion'?{name:'prefers-reduced-motion',value:'reduce'}:mode==='forced'?{name:'forced-colors',value:'active'}:null;
   if(feature)await(await context.newCDPSession(page)).send('Emulation.setEmulatedMedia',{features:[feature]});
   await page.goto(origin+'/?state=populated&lang='+language+'#/');await page.locator('.dw-card').first().waitFor();await page.evaluate(()=>document.fonts.ready);
   const mainActions=await page.locator('.dw-card-actions').count();
   await page.locator('[onclick="showWidgetManager()"]').click();
   const modal=page.locator('.dw-manager-modal');await modal.waitFor();
   await page.locator('.dw-manager-item').first().hover();
   const targets={list:'.dw-manager-item, .dw-manager-item-actions, .dw-btn-icon',presets:'.dw-preset-item',types:'.dw-type-card'};
   for(const entry of ['list','presets','types']){
    if(entry==='presets')await page.locator('[onclick="showAddWidgetPanel()"]').click();
    if(entry!=='list')await page.locator(targets[entry]).first().hover();
    await page.waitForTimeout(250);
    const state=await modal.evaluate((el,selector)=>{
     const s=getComputedStyle(el),bounds=el.getBoundingClientRect();
     return {viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio,scale:visualViewport.scale},bodyScroll:el.querySelector('.modal-body').scrollTop,pageOverflow:document.documentElement.scrollWidth>innerWidth,material:{background:s.backgroundColor,filter:s.backdropFilter},media:{motion:matchMedia('(prefers-reduced-motion: reduce)').matches,forced:matchMedia('(forced-colors: active)').matches},targets:[...el.querySelectorAll(selector)].map(e=>{const c=getComputedStyle(e),b=e.getBoundingClientRect();return{classes:e.className,text:(e.title||e.textContent).trim(),disabled:!!e.disabled,transition:c.transitionDuration,easing:c.transitionTimingFunction,opacity:c.opacity,color:c.color,border:c.borderColor,background:c.backgroundColor,rect:b.toJSON(),horizontalClip:b.left<bounds.left-1||b.right>bounds.right+1};})};
    },targets[entry]);
    assert(state.targets.length>0);assert(!state.pageOverflow);assert(state.targets.every(x=>!x.horizontalClip));
    assert.equal(state.media.motion,mode==='motion');assert.equal(state.media.forced,mode==='forced');
    assert(state.targets.every(x=>x.transition===(mode==='motion'?'0s':'0.2s')),'Unexpected duration '+JSON.stringify({mode,entry,state}));
    if(mode==='forced')assert.equal(state.material.filter,'none');
    const name=[mode,language,width,entry].join('-')+'.png';await page.screenshot({path:out+'/'+name,animations:'disabled'});
    report.rows.push({mode,width,language,entry,mainActions,...state,screenshot:name,sha256:hash(out+'/'+name)});save();
   }
   await modal.locator('.modal-close').click();await modal.waitFor({state:'detached'});
  }finally{await context.close();}
 }
 report.after=identity();assert.deepEqual(report.before,report.after);assert.equal(report.errors.length,0);assert.equal(report.blocked.length,0);assert.equal(report.rows.length,36);report.completed=true;
}catch(e){report.errors.push(String(e));throw e;}finally{save();if(browser)await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
