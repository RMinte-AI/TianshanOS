const {test}=require('node:test'),assert=require('node:assert/strict');
const kit=require('./pagekit.cjs');
kit.install();

// F47 drives refreshDataWidgets through the real variables API and reads the DOM.
async function dualPage(language,{create=false,width=1280,extra=[]}={}) {
 let widgets=create?[]:[{id:'dual-probe',type:'dual',label:'Dual',decimals:1,expression:'${main}',expression2:'${sub}',layout:'auto'}],values={};
 widgets.push(...extra);const writes=[];
 const fixture=await kit.open(language,({endpoint,params})=>{
  if(endpoint==='auth/login')return {code:0,data:{token:'widget-session',username:'admin',level:'admin',expires_in:3600,password_changed:true}};
  if(endpoint==='auth/status')return {code:0,data:{valid:true}};
  if(endpoint==='ui/widgets/get')return {code:0,data:{widgets,refresh_interval:0,source:'nvs'}};
  if(endpoint==='ui/widgets/set'){widgets=structuredClone(params.widgets);writes.push(structuredClone(params));return {code:0,data:{nvs_saved:true}};}
  if(endpoint==='automation/variables/list')return {code:0,data:{variables:Object.entries(values).map(([name,value])=>({name,value}))}};
  if(endpoint==='automation/variables/get')return {code:0,data:{value:'line one\nline two'}};
  return {code:0,data:{}};
 },{viewport:{width,height:1000}});
 const {page}=fixture;
 page.setDefaultTimeout(5000);
 try {
 await page.waitForFunction(()=>router.appReady);
 await page.evaluate(()=>showLoginModal());await page.locator('#username').fill('admin');await page.locator('#password').fill('local-test-password');
 await page.locator('#login-form button[type="submit"]').click();
 await page.waitForFunction(()=>document.getElementById('data-widgets-grid'));
 await page.evaluate(()=>initDataWidgets());await page.waitForFunction(()=>!dataWidgetsRefreshing);
 await page.evaluate(()=>stopDataWidgetsAutoRefresh());
 } catch(error) {
  console.log('F47 setup failure',await page.evaluate(()=>({token:api.token,username:api.username,error:document.getElementById('login-error').textContent,loaded:dataWidgetsLoaded,refreshing:dataWidgetsRefreshing,grid:!!document.getElementById('data-widgets-grid')})));
  await fixture.close();throw error;
 }
 return {...fixture,writes,stored:()=>structuredClone(widgets),refresh:async next=>{
  values=next;await page.evaluate(()=>refreshDataWidgets());
  return page.evaluate(()=>{const w=dataWidgets.find(w=>w.type==='dual');return w?[document.getElementById(`dw-${w.id}-value`).textContent,document.getElementById(`dw-${w.id}-sub`).textContent]:null;});
 }};
}
async function createDual(page) {
 await page.evaluate(()=>showAddWidgetPanel());
 await page.locator('[onclick="createNewWidget(\'dual\')"]').click();
 const id=await page.evaluate(()=>dataWidgets.find(w=>w.type==='dual').id);
 await page.locator('#edit-label').fill('Live dual');await page.locator('#edit-expression').fill('${main}');
 await page.locator('#edit-expression2').fill('${sub}');await page.locator('#edit-decimals').fill('1');
 await page.locator('#widget-edit-modal button[onclick^="saveWidgetEdit"]').click();
 await page.waitForFunction(()=>document.getElementById('widget-edit-modal')?.dataset.configurationClosed==='true'&&!dataWidgetsRefreshing);
 return id;
}
for(const language of ['zh-CN','en-US']) {
 test(`F47 normal: create save edit cancel and reload configuration ${language}`,async()=>{
  const f=await dualPage(language,{create:true});const {page}=f;try{
   const id=await createDual(page);assert.equal(f.stored().find(w=>w.id===id).expression2,'${sub}');
   await page.evaluate(id=>showWidgetEditPanel(id),id);await page.locator('#edit-expression2').fill('${other}');
   await page.locator('#widget-edit-modal button[onclick^="saveWidgetEdit"]').click();
   await page.waitForFunction(()=>document.getElementById('widget-edit-modal')?.dataset.configurationClosed==='true'&&!dataWidgetsRefreshing);
   await page.reload();await page.waitForFunction(()=>dataWidgetsLoaded&&!dataWidgetsRefreshing);
   assert.equal(f.stored().find(w=>w.id===id).expression2,'${other}');
   await page.evaluate(id=>showWidgetEditPanel(id),id);await page.locator('#edit-expression2').fill('${cancelled}');
   await page.locator('#widget-edit-modal button[onclick="closeModal(\'widget-edit-modal\')"]').click();
   await page.evaluate(()=>loadDataWidgets());assert.equal(await page.evaluate(id=>dataWidgets.find(w=>w.id===id).expression2,id),'${other}');
   assert.equal(f.stored().find(w=>w.id===id).expression2,'${other}');assert(f.writes.length>=3);assert.deepEqual(f.errors,[]);
  }finally{await f.close();}
 });
 test(`F47 defect: created dual displays both values in each response ${language}`,async()=>{
  const f=await dualPage(language,{create:true});try{
   await createDual(f.page);const actual=[];
   actual.push(await f.refresh({main:10,sub:20}));actual.push(await f.refresh({main:11,sub:21}));
   console.log(JSON.stringify({case:'F47 two rounds',language,actual}));
   assert.deepEqual(actual,[['10.0','20.0'],['11.0','21.0']]);assert.deepEqual(f.errors,[]);
  }finally{await f.close();}
 });
 const samples=[
  ['main-missing',{sub:31},['-','31.0']],['sub-missing',{main:12},['12.0','-']],
  ['both-missing',{},['-','-']],['nulls',{main:null,sub:null},['-','-']],
  ['zeros',{main:0,sub:0},['0.0','0.0']],['decimals',{main:1.25,sub:2.75},['1.3','2.8']],
  ['text',{main:'Ready',sub:'Standby'},['Ready','Standby']]
 ];
 for(const [name,values,expected] of samples)test(`F47 defect: independent ${name} and recovery ${language}`,async()=>{
  const f=await dualPage(language);try{
   await f.refresh({main:10,sub:20});await f.refresh({main:10,sub:20});
   const actual=await f.refresh(values),recovered=await f.refresh({main:11,sub:21});
   console.log(JSON.stringify({case:`F47 ${name}`,language,actual,recovered}));
   assert.deepEqual({actual,recovered},{actual:expected,recovered:['11.0','21.0']});assert.deepEqual(f.errors,[]);
  }finally{await f.close();}
 });
 test(`F47 defect: saved secondary expression and removal clear old display ${language}`,async()=>{
  const f=await dualPage(language),{page}=f;try{
   await f.refresh({main:10,sub:20,other:42});await f.refresh({main:10,sub:20,other:42});
   await page.evaluate(()=>showWidgetEditPanel('dual-probe'));await page.locator('#edit-expression2').fill('${other}');
   await page.locator('#widget-edit-modal button[onclick^="saveWidgetEdit"]').click();
   await page.waitForFunction(()=>document.getElementById('widget-edit-modal')?.dataset.configurationClosed==='true'&&!dataWidgetsRefreshing);
   await page.locator('.nav-link[href="#/network"]').click();await page.waitForFunction(()=>!!document.querySelector('.page-network'));
   await page.locator('.nav-link[href="#/"]').click();await page.waitForFunction(()=>!!document.getElementById('dw-dual-probe-sub')&&!dataWidgetsRefreshing);
   const changed=await f.refresh({main:11,sub:21,other:43});
   await page.evaluate(()=>showWidgetEditPanel('dual-probe'));await page.locator('#edit-expression2').fill('');
   await page.locator('#widget-edit-modal button[onclick^="saveWidgetEdit"]').click();
   await page.waitForFunction(()=>document.getElementById('widget-edit-modal')?.dataset.configurationClosed==='true'&&!dataWidgetsRefreshing);
   const removed=await f.refresh({main:12,sub:22,other:44});
   console.log(JSON.stringify({case:'F47 secondary edit/remove',language,changed,removed}));
   assert.deepEqual({changed,removed},{changed:['11.0','43.0'],removed:['12.0','-']});assert.deepEqual(f.errors,[]);
  }finally{await f.close();}
 });
 for(const width of [390,1440])test(`F47 normal: other components and dual layout ${language} ${width}`,async()=>{
  const extra=['number','temp','ring','status','log'].map(type=>({id:`probe-${type}`,type,label:type,decimals:1,expression:'${main}',layout:type==='log'?'full':'auto',min:0,max:100}));
  const f=await dualPage(language,{width,extra}),{page}=f;try{
   await f.refresh({main:25,sub:50});await f.refresh({main:25,sub:50});
   const actual=await page.evaluate(()=>({number:document.getElementById('dw-probe-number-value').textContent,temp:document.getElementById('dw-probe-temp-value').textContent,
    ring:document.getElementById('dw-probe-ring-value').textContent,status:document.getElementById('dw-probe-status-value').textContent,
    expectedStatus:t('dataWidget.statusNormal'),log:document.getElementById('dw-probe-log-log').textContent.trim(),expectedLog:t('dataWidget.clickToRead'),
    cards:[...document.querySelectorAll('#data-widgets-grid .dw-card')].map(e=>{const r=e.getBoundingClientRect();return {type:e.dataset.type,width:r.width,right:r.right};})}));
   assert.equal(actual.number,'25.0');assert.equal(actual.temp,'25');assert.equal(actual.ring,'25.0%');assert.equal(actual.status,actual.expectedStatus);assert.equal(actual.log,actual.expectedLog);
   await page.locator('#dw-probe-log-collapse').click();
   await page.evaluate(()=>refreshLogOnce('probe-log'));assert.match(await page.locator('#dw-probe-log-log').innerText(),/line one/);
   assert.equal(actual.cards.length,6);for(const card of actual.cards)assert(card.width>0&&card.right<=width,JSON.stringify(card));
   assert.deepEqual(f.errors,[]);
  }finally{await f.close();}
 });
}

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
