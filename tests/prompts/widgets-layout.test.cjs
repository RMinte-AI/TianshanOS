const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const root=path.resolve(__dirname,'../../components/ts_webui/web');
const source=fs.readFileSync(path.join(root,'js/app.js'),'utf8');
const css=fs.readFileSync(process.env.WIDGET_CSS_PATH || path.join(root,'css/style.css'),'utf8');
const extract=(name,next)=>source.slice(source.indexOf(`function ${name}(`),source.indexOf(next,source.indexOf(`function ${name}(`)));
const functions=extract('renderWidgetHtml','/**\n * 渲染所有组件')+'\n'+extract('updateWidgetValue','/**\n * 初始化数据组件面板');
const iconHelpers=source.split('\n').filter(line=>line.startsWith('const ic =')||line.startsWith('const iconize =')).join('\n');
const types=['ring','gauge','temp','number','bar','text','status','icon','dual','percent','log'];
let browser;
before(async()=>{browser=await chromium.launch({channel:'chrome',headless:true});});
after(async()=>{await browser?.close();});
async function fixture(width,language='en-US'){
 const page=await browser.newPage({viewport:{width,height:1000}});
 await page.setContent(`<style>${css}</style><main style="padding:16px"><div class="dw-grid" id="grid"></div></main>`);
 await page.addScriptTag({content:iconHelpers+'\n'+functions+`;window.i18n={registerLanguage:(id,data)=>window.language=data};window.escapeHtml=s=>s;window.sanitizeWidgetIcon=()=>'';`});
 await page.addScriptTag({content:fs.readFileSync(path.join(root,'js/lang',language+'.js'),'utf8')});
 await page.evaluate(()=>{window.t=key=>key.split('.').reduce((o,k)=>o?.[k],window.language)||key;});
 await page.evaluate(()=>document.fonts.ready);
 await page.evaluate(types=>{
  window.widgets=types.map(type=>({id:type,type,label:type,color:'#da77f2',unit:' MB',decimals:1,subValue:65536,_isCollapsed:false,layout:type==='log'?'small':'auto'}));
  document.getElementById('grid').innerHTML=widgets.map(renderWidgetHtml).join('');
 },types);
 return page;
}
async function populate(page,long){
 await page.evaluate(long=>{
  for(const w of widgets){
   const value=['text','icon'].includes(w.type)?(long?'LongUnbrokenDeviceStatus'.repeat(8)+'设备状态说明'.repeat(8):'正常'):(long?123456789012.3:98.1);
   w.subValue=long?65536:64;updateWidgetValue(w,value);
  }
  document.querySelector('.dw-log-container').innerHTML='<div class="dw-log-line">'+ 'long-log-message '.repeat(40)+'</div>';
 },long);
}
async function violations(page){
 return page.evaluate(()=>{
  const issues=[];
  for(const card of document.querySelectorAll('.dw-card')){
   const bounds=card.getBoundingClientRect(),header=card.querySelector('.t-label').getBoundingClientRect();
   for(const el of card.querySelectorAll('[id], .t-unit, .dw-log-tools button, .dw-log-tools .state')){
    if(el instanceof SVGElement || el.closest('.dw-log-container'))continue; // Only log lines intentionally truncate.
    const box=el.getBoundingClientRect();
    if(box.left<bounds.left-1||box.right>bounds.right+1)issues.push({type:card.dataset.widgetId,element:el.className,boxOverflow:true});
    const range=document.createRange();range.selectNodeContents(el);
    for(const r of range.getClientRects())if(r.left<bounds.left-1||r.right>bounds.right+1||r.top<(card.dataset.type==='log'?bounds.top:header.bottom)-1||r.bottom>bounds.bottom+1)issues.push({type:card.dataset.widgetId,element:el.className,text:el.textContent});
   }
  }
  if(document.documentElement.scrollWidth>innerWidth)issues.push({pageOverflow:true});
  return issues;
 });
}
for(const width of [375,600,768,900,1024,1200,1440]){
 test(`${width}px: all widget values stay inside cards and preserve grid widths`,async()=>{
  const page=await fixture(width);
  try{
   await populate(page,false);
   const shortWidths=await page.locator('.dw-card').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().width));
   const shortHeights=await page.locator('.dw-card').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().height));
   assert.deepEqual(await violations(page),[]);
   await populate(page,true);
   assert.deepEqual(await violations(page),[]);
   const longWidths=await page.locator('.dw-card').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().width));
   assert.deepEqual(longWidths,shortWidths);
   assert.equal(await page.locator('#dw-number-value').textContent(),'123456789012.3');
   assert.equal(await page.locator('.dw-log-line').evaluate(e=>getComputedStyle(e).textOverflow),'ellipsis');
   await populate(page,false);
   assert.deepEqual(await violations(page),[]);
   assert.deepEqual(await page.locator('.dw-card').evaluateAll(es=>es.map(e=>e.getBoundingClientRect().height)),shortHeights);
  }finally{await page.close();}
 });
}
test('wide ring labels overlay the whole card; narrow dual values stack',async()=>{
 const page=await fixture(1024);
 try{
  await page.evaluate(()=>{
   document.getElementById('grid').style.gridTemplateColumns='repeat(3,minmax(0,1fr))';
   for(const type of ['ring','gauge'])updateWidgetValue({id:type,type,decimals:1,unit:' MB'},12345.6);
  });
  for(const type of ['ring','gauge']){
   const m=await page.locator(`.dw-card[data-type=${type}] .dw-ring`).evaluate(e=>{
    const value=e.querySelector('.dw-ring-value'),svg=e.querySelector('svg');
    return {container:e.clientWidth,value:value.clientWidth,height:value.clientHeight,svg:svg.clientWidth,position:getComputedStyle(value).position};
   });
   assert.equal(m.value,m.container);assert(m.value>m.svg);assert(m.height<30);
  }
  await page.setViewportSize({width:600,height:1000});
  await page.evaluate(()=>{document.getElementById('grid').style.gridTemplateColumns='repeat(3,minmax(0,1fr))';updateWidgetValue({id:'dual',type:'dual',decimals:1,subValue:65536},32768.8);});
  assert.equal(await page.locator('.dw-card[data-type=dual] .bigrow').evaluate(e=>getComputedStyle(e).flexDirection),'column');
  assert.equal(await page.locator('.dw-card[data-type=dual] .bigrow').evaluate(e=>getComputedStyle(e).alignItems),'center');
  assert.deepEqual(await violations(page),[]);
 }finally{await page.close();}
});

