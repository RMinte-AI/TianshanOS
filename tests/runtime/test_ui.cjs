const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync('components/ts_webui/web/js/app.js','utf8');
function fn(name){const m=new RegExp('^(?:async )?function '+name+'\\(','m').exec(source);assert(m,name);const text=source.slice(m.index),end=text.indexOf('\n}')+2;assert(end>1);return text.slice(0,end);}
const languages={};for(const lang of ['zh-CN','en-US'])vm.runInNewContext(fs.readFileSync(`components/ts_webui/web/js/lang/${lang}.js`,'utf8'),{i18n:{registerLanguage:(id,data)=>languages[id]=data}});
let locale='zh-CN',writes=[],reject=false,response={code:0,data:{mirror_synced:true}},conditions=[],actions=[];
const nodes=new Map();function el(id){if(!nodes.has(id))nodes.set(id,{value:'',checked:false,disabled:false,dataset:{},style:{},classList:{add(){},remove(){}}});return nodes.get(id);}
const toast=[],failures=[];
const lookup=key=>key.split('.').reduce((v,k)=>v?.[k],languages[locale]);
const slots=v=>[...v.matchAll(/(?<!\$)\{(\w+)\}/g)].map(m=>m[1]);
const complete=(key,params)=>{const v=lookup(key);return typeof v==='string'&&!slots(v).some(p=>!params||params[p]===undefined||params[p]===null);};
const translate=(key,params={})=>{const v=lookup(key);return typeof v==='string'?v.replace(/(?<!\$)\{(\w+)\}/g,(m,p)=>String(params[p])):key;};
const ctx=vm.createContext({console,structuredClone,TextEncoder,Date,Proxy,fieldError:(id,message)=>{throw Error(message)},setTimeout(){},document:{getElementById:el,querySelectorAll:q=>q.includes('condition-row')?conditions:q.includes('action-row')?actions:[],hidden:false},window:{},t:translate,i18n:{complete},showToast:(m,k)=>toast.push(m),showFailureToast:(message,details,options)=>failures.push({message,details,...options}),alert:m=>{throw Error(m)},closeModal(){},refreshRules:async()=>{},refreshAutomationStatus:async()=>{},api:{call:async(method,params)=>{if(method.endsWith('.get'))return {code:0,data:{revision:8}};writes.push({method,params});if(reject)throw Object.assign(Error('synthetic lost reply'),{kind:'network',uncertain:true,rawMessage:''});return response;}}});
vm.runInContext(source.match(/^const CONDITION_OPERATORS = .*;$/m)[0],ctx);
for(const name of ['conditionEditorFields','readConditionEditor','runtimeText','reportFailure','ruleWriteWithRevision','submitAddRule','toggleManualOnly','quickActionRefreshLog','checkRuleHasNohupSsh'])vm.runInContext(fn(name),ctx);
// the failure text builder lives in api.js
const apiSource=fs.readFileSync('components/ts_webui/web/js/api.js','utf8');
function apiFn(name){const m=new RegExp('^function '+name+'\\(','m').exec(apiSource);assert(m,name);const text=apiSource.slice(m.index);return text.slice(0,text.indexOf('\n}')+2);}
vm.runInContext(apiFn('apiErrorReason'),ctx);
vm.runInContext(apiSource.slice(apiSource.indexOf('const FAILURE_LEGACY_TEXT'),apiSource.indexOf('function describeFailure')),ctx);
vm.runInContext(apiFn('describeFailure'),ctx);
function row(fields,extra={}){return {...extra,querySelector:s=>fields[s]||null};}
(async()=>{
 for(locale of ['zh-CN','en-US']){
  el('rule-show-dashboard').dataset={};el('rule-allow-manual').dataset={};
  for(const key of ['saved','mirrorWarning','saveUnknown','startBlocked','unknown','revision_conflict','source_read_only'])assert.notEqual(ctx.runtimeText(key),key);
  el('rule-id').value='model';el('rule-name').value='Model';el('rule-logic').value='and';el('rule-cooldown').value='3000';el('rule-revision').value='7';el('rule-manual-only').checked=true;el('rule-enabled').checked=true;el('rule-show-dashboard').checked=false;el('rule-allow-manual').checked=false;
  conditions=[row({'.cond-variable':{value:'upstream.status'},'.cond-operator':{value:'eq'},'.cond-value':{value:'"123"'}},{_originalCondition:{value:'123',value_type:4}})];
  actions=[row({'.action-template-id':{value:'template'},'.action-delay':{value:'12'},'.action-repeat-mode':{value:'count'},'.action-repeat-count':{value:'3'},'.action-repeat-interval':{value:'0'},'.action-has-condition':{checked:false}},{dataset:{},_originalAction:{template_id:'template',type:'cli',command:'synthetic --arg',async:true}})];
  writes=[];await ctx.submitAddRule('model');assert.equal(writes.length,1);const req=writes[0];assert.equal(req.method,'automation.rules.update');assert.equal(req.params.expected_revision,7);assert.equal(req.params.conditions[0].value,'123');assert.equal(req.params.conditions[0].value_type,4);assert.equal(req.params.actions[0].command,'synthetic --arg');assert.equal(req.params.actions[0].async,true);assert.equal(req.params.actions[0].repeat_interval_ms,0);assert.equal(req.params.show_on_dashboard,false);
  assert.equal(toast.at(-1),locale==='zh-CN'?'已保存规则“Model”':'Rule “Model” saved');
  response={code:1,message:'revision_conflict'};writes=[];failures.length=0;await ctx.submitAddRule('model');assert.equal(writes.length,1);assert.equal(el('rule-name').value,'Model');
  assert.equal(failures.length,1);assert.equal(failures[0].type,'error');   // a refusal is an error; an unconfirmed result below is a warning
  assert.equal(failures[0].message,locale==='zh-CN'?'保存规则“Model”失败：设备上的这份配置刚被其他操作修改过。请刷新后确认最新内容，再重新操作。':'Failed to save rule “Model”: the configuration on the device was just changed by another operation. Refresh, check the latest content, then try again.');
  assert(failures[0].details.includes('revision_conflict')&&failures[0].details.includes('automation.rules.update'));
  reject=true;writes=[];failures.length=0;await ctx.submitAddRule('model');assert.equal(writes.length,1);
  assert.equal(failures.length,1);assert.equal(failures[0].type,'warning');
  assert.equal(failures[0].message,locale==='zh-CN'?'保存规则“Model”的结果未确认。请先刷新核对当前状态，不要重复提交。':'The result of saving rule “Model” is unconfirmed. Refresh and check the current state before repeating the operation.');assert.equal(el('rule-revision').value,'7');assert.equal(el('rule-revision').dataset.observedRevision,8);assert.equal(el('add-rule-modal').dataset.saving,'false');reject=false;response={code:0,data:{mirror_synced:true}};
  el('rule-show-dashboard').dataset.present='false';writes=[];await ctx.submitAddRule('model');assert(!Object.hasOwn(writes[0].params,'show_on_dashboard'));el('rule-show-dashboard').dataset.dirty='true';writes=[];await ctx.submitAddRule('model');assert.equal(writes[0].params.show_on_dashboard,false);
 }
 const log=el('quick-log-content');log.isConnected=true;log.dataset={};ctx.window._sshHostsData={host:{host:'192.0.2.8',username:'test'}};vm.runInContext("let quickActionLastContent=''; let sshCommands={host:[{id:'new',name:'model',command:'true',nohup:true,serviceMode:true}]};",ctx);
 let resolve,reads=0;ctx.api.call=()=>{reads++;return new Promise(r=>resolve=r)};const pending=ctx.quickActionRefreshLog('/tmp/test.log','host');await ctx.quickActionRefreshLog('/tmp/test.log','host');assert.equal(reads,1);log.isConnected=false;resolve({code:0,data:{stdout:'obsolete'}});await pending;assert.notEqual(log.textContent,'obsolete');assert.equal(log.dataset.loading,'false');
 ctx.api.call=async method=>method==='automation.rules.get'?{code:0,data:{actions:[{type:'ssh_cmd_ref',cmd_id:'old',template_id:'template'}]}}:{code:0,data:{type:'ssh_cmd_ref',ssh_ref:{cmd_id:'new'}}};assert.equal((await ctx.checkRuleHasNohupSsh({id:'rule',actions_count:1})).commandId,'new');ctx.api.call=async method=>method==='automation.rules.get'?{code:0,data:{actions:[{type:'ssh_cmd_ref',cmd_id:'missing'}]}}:{code:1};assert.equal((await ctx.checkRuleHasNohupSsh({id:'rule',actions_count:1})).unresolved,true);ctx.api.call=async()=>({code:1});assert.equal((await ctx.checkRuleHasNohupSsh({id:'rule',actions_count:1})).unresolved,true);
 const card={dataset:{}};const statusValue={};const statusContainer={classList:{add(){},remove(){}},isConnected:true,dataset:{command:'model'},closest:()=>card,querySelector:()=>statusValue};
 ctx.document.hidden=false;ctx.document.querySelectorAll=()=>[statusContainer];
 vm.runInContext(source.slice(source.indexOf('let serviceStatusInFlight = false;'),source.indexOf('// 触发快捷操作后的冷却时间')),ctx);
 let statusReads=0;ctx.api.call=async(method,params)=>{assert.equal(method,'automation.services.status');assert.equal(params.verify,undefined);statusReads++;return {code:0,data:{state:'unknown',source:'none'}};};
 for(let i=0;i<3;i++)await ctx.updateQuickActionServiceStatus();assert.equal(statusReads,3);
 console.log('PASS actual UI functions: bilingual states, one revisioned update, manual conditions/types, inline fields/repeat preservation, explicit false vs omitted, conflict/draft/uncertain write');
})().catch(e=>{console.error(e);process.exitCode=1});
