const {test}=require('node:test'),assert=require('node:assert/strict');
const kit=require('./pagekit.cjs');
kit.install();

for(const spec of [
 {type:'rest',fn:'testRestConnection',input:'source-rest-url',endpoint:'automation/proxy/fetch',data:v=>({body:JSON.stringify({[v]:1}),status:200})},
 {type:'ws',fn:'testWsConnection',input:'source-ws-uri',endpoint:'automation/proxy/websocket_test',data:v=>({message:JSON.stringify({[v]:1})})},
 {type:'sio',fn:'testSioConnection',input:'source-sio-url',endpoint:'automation/proxy/socketio_test',data:v=>({event:v,data:JSON.stringify({[v]:1})})}
]) {
 test(`${spec.type}: a result from a closed source editor cannot affect its replacement`,async()=>{
  await kit.withPage('en-US',()=>({code:0,data:{}}),async({page})=>{
   const routes=[];let notify;
   const arrival=()=>new Promise(resolve=>notify=resolve);
   await page.route('**/api/v1/'+spec.endpoint,route=>{routes.push(route);notify?.();});
   let next=arrival();
   await page.evaluate(s=>{window.sourceDone=[];showAddSourceModal();document.getElementById(s.input).value='http://old.invalid';void window[s.fn]().finally(()=>sourceDone.push('old'));},{fn:spec.fn,input:spec.input});await next;
   next=arrival();
   await page.evaluate(s=>{closeModal('add-source-modal');showAddSourceModal();document.getElementById(s.input).value='http://new.invalid';void window[s.fn]().finally(()=>sourceDone.push('new'));},{fn:spec.fn,input:spec.input});await next;
   await routes[1].fulfill({json:{code:0,data:spec.data('newField')}});
   await page.waitForFunction(()=>sourceDone.includes('new'));
   await routes[0].fulfill({json:{code:0,data:spec.data('oldField')}});
   await page.waitForFunction(()=>sourceDone.includes('old'));
   assert.match(await page.locator(`#${spec.type}-var-selector`).innerText(),/newField/);
   assert.doesNotMatch(await page.locator(`#${spec.type}-var-selector`).innerText(),/oldField/);
   if(spec.type==='sio')assert.equal(await page.locator('#source-sio-event').inputValue(),'newField');
  });
 });
 test(`${spec.type}: a superseded request cannot change fields or re-enable the new test`,async()=>{
  await kit.withPage('en-US',()=>({code:0,data:{}}),async({page})=>{
   const routes=[];let notify;
   const arrival=()=>new Promise(resolve=>notify=resolve);
   await page.route('**/api/v1/'+spec.endpoint,route=>{routes.push(route);notify?.();});
   let next=arrival();
   await page.evaluate(s=>{window.sourceDone=[];showAddSourceModal();document.getElementById(s.input).value='http://one.invalid';void window[s.fn]().finally(()=>sourceDone.push('first'));},{fn:spec.fn,input:spec.input});await next;
   next=arrival();
   await page.evaluate(s=>{document.getElementById(s.input).value='http://two.invalid';void window[s.fn]().finally(()=>sourceDone.push('second'));},{fn:spec.fn,input:spec.input});await next;
   await routes[0].fulfill({json:{code:6,message:'old error'}});
   await page.waitForFunction(()=>sourceDone.includes('first'));
   assert.equal(await page.locator(`#btn-test-${spec.type}`).isDisabled(),true);
   assert.doesNotMatch(await page.locator(`#${spec.type}-test-result`).innerText(),/old error/);
   await routes[1].fulfill({json:{code:0,data:spec.data('latestField')}});
   await page.waitForFunction(()=>sourceDone.includes('second'));
   assert.equal(await page.locator(`#btn-test-${spec.type}`).isDisabled(),false);
   assert.match(await page.locator(`#${spec.type}-var-selector`).innerText(),/latestField/);
  });
 });
}
