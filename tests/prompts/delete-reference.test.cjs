const {test}=require('node:test');
const assert=require('node:assert/strict');
const {harness}=require('./harness.cjs');
for(const language of ['zh-CN','en-US'])test(`${language}: H15 nine states, busy and operation kind select an approved explanation`,async()=>{
 const h=harness(language);await h.ready();h.load();
 const states={unknown:'unconfirmed',running:'running',ready:'running',checking:'running',starting:'starting',stopping:'stopping',stopped:null,failed:'unconfirmed',timeout:'unconfirmed'};
 for(const [state,expected] of Object.entries(states))for(const busy of [false,true])for(const kind of ['start','stop','verify','other'])for(const phase of ['succeeded','queued','executing']){
  const pending=busy||phase!=='succeeded';const operation={start:'starting',stop:'stopping',verify:'verifying'};
  const want=pending?(operation[kind]||'operationPending'):expected;
  const got=h.ctx.deleteServiceMessage({state,busy,operation_kind:kind,operation_phase:phase});
  assert.equal(got,want,JSON.stringify({state,busy,kind,phase}));
  if(got){const text=h.ctx.t('deleteProtection.'+got,{command:'Model'});assert(text&&!text.includes('deleteProtection.'));}
 }
 assert.equal(h.ctx.deleteServiceMessage({state:'future',busy:false}),'unconfirmed');
 assert.equal(h.ctx.deleteServiceMessage({busy:false}),'unconfirmed');
});
for(const language of ['zh-CN','en-US'])test(`${language}: approved keys, placeholders and missing-template display label`,async()=>{
 const h=harness(language);await h.ready();h.load();
 const missing=h.ctx.runtimeSaveError?h.ctx.runtimeSaveError({data:{error_code:'action_missing',missing_template_id:'missing'},missingTemplateName:'Human label'}):h.run("runtimeSaveError({data:{error_code:'action_missing',missing_template_id:'missing'},missingTemplateName:'Human label'})");
 assert(missing.includes('Human label')&&!missing.includes('action_missing'));
 assert.equal(h.ctx.runtimeSaveError({rawMessage:'action_missing'}),h.ctx.runtimeText('saveFailed'));
 assert.equal(h.ctx.runtimeSaveError({data:{error_code:'template_lookup_failed'}}),h.ctx.runtimeText('saveFailed'));
 let sheet;h.ctx.confirmSheet=async options=>{sheet=options;return false;};
 await h.ctx.showDeleteProtection({code:4,message:'action_in_use'}, {type:'action',name:'Model'},()=>true);
 assert(sheet.bodyHtml.includes(h.ctx.escapeHtml(h.ctx.t('deleteProtection.actionInUseWithoutDetails',{name:'Model'}))));
 await h.ctx.showDeleteProtection({code:4,message:'action_reference_check_unavailable',data:{check_reason:'loading'}}, {type:'action',name:'Model'},()=>true);
 assert.equal(sheet.secondary,false);assert.equal(sheet.primary,h.ctx.t('common.close'));
});