test('180px cards contain long units and labels and remain editable',async()=>{
 const page=await fixture(1024);
 try{
  await page.evaluate(()=>{
   document.getElementById('grid').style.gridTemplateColumns='repeat(3,minmax(0,1fr))';
   document.getElementById('grid').style.width='564px';
   window.edited=[];window.showWidgetManager=id=>edited.push(id);
   for(const w of widgets){
    w.label='LongUnbrokenComponentLabel'.repeat(4);
    w.unit=' VeryLongUnbrokenUnitName'.repeat(3);
   }
   document.getElementById('grid').innerHTML=widgets.map(renderWidgetHtml).join('');
  });
  await populate(page,true);assert.deepEqual(await violations(page),[]);
  await page.locator('#dw-ring-value').click();
  assert.deepEqual(await page.evaluate(()=>edited),['ring']);
  await page.evaluate(()=>updateWidgetValue({id:'ring',type:'ring'},null));
  assert.equal(await page.locator('#dw-ring-value').textContent(),'-');
  assert.equal(await page.locator('#dw-ring-ring').evaluate(e=>e.getAttribute('stroke-dasharray')),'0.0 163.4');
 }finally{await page.close();}
});

test('widget layout visual samples',async()=>{
 const page=await fixture(1024);
 try{
  await populate(page,false);
  await page.evaluate(()=>{
   for(const w of widgets){
    w.subValue=65536;
    updateWidgetValue(w,['text','icon'].includes(w.type)?'设备运行正常，当前模型正在加载':w.type==='percent'?100:w.type==='temp'?53:12345.6);
   }
  });
  const dir=path.resolve(__dirname,'../../output/widget-layout');fs.mkdirSync(dir,{recursive:true});
  await page.screenshot({path:path.join(dir,'desktop.png'),fullPage:true});
  await page.setViewportSize({width:375,height:1000});
  await page.screenshot({path:path.join(dir,'mobile.png'),fullPage:true});
  assert.deepEqual(await violations(page),[]);
 }finally{await page.close();}
});

