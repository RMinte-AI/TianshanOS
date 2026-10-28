const {fixture,version}=require('./configuration-fixture.cjs');
const OLD='111111111111111111111111',NEW='222222222222222222222222',START='333333333333333333333333';
function serviceFixture() {
 const base=fixture(), state={runs:new Map(),failVerify:false,starts:[],controls:[],logs:[]};
 const run=(id,host,name)=>({run_id:id,command_id:'model',name,rule_id:'original-rule',config_version:version(1),host_id:'same-host',host,port:22,username:'original-user',keyid:'original-key',log_file:`/tmp/ts_nohup_r_${id}.log`,state:'unknown',last_known:'',confirmed_ms:0,operation_id:0,operation_phase:'',operation_kind:'',operation_reason:''});
 state.runs.set(OLD,run(OLD,'192.0.2.1','Original “model” <&>'));
 state.runs.set(NEW,{...run(NEW,'192.0.2.2','Recreated model'),username:'new-user',keyid:'new-key'});
 const commands=[{id:'plain',name:'Ordinary background',host_id:'host-one',command:'printf ordinary',nohup:true,serviceMode:false},
  {id:'managed',name:'Ready-pattern background',host_id:'host-one',command:'printf ready',nohup:true,serviceMode:true,varName:'model',readyPattern:'ready'},
  {id:'foreground',name:'Foreground',host_id:'host-one',command:'printf direct',nohup:false,serviceMode:false}];
 const ok=data=>({code:0,data});
 const respond=call=>{
  const {endpoint:e,params:p}=call;
  if(e==='automation/services/list') return ok({items:[...state.runs.values()].map(x=>({...x}))});
  if(e==='ssh/hosts/list') return ok({...base.status(),hosts:[{id:'host-one',host:'192.0.2.10',port:22,username:'original-user',keyid:'original-key'},{id:'host-two',host:'192.0.2.99',port:22,username:'new-user',keyid:'new-key'}]});
  if(e==='ssh/commands/list')return ok({...base.status(),commands:commands.filter(c=>!p.host_id||c.host_id===p.host_id)});
  if(e==='ssh/services/start') {
   state.starts.push(p);if(p.active_version!==version(base.state.revision))return {code:4,message:'configuration_changed'};
   const item={...run(START,'192.0.2.10',commands.find(c=>c.id===p.command_id).name),command_id:p.command_id,operation_id:1,operation_kind:'start',operation_phase:'queued',rule_id:''};
   state.runs.set(START,item);return ok({run_id:START,operation_id:1});
  }
  if(e==='automation/services/status'||e==='automation/services/stop'){
   const item=state.runs.get(p.run_id);if(!item)return {code:2,message:'service_record_not_found'};
   if(p.verify||e.endsWith('/stop')){state.controls.push({endpoint:e,...p});++item.operation_id;item.operation_kind=p.verify?'verify':'stop';item.operation_phase='queued';return ok({run_id:item.run_id,operation_id:item.operation_id});}
   if(item.operation_phase==='queued'){
    item.operation_phase=state.failVerify&&item.operation_kind==='verify'?'failed':'succeeded';
    item.operation_reason=item.operation_phase==='failed'?'ssh_auth_failed':'';
    item.state=item.operation_phase==='failed'?'unknown':item.operation_kind==='stop'?'stopped':'running';item.confirmed_ms=item.state==='unknown'?0:4200;
   }
   return ok({...item});
  }
  if(e==='ssh/exec'){state.logs.push(p);return ok({exit_code:0,stdout:'frozen-target log\n'});}
  if(e==='ssh/exec_stream')return ok({session_id:71});
  return base.respond(call);
 };
 return {...base,configuration:base.state,state,respond,commands};
}
module.exports={serviceFixture,OLD,NEW,START};
