async (page) => {
 const assert=require('node:assert/strict');
 const origin=new URL(page.url()).origin;if(origin!=='http://127.0.0.1:18783')throw new Error('Local fixture only');
 const results=[],requests=[],errors=[];let responseCode=0;
 page.on('pageerror',e=>errors.push(e.message));
 await page.route(origin+'/api/v1/network/hostname',route=>{
  if(route.request().method()!=='POST')return route.fulfill({json:{code:0,data:{hostname:'fixture-hostname'}}});
  requests.push({endpoint:'network/hostname',method:route.request().method(),body:route.request().postDataJSON()});
  return route.fulfill({json:responseCode===0?{code:0,data:{}}:{code:500,message:'LOCAL SYNTHETIC REJECTION'}});
 });
 await page.route(origin+'/api/v1/nat/save',route=>{requests.push({endpoint:'nat/save',method:route.request().method(),body:route.request().postData()});return route.fulfill({json:responseCode===0?{code:0,data:{}}:{code:500,message:'LOCAL SYNTHETIC REJECTION'}});});
 const open=async(lang='en-US')=>{await page.goto('about:blank');await page.goto(origin+'/?state=populated&lang='+lang+'#/network');await page.locator('#hostname-input').waitFor();await page.evaluate(()=>document.fonts.ready);await page.waitForTimeout(250);};
 await open();const phase=await page.locator('[onclick="setHostname()"]').evaluate(e=>e.classList.contains('btn-primary')?'after':'before');
 for(const width of [320,390,768,1024,1440])for(const lang of ['en-US','zh-CN']){
  await page.setViewportSize({width,height:900});await open(lang);
  const states=[];
  for(const handler of ['setHostname()','saveNatConfig()']){
   const btn=page.locator('[onclick='+JSON.stringify(handler)+']');await page.mouse.move(0,0);
   const read=()=>btn.evaluate(e=>{const s=getComputedStyle(e),r=e.getBoundingClientRect();return {text:e.textContent,handler:e.getAttribute('onclick'),class:e.className,fontSize:s.fontSize,fontWeight:s.fontWeight,bg:s.backgroundColor,color:s.color,height:r.height,width:r.width,outline:s.outline,focusVisible:e.matches(':focus-visible')};});
   await page.waitForTimeout(200);const normal=await read();await btn.hover();await page.waitForTimeout(200);const hover=await read();await page.mouse.move(0,0);await btn.focus();await page.keyboard.press('Tab');await page.keyboard.press('Shift+Tab');await page.waitForTimeout(200);const focus=await read();
   assert.equal(normal.fontSize,'13px');assert(normal.height>=30);assert.equal(focus.focusVisible,true);
   if(phase==='after'){assert.equal(normal.bg,'rgb(0, 100, 210)');assert.equal(normal.color,'rgb(255, 255, 255)');assert.equal(normal.fontWeight,'600');assert.equal(hover.bg,'rgb(5, 85, 168)');assert.equal(focus.outline,'rgb(0, 100, 210) solid 2px');}
   states.push({handler,normal,hover,focus});
  }
  const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);assert.equal(overflow,false);
  const screenshot='output/macos27-20260928/'+phase+'/submit-network-'+lang+'-'+width+'.png';await page.locator('.net-panel').nth(1).screenshot({path:screenshot});results.push({width,lang,states,overflow,screenshot});
 }
 const interactions=[];await page.setViewportSize({width:390,height:900});
 for(const name of ['hostname-empty','hostname-success','hostname-error','nat-success','nat-error']){
  await open();responseCode=name.endsWith('error')?500:0;const start=requests.length;
  if(name.startsWith('hostname')){await page.locator('#hostname-input').fill(name.endsWith('empty')?'   ':'  local-role-fixture  ');await page.locator('[onclick="setHostname()"]').click();}
  else await page.locator('[onclick="saveNatConfig()"]').click();
  const expected=name.endsWith('success')?'toast-success':'toast-error';await page.waitForFunction(c=>document.querySelector('#toast')?.classList.contains(c),expected);
  const observed=requests.slice(start),input=await page.locator('#hostname-input').inputValue(),toast=await page.locator('#toast').textContent();
  if(name==='hostname-empty')assert.equal(observed.length,0);
  else {assert.equal(observed.length,1);assert.equal(observed[0].method,'POST');if(name.startsWith('hostname'))assert.deepEqual(observed[0].body,{hostname:'local-role-fixture'});else assert.equal(observed[0].body,null);}
  if(name==='hostname-success')assert.equal(input,'');if(name==='hostname-error')assert.equal(input,'  local-role-fixture  ');
  interactions.push({name,requests:observed,input,toast,expected});
 }
 assert.deepEqual(errors,[]);return {scope:'Local fixture only. Two normal-submit roles at five widths/two languages, normal/hover/keyboard focus; actual frontend validation and success/error contract with explicit intercepted synthetic POSTs. No device writes or real auth proof.',phase,results,interactions,errors};
}