for(const width of [375,600,601,768,900,901,1200,1201,1440]){
 test(`${width}px: mixed layout sizes never create implicit grid columns`,async()=>{
  const page=await fixture(width);
  try{
   const expectedColumns=width<=600?1:width<=900?2:width<=1200?3:4;
   await page.evaluate(()=>{
    widgets=['large','medium','small','auto','full'].map(layout=>({id:layout,type:'ring',label:'GPU',layout,unit:'MB',decimals:1}));
    document.getElementById('grid').innerHTML=widgets.map(renderWidgetHtml).join('');
    widgets.forEach(w=>updateWidgetValue(w,98.1));
   });
   const measure=()=>page.evaluate(()=>{
    const g=document.getElementById('grid'),columns=getComputedStyle(g).gridTemplateColumns.split(' ').map(Number.parseFloat);
    return {columns,cards:[...g.children].map(c=>({layout:c.dataset.layout,width:c.getBoundingClientRect().width})),gridWidth:g.clientWidth};
   });
   const m=await measure();assert.equal(m.columns.length,expectedColumns);
   assert(m.columns.every(n=>Math.abs(n-m.columns[0])<1));
   for(const c of m.cards){const span=Math.min(expectedColumns,{large:3,medium:2,small:1,auto:1,full:expectedColumns}[c.layout]);assert(Math.abs(c.width-(m.columns[0]*span+12*(span-1)))<1,JSON.stringify(c));}
   assert.deepEqual(await violations(page),[]);
   await page.setViewportSize({width:375,height:1000});assert.equal((await measure()).columns.length,1);
   await page.setViewportSize({width:1440,height:1000});assert.equal((await measure()).columns.length,4);
   assert.deepEqual(await violations(page),[]);
  }finally{await page.close();}
 });
}
for(const language of ['zh-CN','en-US']){
 test(`${language}: narrow log toolbar contains controls and status in both states`,async()=>{
  const page=await fixture(1024,language);
  try{
   await page.evaluate(()=>{document.getElementById('grid').style.width='564px';});
   for(const reading of [false,true])for(const collapsed of [false,true]){
    await page.evaluate(({reading,collapsed})=>{
     widgets=[{id:'log',type:'log',label:'Log',layout:'small',_isReading:reading,_isCollapsed:collapsed},{id:'ring',type:'ring',label:'GPU'}];
     document.getElementById('grid').innerHTML=widgets.map(renderWidgetHtml).join('');
     document.querySelector('.dw-log-container').innerHTML='<div class="dw-log-line">'+ 'long-log-message '.repeat(40)+'</div>';
     window.actions=[];window.toggleLogReading=id=>actions.push(id);
    },{reading,collapsed});
    assert.deepEqual(await violations(page),[]);
    await page.locator('#dw-log-toggle').click();assert.deepEqual(await page.evaluate(()=>actions),['log']);
    if(!collapsed)assert.equal(await page.locator('.dw-log-line').evaluate(e=>getComputedStyle(e).textOverflow),'ellipsis');
   }
  }finally{await page.close();}
 });
}

test('device service cards cap at three columns across the fan breakpoint',async()=>{
 const page=await browser.newPage();
 try{
  await page.setContent(`<style>${css}</style><main style="padding:24px"><div class="sys-mid"><section class="card" style="padding:20px;min-width:0"><div class="qa-grid">${Array.from({length:5},()=>'<div class="quick-action-card">Qwen3.6-35B-A3B</div>').join('')}</div></section><section class="card">Fan</section></div></main>`);
  for(const width of [375,600,768,1024,1374,1389,1390,1399,1440,1920,2560]){
   await page.setViewportSize({width,height:1000});
   const result=await page.locator('.qa-grid').evaluate(e=>({columns:getComputedStyle(e).gridTemplateColumns.split(' ').length,overflow:document.documentElement.scrollWidth>innerWidth}));
   assert(result.columns<=3,`${width}px unexpectedly has ${result.columns} columns`);
   if(width>=1374)assert.equal(result.columns,3,`${width}px should fit three cards`);
   assert.equal(result.overflow,false,`${width}px overflows`);
  }
 }finally{await page.close();}
});
