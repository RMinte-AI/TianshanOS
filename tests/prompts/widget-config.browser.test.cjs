const {test}=require('node:test'),assert=require('node:assert/strict');
const kit=require('./pagekit.cjs');
kit.install();

for(const language of ['zh-CN','en-US'])test(`${language}: widget read failure cannot migrate or overwrite browser data`,async()=>{
 let mode='failure';
 const respond=({endpoint})=>{
  if(endpoint==='ui/widgets/get')return mode==='failure'?{code:6,error:'widgets_load_failed'}:{code:0,data:{widgets:[],refresh_interval:5000,source:'default'}};
  if(endpoint==='ui/widgets/set')return {code:0,data:{nvs_saved:true}};
  return {code:0,data:{}};
 };
 await kit.withPage(language,respond,async({page,calls})=>{
  await page.evaluate(()=>{localStorage.setItem('data_widgets_v2',JSON.stringify([{id:'old',type:'number',label:'old'}]));localStorage.setItem('data_widgets_refresh_interval','0');});
  const before=calls.filter(c=>c.endpoint==='ui/widgets/set').length;
  assert.equal(await page.evaluate(()=>loadDataWidgets()),false);
  assert.equal(calls.filter(c=>c.endpoint==='ui/widgets/set').length,before);
  await page.evaluate(()=>showWidgetManager());
  assert.equal(await page.locator('#widget-manager-modal').count(),0);
  mode='default';assert.equal(await page.evaluate(()=>loadDataWidgets()),true);
  const saves=calls.filter(c=>c.endpoint==='ui/widgets/set');
  assert.equal(saves.length,before+1);assert.equal(saves.at(-1).params.widgets[0].id,'old');assert.equal(saves.at(-1).params.refresh_interval,0);
 });
});

test('saved device refresh interval takes precedence over stale browser settings',async()=>{
 const respond=({endpoint})=>endpoint==='ui/widgets/get'?{code:0,data:{widgets:[],refresh_interval:0,source:'nvs'}}:{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>localStorage.setItem('data_widgets_refresh_interval','5000'));
  await page.evaluate(()=>initDataWidgets());
  assert.equal(await page.evaluate(()=>dataWidgetsRefreshInterval),0);
 });
});

test('widget saves are serialized and a late result cannot close a newer editor',async()=>{
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],refresh_interval:5000,source:'nvs'}}
  :{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());
  const requests=[],routes=[];let notify;
  const arrival=()=>new Promise(resolve=>{notify=resolve;});
  await page.route('**/api/v1/ui/widgets/set',route=>{requests.push(route.request().postDataJSON());routes.push(route);notify?.();});
  await page.evaluate(()=>showWidgetEditPanel('w1'));
  await page.locator('#edit-label').fill('First');
  let arrived=arrival();await page.evaluate(()=>{void saveWidgetEdit('w1');});await arrived;
  await page.evaluate(()=>showWidgetEditPanel('w1'));
  await page.locator('#edit-label').fill('Second');
  await page.evaluate(()=>{void saveWidgetEdit('w1');});
  assert.equal(requests.length,1);
  arrived=arrival();await routes[0].fulfill({json:{code:0,data:{nvs_saved:true}}});await arrived;
  assert.equal(await page.locator('#edit-label').inputValue(),'Second');
  assert.equal(requests[0].widgets[0].label,'First');assert.equal(requests[1].widgets[0].label,'Second');
  await routes[1].fulfill({json:{code:0,data:{nvs_saved:true}}});
  await page.waitForFunction(()=>document.getElementById('widget-edit-modal')?.dataset.configurationClosed==='true');
 });
});

test('a failed widget save keeps the edited panel open',async()=>{
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],source:'nvs'}}
  :endpoint==='ui/widgets/set'?{code:6,error:'widgets_save_failed'}:{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());await page.evaluate(()=>showWidgetEditPanel('w1'));
  await page.locator('#edit-label').fill('Unsaved edit');
  await page.evaluate(()=>saveWidgetEdit('w1'));
  assert.equal(await page.locator('#edit-label').inputValue(),'Unsaved edit');
  assert.equal(await page.evaluate(()=>dataWidgets[0].label),'Unsaved edit');
  assert.notEqual(await page.locator('#widget-edit-modal').getAttribute('data-configuration-closed'),'true');
  await kit.toastIs(page,'error');
 });
});

