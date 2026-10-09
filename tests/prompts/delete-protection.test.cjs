const {test}=require('node:test');
const assert=require('node:assert/strict');
const {harness}=require('./harness.cjs');
for(const lang of ['zh-CN','en-US'])test(`${lang}: protected delete explains service state rather than internal failure`,async()=>{
 const h=harness(lang);await h.ready();h.load();
 h.ctx.fetch=async()=>({ok:true,status:200,text:async()=>JSON.stringify({code:4,error:'service_delete_protected'})});
 for(const endpoint of ['/automation/actions/delete','/ssh/commands/remove']){
  const result=await h.run(`api.request('${endpoint}','POST',{id:'fixture'})`);
  assert.equal(result.code,4);assert.equal(result.rawMessage,'service_delete_protected');
  assert.equal(result.message,h.ctx.t('promptRepair.serviceDeleteProtected'));
  assert(!result.message.includes('service_delete_protected'));
  assert.throws(()=>h.ctx.requireApiSuccess(result,endpoint),e=>e.code===4&&e.rawMessage==='service_delete_protected');
 }
});
async function deleteFixture(){
 const h=harness();await h.ready();h.load();h.el('page-content');
 h.ctx.confirmSheet=async()=>true;
 h.run(`refreshActions=async()=>{};refreshCommandsList=()=>{};selectedHostId='host';sshCommands={host:[{id:'model',name:'Model',nohup:true,serviceMode:true}]};`);
 return h;
}
function responses(h,{state='stopped',phase='succeeded',service=true,template=true}={}){
 const calls=[];
 h.run('api').call=async(name,args,method)=>{
  calls.push({name,args,method});
  if(name==='automation.actions.get')return {code:0,data:template?{type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model'}}:{type:'device'}};
  if(name==='ssh.commands.get')return {code:0,data:{id:'model',nohup:service,serviceMode:service}};
  if(name==='automation.services.status')return {code:0,data:method==='POST'?{operation_id:31}:{operation_id:31,operation_phase:phase,state,busy:false}};
  return {code:0};
 };
 return calls;
}
for(const entry of ['action','command']){
 const invoke=h=>h.run(entry==='action'?"deleteAction('template')":"deleteCommand(0)");
 test(`${entry}: unregistered service is verified stopped before deletion`,async()=>{
  const h=await deleteFixture(),calls=responses(h);await invoke(h);
  const names=calls.map(c=>c.name);
  const deletion=entry==='action'?'automation.actions.delete':'ssh.commands.remove';
  assert(names.indexOf('automation.services.status')<names.indexOf(deletion));
  assert.equal(calls.filter(c=>c.name===deletion).length,1);
  assert.deepEqual(calls.filter(c=>c.name==='automation.services.status').map(c=>c.method),['POST',undefined]);
  assert.equal(calls.find(c=>c.method==='POST').args.verify,true);
  assert(!calls.some(c=>c.name.includes('.start')||c.name.includes('.stop')));
 });
 for(const state of ['running','ready','unknown'])test(`${entry}: ${state} never leads to delete or stop`,async()=>{
  const h=await deleteFixture(),calls=responses(h,{state});await invoke(h);
  assert(!calls.some(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')||c.name.endsWith('.stop')));
  assert(h.el('toast').textContent.includes(h.ctx.t('promptRepair.serviceDeleteProtected')));
 });
 test(`${entry}: failed verification never deletes`,async()=>{
  const h=await deleteFixture(),calls=responses(h,{phase:'failed'});await invoke(h);
  assert(!calls.some(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')));
 });
 test(`${entry}: ordinary command has no service verification`,async()=>{
  const h=await deleteFixture(),calls=responses(h,{service:false});await invoke(h);
  assert(!calls.some(c=>c.name==='automation.services.status'));
  assert(calls.some(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')));
 });
 test(`${entry}: repeated click submits only one verification and delete`,async()=>{
  const h=await deleteFixture(),calls=responses(h),original=h.run('api').call;
  let release,entered;const gate=new Promise(r=>entered=r);
  h.run('api').call=async(name,args,method)=>{
   if(name==='automation.services.status'&&method==='POST'){entered();await new Promise(r=>release=r);}
   return original(name,args,method);
  };
  const first=invoke(h);await gate;await invoke(h);release();await first;
  assert.equal(calls.filter(c=>c.method==='POST').length,1);
  assert.equal(calls.filter(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')).length,1);
 });
 test(`${entry}: leaving the page during verification cancels subsequent deletion`,async()=>{
  const h=await deleteFixture(),calls=responses(h),original=h.run('api').call;
  let release,entered;const gate=new Promise(r=>entered=r);
  h.run('api').call=async(name,args,method)=>{
   if(name==='automation.services.status'&&method!=='POST'){entered();await new Promise(r=>release=r);}
   return original(name,args,method);
  };
  const first=invoke(h);await gate;h.el('page-content').remove();h.el('page-content');release();await first;
  assert(!calls.some(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')));
 });
}
test('switching hosts during verification removes the original command only',async()=>{
 const h=await deleteFixture(),calls=responses(h),original=h.run('api').call;
 let release,entered;const gate=new Promise(r=>entered=r);
 h.run('api').call=async(name,args,method)=>{
  if(name==='automation.services.status'&&method==='POST'){entered();await new Promise(r=>release=r);}
  return original(name,args,method);
 };
 const first=h.run('deleteCommand(0)');await gate;
 h.run(`sshCommands.other=[{id:'other',name:'Other'}];selectedHostId='other';`);
 release();await first;
 assert.equal(h.run('sshCommands.host.length'),0);assert.equal(h.run('sshCommands.other[0].id'),'other');
});
for(const entry of ['action','command'])for(const newer of ['start','stop','verify'])test(`${entry}: newer ${newer} prevents stale deletion and status painting`,async()=>{
 const h=await deleteFixture();h.el('exec-result');h.el('cancel-exec-btn');h.el('match-result-panel');
 h.run(`window.painted=[];renderServiceCommand=(id,data)=>painted.push(data.state);updateServiceStatusInList=async()=>renderServiceCommand('model',{state:'starting'});updateQuickActionServiceStatus=async()=>{};`);
 const calls=[];let release,entered,reads=0,verifications=0;const gate=new Promise(r=>entered=r);
 h.run('api').call=async(name,args,method)=>{
  calls.push({name,method});
  if(name==='automation.actions.get')return {code:0,data:{type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model'}}};
  if(name==='ssh.commands.get')return {code:0,data:{nohup:true,serviceMode:true}};
  if(name==='ssh.services.start'||name==='automation.services.stop')return {code:0,data:{operation_id:32}};
  if(name==='automation.services.status'&&method==='POST')return {code:0,data:{operation_id:++verifications===1?31:32}};
  if(name==='automation.services.status'){
   if(++reads===1){entered();await new Promise(r=>release=r);return {code:0,data:{operation_id:31,operation_phase:'succeeded',state:'stopped',busy:false}};}
   return {code:0,data:{operation_id:32,operation_phase:'succeeded',state:newer==='stop'?'stopped':'ready',busy:false}};
  }
  if(name.endsWith('.delete')||name.endsWith('.remove'))return {code:4,error:'service_delete_protected'};
  throw new Error(name);
 };
 const deletion=h.run(entry==='action'?"deleteAction('template')":"deleteCommand(0)");await gate;
 await h.run(newer==='start'?"executeManagedService(sshCommands.host[0],document.getElementById('exec-result'))":newer==='stop'?"quickActionStopProcess('model',false)":"verifyServiceState('model')");
 const painted=Array.from(h.run('painted')),toast=h.el('toast').textContent;
 release();await deletion;
 assert.deepEqual(Array.from(h.run('painted')),painted);
 assert.equal(h.el('toast').textContent,toast);
 assert(!calls.some(c=>c.name.endsWith('.delete')||c.name.endsWith('.remove')));
 assert.equal(h.run('sshCommands.host.length'),1);
});
for(const entry of ['action','command'])test(`${entry}: stale admission failure cannot replace newer start feedback`,async()=>{
 const h=await deleteFixture();h.el('exec-result');h.el('cancel-exec-btn');h.el('match-result-panel');
 h.run(`window.painted=[];renderServiceCommand=(id,data)=>painted.push(data.state);updateServiceStatusInList=async()=>renderServiceCommand('model',{state:'starting'});`);
 let release,entered;const gate=new Promise(r=>entered=r);
 h.run('api').call=async(name,args,method)=>{
  if(name==='automation.actions.get')return {code:0,data:{type:'ssh_cmd_ref',ssh_ref:{cmd_id:'model'}}};
  if(name==='ssh.commands.get')return {code:0,data:{nohup:true,serviceMode:true}};
  if(name==='automation.services.status'&&method==='POST'){entered();await new Promise(r=>release=r);return {code:4,error:'service_delete_protected'};}
  if(name==='ssh.services.start')return {code:0,data:{operation_id:32}};
  throw new Error('unexpected request '+name);
 };
 const deletion=h.run(entry==='action'?"deleteAction('template')":"deleteCommand(0)");await gate;
 await h.run("executeManagedService(sshCommands.host[0],document.getElementById('exec-result'))");
 const toast=h.el('toast').textContent;release();await deletion;
 assert.deepEqual(Array.from(h.run('painted')),['starting']);assert.equal(h.el('toast').textContent,toast);
});
