const {test}=require('node:test');
const assert=require('node:assert/strict');
const {harness}=require('./harness.cjs');
test('a service display rejects prior SSH output and completion',async()=>{
 const h=harness();await h.ready();h.load();h.el('exec-result');h.el('cancel-exec-btn');h.el('match-result-panel');
 h.run(`selectedHostId='host';currentExecSessionId=7;api.call=async()=>({code:0,data:{operation_id:41}});updateServiceStatusInList=async()=>{};`);
 await h.run(`executeManagedService({id:'model',name:'Model'},document.getElementById('exec-result'))`);
 const before=h.el('exec-result').textContent;
 h.run(`handleSshExecMessage({type:'ssh_exec_output',session_id:7,data:'OLD_COMMAND_OUTPUT'});`);
 assert.equal(h.el('exec-result').textContent,before);
 h.run(`handleSshExecMessage({type:'ssh_exec_error',session_id:7,error:'OLD_COMMAND_FAILURE'});`);
 assert.equal(h.el('exec-result').textContent,before);
});
async function commandPage(){
 const h=harness();await h.ready();h.load();
 for(const id of ['exec-result','exec-result-section','cancel-exec-btn','nohup-actions','match-result-panel','match-status-badge','match-final-status','nohup-tail-log','nohup-stop-tail'])h.el(id);
 h.run(`selectedHostId='host';sshCommands={host:[{id:'plain',name:'Plain',command:'printf plain'},{id:'model',name:'Model',nohup:true,serviceMode:true}]};window._cmdHostsList=[{id:'host',host:'fixture',username:'test',port:22}];updateServiceStatusInList=async()=>{};`);
 return h;
}
test('fast SSH messages are replayed only for the acknowledged session',async()=>{
 const h=await commandPage();let release;
 h.run('api').call=()=>new Promise(r=>release=r);
 const pending=h.run('executeCommand(0)');
 h.run(`handleSshExecMessage({type:'ssh_exec_start',session_id:8});handleSshExecMessage({type:'ssh_exec_output',session_id:8,data:'FOREIGN'});handleSshExecMessage({type:'ssh_exec_output',session_id:17,data:'CURRENT'});handleSshExecMessage({type:'ssh_exec_done',session_id:17,status:'success',exit_code:0});`);
 release({code:0,data:{session_id:17}});await pending;
 assert(!h.el('exec-result').textContent.includes('FOREIGN'));
 assert(h.el('exec-result').textContent.includes('CURRENT'));
 assert(h.el('exec-result').textContent.includes(h.ctx.t('promptRepair.sshSuccess')));
 assert.equal(h.run('currentExecSessionId'),null);
});
test('late SSH admission cannot replace a newer service display',async()=>{
 const h=await commandPage();let release;
 h.run('api').call=async endpoint=>endpoint==='ssh.exec_stream'?new Promise(r=>release=r):{code:0,data:{operation_id:31}};
 const old=h.run('executeCommand(0)');await h.run('executeCommand(1)');
 const text=h.el('exec-result').textContent;
 release({code:0,data:{session_id:17}});await old;
 h.run(`handleSshExecMessage({type:'ssh_exec_done',session_id:17,status:'failed',exit_code:2});`);
 assert.equal(h.el('exec-result').textContent,text);
 assert.equal(h.run('currentExecSessionId'),null);
});
test('late log response and tail timer cannot survive display replacement',async()=>{
 const h=await commandPage();let release;
 h.run(`currentNohupInfo={hostId:'host',logFile:'/tmp/model.log'};`);
 h.run('api').call=async endpoint=>endpoint==='ssh.exec'?new Promise(r=>release=r):{code:0,data:{operation_id:31}};
 const log=h.run('nohupTailLog()');
 await h.run('executeCommand(1)');const text=h.el('exec-result').textContent;
 release({code:0,data:{stdout:'OLD_LOG'}});await log;
 assert.equal(h.el('exec-result').textContent,text);assert.equal(h.run('tailIntervalId'),null);
});
test('page replacement and clear do not let old messages adopt the new panel',async()=>{
 const h=await commandPage();h.run('api').call=async()=>({code:0,data:{session_id:17}});await h.run('executeCommand(0)');
 h.el('exec-result').remove();const replacement=h.el('exec-result');replacement.textContent='new page';
 h.run(`handleSshExecMessage({type:'ssh_exec_output',session_id:17,data:'OLD'});`);
 assert.equal(replacement.textContent,'new page');
 h.run('clearExecResult()');h.run(`handleSshExecMessage({type:'ssh_exec_start',session_id:17});handleSshExecMessage({type:'ssh_exec_output',session_id:17,data:'OLD'});`);
 assert.equal(replacement.textContent,'');
});
test('expired and replaced service operations never trigger a retry',async()=>{
 const h=await commandPage();
 for(const data of [{operation_id:31,operation_phase:'expired'},{operation_id:32,operation_phase:'succeeded'}]){
  const calls=[];h.run('api').call=async(name,args,method)=>{calls.push({name,args,method});return {code:0,data:method==='POST'?{operation_id:31}:data};};
  await assert.rejects(h.run(`requestServiceControl('model',false,()=>true)`));
  assert.equal(calls.filter(c=>c.method==='POST').length,1);
  assert.equal(calls.filter(c=>c.method!=='POST').length,1);
 }
});
test('clearing during explicit verification prevents display updates',async()=>{
 const h=await commandPage();let release;
 h.run(`claimExecDisplay('service',document.getElementById('exec-result'));currentNohupInfo={commandId:'model',hostId:'host'};`);
 h.run('api').call=async(name,args,method)=>method==='POST'?{code:0,data:{operation_id:31}}:new Promise(r=>release=r);
 const pending=h.run('nohupCheckProcess()');while(!release)await Promise.resolve();
 h.run('clearExecResult()');release({code:0,data:{operation_id:31,operation_phase:'succeeded',state:'ready'}});await pending;
 assert.equal(h.el('exec-result').textContent,'');
});