test('an interrupted widget save is reported as unconfirmed',async()=>{
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],source:'nvs'}}
  :{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());await page.evaluate(()=>showWidgetEditPanel('w1'));
  await page.route('**/api/v1/ui/widgets/set',route=>route.abort());
  await page.locator('#edit-label').fill('Unconfirmed edit');
  await page.evaluate(()=>saveWidgetEdit('w1'));
  assert.equal(await page.locator('#edit-label').inputValue(),'Unconfirmed edit');
  await kit.toastIs(page,'warning');
 });
});

test('a widget reload waits for queued saves before replacing the in-memory data',async()=>{
 let device=[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],reads=0;
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?(reads++,{code:0,data:{widgets:device,refresh_interval:5000,source:'nvs'}}):{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());const routes=[],writes=[];let notify;
  const arrival=()=>new Promise(resolve=>notify=resolve);
  await page.route('**/api/v1/ui/widgets/set',route=>{routes.push(route);writes.push(route.request().postDataJSON());notify?.();});
  await page.evaluate(()=>showWidgetEditPanel('w1'));await page.locator('#edit-label').fill('First');
  let next=arrival();await page.evaluate(()=>{void saveWidgetEdit('w1');});await next;
  await page.evaluate(()=>showWidgetEditPanel('w1'));await page.locator('#edit-label').fill('Second');
  await page.evaluate(()=>{void saveWidgetEdit('w1');window.widgetReloadDone=false;void loadDataWidgets().then(()=>widgetReloadDone=true);});
  assert.equal(reads,1);
  device=writes[0].widgets;next=arrival();await routes[0].fulfill({json:{code:0,data:{nvs_saved:true}}});await next;
  assert.equal(reads,1);assert.equal(writes[1].widgets[0].label,'Second');
  device=writes[1].widgets;await routes[1].fulfill({json:{code:0,data:{nvs_saved:true}}});
  await page.waitForFunction(()=>widgetReloadDone);
  assert.equal(reads,2);assert.equal(await page.evaluate(()=>dataWidgets[0].label),'Second');
 });
});

test('a failed widget save remains editable after a panel reload',async()=>{
 let reads=0;
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?(reads++,{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],source:'nvs'}})
  :endpoint==='ui/widgets/set'?{code:6,error:'widgets_save_failed'}:{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());await page.evaluate(()=>showWidgetEditPanel('w1'));
  await page.locator('#edit-label').fill('Unsaved edit');await page.evaluate(()=>saveWidgetEdit('w1'));
  assert.equal(await page.evaluate(()=>loadDataWidgets()),true);
  assert.equal(reads,1);assert.equal(await page.evaluate(()=>dataWidgets[0].label),'Unsaved edit');
  assert.equal(await page.locator('#edit-label').inputValue(),'Unsaved edit');
 });
});

test('a widget read started before a later save cannot restore old data',async()=>{
 const respond=({endpoint})=>endpoint==='ui/widgets/get'
  ?{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original',min:0,max:100,decimals:1}],source:'nvs'}}
  :endpoint==='ui/widgets/set'?{code:0,data:{nvs_saved:true}}:{code:0,data:{}};
 await kit.withPage('en-US',respond,async({page})=>{
  await page.evaluate(()=>loadDataWidgets());let route,notify;
  const arrival=new Promise(resolve=>notify=resolve);
  await page.route('**/api/v1/ui/widgets/get',r=>{route=r;notify();});
  await page.evaluate(()=>{window.widgetReloadDone=false;void loadDataWidgets().then(()=>widgetReloadDone=true);});await arrival;
  await page.evaluate(()=>showWidgetEditPanel('w1'));await page.locator('#edit-label').fill('New value');
  await page.evaluate(()=>saveWidgetEdit('w1'));
  await route.fulfill({json:{code:0,data:{widgets:[{id:'w1',type:'number',label:'Original'}],source:'nvs'}}});
  await page.waitForFunction(()=>widgetReloadDone);
  assert.equal(await page.evaluate(()=>dataWidgets[0].label),'New value');
 });
});
