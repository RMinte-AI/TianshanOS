// Real browser zoom in disposable profiles; viewport capture avoids CSS-pixel clip at 200%.
const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
(async()=>{
 const results=[];
 for(const scale of [1,2]) {
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'macos27-rework-zoom-'));
  const context=await chromium.launchPersistentContext(dir,{channel:'chrome',headless:true,viewport:null,args:['--window-size=1440,1000']});
  try {
   const page=context.pages()[0],cdp=await context.newCDPSession(page);
   await page.goto('chrome://settings/appearance');await page.locator('#zoomLevel').selectOption(String(scale));
   for(const route of ['/','/network','/files','/terminal','/automation','/commands','/security','/ota']) {
    await page.goto('http://127.0.0.1:18783/?state=populated&lang=en-US#'+route);await page.waitForTimeout(500);await page.evaluate(()=>document.fonts.ready);
    for(const [position,fraction] of [['top',0],['middle',0.5],['bottom',1]]) {
    await page.evaluate(f=>scrollTo(0,(document.documentElement.scrollHeight-innerHeight)*f),fraction);
    const state=await page.evaluate(()=>({innerWidth,innerHeight,outerWidth,outerHeight,scrollY,scrollHeight:document.documentElement.scrollHeight,dpr:devicePixelRatio,scale:visualViewport.scale,overflow:document.documentElement.scrollWidth>innerWidth,content:document.getElementById('page-content').getBoundingClientRect().toJSON()}));
    if(state.dpr!==scale||state.innerWidth!==1440/scale||state.overflow)throw new Error('Zoom layout mismatch '+JSON.stringify({route,requestedScale:scale,...state}));
    const shot=await cdp.send('Page.captureScreenshot',{format:'png',fromSurface:true});const bytes=Buffer.from(shot.data,'base64');
    const screenshot='output/macos27-20260928/after/viewport-zoom-'+scale+'-'+(route.slice(1)||'system')+'-'+position+'.png';fs.writeFileSync(screenshot,bytes);
    const pixelWidth=bytes.readUInt32BE(16),pixelHeight=bytes.readUInt32BE(20);
    results.push({requestedScale:scale,route,position,screenshot,pixelWidth,pixelHeight,...state});
    }
   }
  } finally {await context.close();}
 }
 fs.writeFileSync('output/macos27-20260928/after/native-rework-zoom.json',JSON.stringify({scope:'Disposable Chrome profiles, actual zoom preference and DPR/layout checked. CDP fromSurface:true without clip captures browser surface; dimensions recorded. No user profile changes.',results},null,2));
})().catch(e=>{console.error(e);process.exitCode=1;});
