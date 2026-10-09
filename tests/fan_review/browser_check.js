async (page) => {
 const base='http://127.0.0.1:58007', results=[],errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 await page.addInitScript(()=>{window.WebSocket=class{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}};});
 await page.route('**/*',async route=>{
  const url=new URL(route.request().url());
  if(url.origin!==base){await route.abort();return;}
  if(url.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{authenticated:true,user:{role:'admin',username:'fixture'}}}});return;}
  await route.continue();
 });
 // JSON records are real source-extracted driver + production API serializer outputs.
 const packets=await (await page.request.get(base+'/evidence/repaired-repro.jsonl')).text();
 const records=packets.trim().split('\n').map(JSON.parse);
 for(const source of ['/', '/candidate/'])for(const language of ['zh-CN','en-US']){
  await page.goto(base+source);await page.evaluate(lang=>localStorage.setItem('ts_language',lang),language);await page.reload();
  await page.waitForFunction(()=>window.i18n?.isReady());await page.evaluate(async()=>{closeLoginModal();await loadSystemPage();});
  for(const r of records.filter(r=>r.case.startsWith('R2_'))){
   await page.evaluate(fan=>updateFanInfo({fans:[fan]}),r.fan);
   const expected=r.fan.duty_valid===false?'--':String(r.case==='R2_inverted'?100-r.hal_output:r.hal_output);
   const big=await page.locator('.fan-speed-num').textContent();if(big!==expected)throw Error(r.case+': '+big+' != '+expected);
   if(await page.locator('#fan-global-duty').textContent()!==expected+'%')throw Error('global '+r.case);
   if(expected==='--'&&(!await page.locator('.fan-slider').isDisabled()||await page.locator('.fan-slider-value').textContent()!=='--%'))throw Error('unknown slider');
   if(r.case==='R2_manual_fail'&&!(await page.locator('.fan-request').textContent()).includes('70%'))throw Error('requested value lost');
   results.push({source,language,case:r.case,big});
  }
  const fresh={id:0,mode:'manual',duty:37,duty_valid:true,target_duty:37,enabled:true};
  await page.evaluate(f=>{window.fixtureFan=f;fanSpeedDrafts.clear();window.fanCalls=0;window.fanOutcome='success';api.fanSet=async(id,speed)=>{window.fanCalls++;if(window.fanOutcome==='unknown')throw new Error('fixture timeout');if(window.fanOutcome==='httpfail')throw new ApiOperationError({code:9},{},{httpStatus:500});if(window.fanOutcome==='fail')return{code:9};return{code:0,data:{enabled:true}};};api.call=async()=>({code:0,data:{fans:[window.fixtureFan]}});updateFanInfo({fans:[f]});},fresh);
  const slider=page.locator('.fan-slider');await slider.focus();await slider.evaluate(el=>{el.value='70';el.dispatchEvent(new Event('input',{bubbles:true}));});
  if(await page.locator('.fan-speed-num').textContent()!=='37')throw Error('draft replaced successful output');
  await page.evaluate(()=>{window.originalSlider=document.querySelector('.fan-slider');updateFanInfo({fans:[fixtureFan]});});
  if(!await page.evaluate(()=>originalSlider===document.querySelector('.fan-slider')&&originalSlider.value==='70'))throw Error('poll replaced active draft input');
  if(!(await page.locator('.fan-draft').textContent()).includes('70%'))throw Error('draft label');
  if(await page.evaluate(()=>fanCalls)!==0)throw Error('input sent command');
  await page.screenshot({path:`output/fan-fall-review-repair-20261009/${source==='/'?'source':'built'}-${language}-draft.png`,fullPage:true});
  for(const outcome of ['fail','httpfail','unknown','success']){
   await page.evaluate(outcome=>{fanOutcome=outcome;fixtureFan.duty=outcome==='success'?70:37;fixtureFan.target_duty=outcome==='success'?70:37;updateFanInfo({fans:[fixtureFan]});},outcome);
   await page.evaluate(async()=>{await setFanSpeed(0,70);});
   const toast=await page.locator('#toast').textContent();
   const expected=outcome==='success'?(language==='zh-CN'?'风扇0已设为70%。':'Fan 0 set to 70%.'):outcome==='unknown'?(language==='zh-CN'?'暂时无法确认':'Unable to confirm'):(language==='zh-CN'?'风扇0未能改为70%。':'Could not set fan 0 to 70%.');
   if(!toast.includes(expected)||toast.includes('fixture')||toast.includes('fan.setting'))throw Error(outcome+' toast: '+toast);
   const big=await page.locator('.fan-speed-num').textContent();if(big!==(outcome==='unknown'?'--':outcome==='success'?'70':'37'))throw Error(outcome+' output: '+big);
   if(outcome==='unknown'&&(!await page.locator('.fan-slider').isDisabled()||await page.locator('.fan-draft').textContent()!==''))throw Error('unknown keeps active draft');
   results.push({source,language,case:outcome,toast,big});
  }
  if(await page.evaluate(()=>fanCalls)!==4)throw Error('unexpected repeated command');
  await page.evaluate(async()=>{api.call=async()=>{throw Error('fixture status timeout');};await refreshFans();});
  if(await page.locator('.fan-speed-num').textContent()!=='--'||await page.locator('#fan-global-duty').textContent()!=='--%')throw Error('old reading presented after status failure');
  results.push({source,language,case:'status read failure: old output invalidated'});
  await page.evaluate(async()=>{fixtureFan.enabled=false;fixtureFan.duty=0;fixtureFan.target_duty=70;api.fanSet=async()=>({code:0,data:{enabled:false}});api.call=async()=>({code:0,data:{fans:[fixtureFan]}});await setFanSpeed(0,70);});
  const disabledToast=await page.locator('#toast').textContent();
  if(!disabledToast.includes(language==='zh-CN'?'当前处于停用状态':'currently disabled')||await page.locator('.fan-speed-num').textContent()!=='0')throw Error('disabled setting misreported as applied 70');
  results.push({source,language,case:'disabled success',toast:disabledToast});
  for(const mode of ['auto','curve','manual','off']){
   await page.evaluate(mode=>updateFanInfo({fans:[{id:0,mode,target_duty:70,enabled:true}]}),mode);
   if(await page.locator('.fan-speed-num').textContent()!=='--'||await page.locator('#fan-global-duty').textContent()!=='--%'||await page.locator('.fan-slider-value').textContent()!=='--%')throw Error('missing output in '+mode);
   results.push({source,language,case:'unknown_'+mode});
  }
  await page.setViewportSize({width:390,height:844});await page.evaluate(()=>updateFanInfo({fans:[fixtureFan]}));
  const overflow=await page.locator('.fan-card').evaluate(el=>el.scrollWidth>el.clientWidth+1);if(overflow)throw Error('mobile fan card overflow');
  results.push({source,language,case:'390px layout',overflow});await page.setViewportSize({width:1280,height:900});
  const help=await page.evaluate(()=>{showFanAutoHelpModal();const txt=document.getElementById('fan-auto-help-modal').textContent;closeFanAutoHelpModal();return txt;});
  if(!help||help.includes('fanPage.'))throw Error('help translation');
 }
 if(errors.length)throw Error(JSON.stringify(errors));
 return{checks:results.length,results,script_errors:errors,source_and_built_checked:true,mock_only:true};
}
