async (page) => {
 const fs=require('node:fs');
 const root='output/macos27-20260929-native-rework';
 const previous=fs.readFileSync(root+'/shell-dedup/before.css','utf8');
 const current=fs.readFileSync('components/ts_webui/web/css/style.css','utf8');
 const origin=new URL(page.url()).origin;
 if(origin!=='http://127.0.0.1:18783')throw new Error('Source fixture only');
 const results=[],errors=[];page.on('pageerror',e=>errors.push(e.message));
 const properties=['display','position','width','height','minWidth','maxWidth','minHeight','maxHeight','padding','margin','gap','fontFamily','fontSize','fontWeight','lineHeight','color','backgroundColor','backgroundImage','border','borderRadius','boxShadow','backdropFilter','overflow','flexDirection','alignItems','justifyContent'];
 const read=()=>page.evaluate(props=>[...document.querySelectorAll('#app, #app *, .modal, .modal *')].filter(e=>e.getClientRects().length).map(e=>{const s=getComputedStyle(e);return {tag:e.tagName,id:e.id,class:e.className,style:Object.fromEntries(props.map(k=>[k,s[k]]))};}),properties);
 const scenarios=[];
 for(const width of [320,390,768,1024,1440])for(const route of ['/','/network','/files','/terminal','/automation','/commands','/security','/ota'])scenarios.push({width,route,entry:'page'});
 for(const width of [390,1440])for(const entry of ['timezone','memory','led','command','variable'])scenarios.push({width,route:entry==='command'?'/commands':'/',entry});
 for(const x of scenarios){
  await page.setViewportSize({width:x.width,height:900});await page.goto('about:blank');await page.goto(origin+'/?state=populated&lang=en-US#'+x.route);
  await page.waitForFunction(()=>[...document.styleSheets].some(s=>s.href?.includes('/css/style.css')));
  await page.evaluate(()=>document.fonts.ready);await page.waitForTimeout(350);
  const click=handler=>page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
  if(x.entry==='timezone')await click('showTimezoneModal()');
  if(x.entry==='memory')await click('showMemoryDetailModal()');
  if(x.entry==='led')await click("openLedModal('matrix', 'colorcorrection')");
  if(x.entry==='command'){await page.locator('[data-host-id="fixture-host"]').click();await click('showAddCommandModal()');await page.locator('.advanced-options summary').click();}
  if(x.entry==='variable'){await click('showWidgetManager()');await click("showWidgetEditPanel('fixture-ring')");await click('selectVariableForWidget()');await page.waitForFunction(()=>document.activeElement?.id==='var-search');}
  await page.evaluate(css=>{const link=[...document.querySelectorAll('link')].find(e=>e.href.includes('/css/style.css'));const style=document.createElement('style');style.id='dedup-comparison';style.textContent=css;link.replaceWith(style);},previous);
  await page.waitForTimeout(300);const before=await read();
  const sample=(x.width===390&&['timezone','led','variable'].includes(x.entry))||(x.width===1440&&x.entry==='page'&&x.route==='/security');
  const slug=x.entry==='page'?x.route.slice(1)||'system':x.entry;
  if(sample)await page.screenshot({path:root+'/shell-dedup/'+slug+'-'+x.width+'-before.png',fullPage:true});
  await page.evaluate(css=>document.querySelector('#dedup-comparison').textContent=css,current);await page.waitForTimeout(300);const after=await read();
  if(sample)await page.screenshot({path:root+'/shell-dedup/'+slug+'-'+x.width+'-after.png',fullPage:true});
  const differences=[];
  if(before.length!==after.length)differences.push({count:[before.length,after.length]});
  for(let i=0;i<Math.min(before.length,after.length);i++)if(JSON.stringify(before[i])!==JSON.stringify(after[i])){
   const changed=properties.filter(k=>before[i].style[k]!==after[i].style[k]);
   const rounding=changed.every(k=>['width','height','margin','padding','gap'].includes(k)&&before[i].style[k].split(' ').length===after[i].style[k].split(' ').length&&before[i].style[k].split(' ').every((v,n)=>v.endsWith('px')&&after[i].style[k].split(' ')[n].endsWith('px')&&Math.abs(parseFloat(v)-parseFloat(after[i].style[k].split(' ')[n]))<=1/16));
   differences.push({i,before:before[i],after:after[i],rounding});
  }
  results.push({...x,elements:before.length,differences});
 }
 fs.writeFileSync(root+'/shell-dedup/computed-comparison.json',JSON.stringify({scope:'Same document compares pre-dedup and current main CSS after font/load waits; 31 computed properties (geometry tolerance 1/16 CSS px, exact other styles), all visible app/modal elements. Synthetic data, no save. Not full visual acceptance.',results,errors},null,2));
 if(results.some(r=>r.differences.some(d=>!d.rounding))||errors.length)throw new Error('Styles differ; see shell-dedup/computed-comparison.json');
 return {results,errors};
}
