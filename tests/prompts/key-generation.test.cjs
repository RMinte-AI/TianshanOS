const {test}=require('node:test');
const assert=require('node:assert/strict');
const {harness}=require('./harness.cjs');
async function setup(lang){
 const h=harness(lang);await h.ready();h.load();
 for(const id of ['keygen-modal','keygen-id','keygen-type','keygen-comment','keygen-alias','keygen-exportable','keygen-hidden','keygen-status','keygen-submit','keygen-close','keygen-ec-warn'])h.el(id);
 h.ctx.showGenerateKeyModal();h.el('keygen-id').value='fixture';h.el('keygen-type').value='rsa2048';
 h.run('refreshSecurityPage=async()=>({keysLoaded:true,keysCurrent:true});');return h;
}
const reply=(json,status=200)=>({ok:status<400,status,text:async()=>JSON.stringify(json)});
for(const lang of ['zh-CN','en-US']){
 for(const type of ['rsa2048','rsa4096','ec256','ec384'])test(`${lang} ${type}: delayed real handler gives persistent status, one POST, closes and refreshes`,async()=>{
  const h=await setup(lang);h.el('keygen-type').value=type;let release,requests=[];
  h.ctx.fetch=async(url,opts)=>{requests.push({url,params:JSON.parse(opts.body)});return new Promise(r=>release=r);};
  const pending=h.ctx.generateKey();await h.ctx.generateKey();assert.equal(requests.length,1);assert(h.el('keygen-submit').disabled);
  await h.advance(4000);assert(h.el('keygen-status').textContent.includes('fixture'));assert(!h.el('keygen-modal').classList.contains('hidden'));
  release(reply({code:0,data:{generated:true,id:'fixture',type:({ec256:'ecdsa-p256',ec384:'ecdsa-p384'})[type]||type}}));await pending;
  assert(h.el('keygen-modal').classList.contains('hidden'));assert(h.el('toast').classList.contains('toast-success'));assert(!h.el('keygen-submit').disabled);
  assert.match(requests[0].params.request_id,/^kg-/);const trace=h.run('api.keyGenerationTrace');assert(trace.events.some(x=>x.stage==='response_headers'));assert(trace.events.some(x=>x.stage==='modal_closed'));assert(trace.events.some(x=>x.stage==='list_done'));
  assert(!JSON.stringify(trace).includes('Authorization'));
 });
 test(`${lang}: business failure keeps inputs, stage and cleanup, distinguishes memory/storage`,async()=>{
  const h=await setup(lang);let posts=0;h.ctx.fetch=async()=>{posts++;return reply({code:7,error:'key_storage_full',data:{failed_stage:'metadata_write',esp_error:'ESP_ERR_NVS_NOT_ENOUGH_SPACE',cleanup_complete:false,cleanup_error:'ESP_FAIL'}});};
  await h.ctx.generateKey();assert.equal(posts,1);assert(!h.el('keygen-modal').classList.contains('hidden'));assert.equal(h.el('keygen-id').value,'fixture');assert(h.el('keygen-status').textContent.includes('metadata_write'));assert(h.el('keygen-status').textContent.includes('ESP_FAIL'));assert(!h.el('keygen-submit').disabled);
  h.ctx.fetch=async()=>reply({code:6,error:'Memory allocation failed'});h.run('toastDeadline=0;');await h.ctx.generateKey();assert(h.el('keygen-status').textContent.includes(h.ctx.t('keyGeneration.memory')));
 });
 for(const failure of ['timeout','network','http','body','json','string-code','missing','wrong-id','response-allocation','response-no-message'])test(`${lang}: ${failure} is unknown, one POST and at most one record check, no automatic retry`,async()=>{
  const h=await setup(lang);let posts=0,checks=0;
  h.ctx.fetch=async(url,opts)=>{
   if(opts.method==='GET'){checks++;return reply({code:0,data:{id:'fixture'}});}
   posts++;
   if(failure==='timeout')return new Promise((r,j)=>opts.signal.addEventListener('abort',()=>j(new Error('aborted'))));
   if(failure==='network')throw new Error('offline');
   if(failure==='http')return reply({code:7},500);
   if(failure==='body')return {ok:true,status:200,text:async()=>{throw new Error('cut');}};
   if(failure==='json')return {ok:true,status:200,text:async()=>'{invalid'};
   if(failure==='response-no-message')return reply({code:6});
   if(failure==='response-allocation')return reply({code:6,error:'key_response_failed'});
   return reply({code:failure==='string-code'?'0':0,data:failure==='missing'?{}:{generated:true,id:'other'}});
  };
  const pending=h.ctx.generateKey();if(failure==='timeout')await h.advance(30000);await pending;
  assert.equal(posts,1);assert.equal(checks,1);assert(!h.el('keygen-modal').classList.contains('hidden'));assert(h.el('keygen-status').textContent.includes(h.ctx.t('keyGeneration.recordFound')));assert(!h.el('toast').classList.contains('toast-success'));assert(!h.el('keygen-submit').disabled);
 });
 test(`${lang}: a failed record check remains unknown and refresh failure cannot become generation failure`,async()=>{
  const h=await setup(lang);h.ctx.fetch=async()=>{throw new Error('offline');};await h.ctx.generateKey();assert.equal(h.el('keygen-status').textContent,h.ctx.t('keyGeneration.unknown'));
  h.ctx.fetch=async()=>reply({code:0,data:{generated:true,id:'fixture'}});h.run('refreshSecurityPage=async()=>({keysLoaded:false,keysCurrent:true});toastDeadline=0;');await h.ctx.generateKey();assert(h.el('keygen-modal').classList.contains('hidden'));assert.equal(h.el('toast').textContent,h.ctx.t('keyGeneration.refreshFailed'));
 });
 test(`${lang}: close/reopen and navigation prevent stale modal updates; pending guard survives closure`,async()=>{
  const h=await setup(lang);let release,posts=0;h.ctx.fetch=async()=>{posts++;return new Promise(r=>release=r);};const pending=h.ctx.generateKey();h.ctx.hideGenerateKeyModal();h.ctx.showGenerateKeyModal();h.el('keygen-id').value='newinput';await h.ctx.generateKey();assert.equal(posts,1);
  release(reply({code:0,data:{generated:true,id:'fixture'}}));await pending;assert.equal(h.el('keygen-id').value,'newinput');assert(!h.el('keygen-modal').classList.contains('hidden'));assert(!h.el('keygen-submit').disabled);assert(!h.el('toast').classList.contains('toast-success'));
  h.ctx.router={navigation:{isCurrent:()=>true}};h.el('keygen-id').value='fixture';const pending2=h.ctx.generateKey();h.ctx.router.navigation={isCurrent:()=>true};release(reply({code:0,data:{generated:true,id:'fixture'}}));await pending2;assert(!h.el('keygen-modal').classList.contains('hidden'));
 });
 test(`${lang}: invalid IDs rejected before any POST`,async()=>{
  const h=await setup(lang);h.ctx.fetch=async()=>{assert.fail('must reject before fetch');};for(const id of ['abcdefghijk','x,y','中文中文']){h.el('keygen-id').value=id;await h.ctx.generateKey();assert.equal(h.el('keygen-status').textContent,h.ctx.t('keyGeneration.invalidId'));}
 });
 test(`${lang}: actual key refresh rejects business/format failures, keeps HTTPS and resists out-of-order responses`,async()=>{
  const h=harness(lang);await h.ready();h.load();h.el('keys-table-body').innerHTML='retained';
  h.ctx.fetch=async()=>reply({code:0,data:{}});h.run("api.certStatus=async()=>({code:0,data:{has_private_key:true,has_certificate:true,cert_info:{subject_cn:'fixture'}}});");
  for(const value of [{code:7,error:'failed'},{code:0,data:{keys:{}}}]){h.ctx.value=value;h.run('api.keyList=async()=>value;');const r=await h.ctx.refreshSecurityPage({keysOnly:true});assert.equal(r.keysLoaded,false);assert.equal(h.el('keys-table-body').innerHTML,'retained');}
  let resolveFirst,calls=0;h.ctx.apiMock=async()=>{if(++calls===1)return new Promise(r=>resolveFirst=r);return {code:0,data:{keys:[{id:'new',type:'rsa2048'}]}};};h.run('api.keyList=apiMock;');
  const first=h.ctx.refreshSecurityPage({keysOnly:true});const second=await h.ctx.refreshSecurityPage({keysOnly:true});resolveFirst({code:0,data:{keys:[{id:'old',type:'rsa2048'}]}});const stale=await first;
  assert(second.keysCurrent&&!stale.keysCurrent);assert(h.el('keys-table-body').innerHTML.includes('new'));assert(!h.el('keys-table-body').innerHTML.includes('>old<'));assert(h.el('keys-table-body').innerHTML.includes('HTTPS'));
 });
}
for(const lang of ['zh-CN','en-US'])test(`${lang}: rebuilt page controls are released without stale errors; explicit success replaces a prior warning`,async()=>{
 const h=await setup(lang);let release;h.ctx.fetch=async()=>new Promise(r=>release=r);const pending=h.ctx.generateKey();
 for(const id of ['keygen-modal','keygen-submit','keygen-status','keygen-close']){h.el(id).remove();h.el(id);}
 h.ctx.showGenerateKeyModal();h.el('keygen-id').value='newinput';assert(h.el('keygen-submit').disabled);
 release(reply({code:7,error:'key_storage_full'}));await pending;assert.equal(h.el('keygen-id').value,'newinput');assert(!h.el('keygen-submit').disabled);assert.equal(h.el('keygen-status').textContent,'');assert(!h.el('toast').classList.contains('toast-error'));
 h.ctx.showToast('previous warning','error',10000);h.el('keygen-id').value='fixture';h.ctx.fetch=async()=>reply({code:0,data:{generated:true,id:'fixture'}});await h.ctx.generateKey();assert(h.el('toast').classList.contains('toast-success'));
});
