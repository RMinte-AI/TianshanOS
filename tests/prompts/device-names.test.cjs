const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {harness,read}=require('./harness.cjs');
const hasRoleCode = value => /\b(?:AGX|LPMU)\b/.test(value.replace(/\b(?:Jetson )?AGX (?:Orin|Xavier)\b/g,''));
test('real model names are allowed without accepting role codes',()=>{
 assert(!hasRoleCode('Jetson AGX Orin'));assert(!hasRoleCode('AGX Xavier'));
 assert(hasRoleCode('AGX Power'));assert(hasRoleCode('Jetson AGX Orin and LPMU power'));
});
async function ready(lang){const h=harness(lang);await h.ready();h.load();return h;}
for(const lang of ['zh-CN','en-US']){
 test(`${lang}: USB display changes without changing device parameters`,async()=>{
  const h=await ready(lang);h.el('usb-mux-target');h.el('usb-mux-btn');
  h.run("window.requests=[];api.call=async(...args)=>{requests.push(args);return {code:0}};usbMuxConfigured=true;usbMuxTarget='esp32';");
  await h.run('toggleUsbMux()');
  assert.equal(h.el('usb-mux-target').textContent,lang==='zh-CN'?'推理计算机':'Inference Computer');
  assert.equal(h.ctx.requests[0][0],'device.usb.set');assert.equal(h.ctx.requests[0][1].target,'agx');
  h.run(`i18n.setLanguage('${lang==='zh-CN'?'en-US':'zh-CN'}',true);updateUsbMuxButton()`);
  assert.equal(h.el('usb-mux-target').textContent,lang==='zh-CN'?'Inference Computer':'推理计算机');
  await h.run('toggleUsbMux()');assert.equal(h.ctx.requests[1][1].target,'lpmu');
  h.run("usbMuxTarget='disconnected';updateUsbMuxButton()");
  assert.equal(h.el('usb-mux-target').textContent,lang==='zh-CN'?'Disconnected':'已断开');
  h.run("usbMuxTarget='future_target';updateUsbMuxButton()");
  assert.equal(h.el('usb-mux-target').textContent,h.run("t('common.unknown')"));
 });
 test(`${lang}: native and old task failures have the same explanation and preserve diagnostics`,async()=>{
  const h=await ready(lang);
  const result=h.run(`(()=>{const raw='failed to create LPMU access task';return {
    native:describeFailure({op:'network.lpmuaccess',result:{code:6,error:raw,data:{reason:'access_task_unavailable'}},write:true}),
    old:describeFailure({op:'network.lpmuaccess',result:{code:6,error:raw},write:true}),
    status:normalizeNetworkLpmuAccessStatus({stage:'failed',running:false,exit_code:-1,last_error:raw,reason:'access_task_unavailable',failure_stage:'queued'})};})()`);
  assert.equal(result.native.message,result.old.message);
  assert.match(result.native.message,lang==='zh-CN'?/管理控制器.*内存/:/management controller.*memory/);
  assert.match(result.status.summary,lang==='zh-CN'?/管理控制器.*内存/:/management controller.*memory/);
  assert(!/AGX|LPMU/.test(result.native.message+result.status.summary));
  assert(result.native.details.includes('failed to create LPMU access task'));
  assert.equal(result.status.technicalError,'failed to create LPMU access task');
 });
 test(`${lang}: explicit stages cannot collide and unknown phases do not expose machine names`,async()=>{
  const h=await ready(lang);
  assert.equal(h.run("normalizeNetworkLpmuAccessStage('uploading',{running:true})"),'upload');
  assert.equal(h.run("normalizeNetworkLpmuAccessStage('loading_key',{running:true})"),'check');
  assert.equal(h.run("normalizeNetworkLpmuAccessStatus({stage:'success',running:true}).phase"),'execute');
  assert.equal(h.run("normalizeNetworkLpmuAccessStatus({stage:'failed',running:true}).success"),false);
  assert.notEqual(h.run("getNetworkLpmuAccessStageLabel('uploading')"),h.run("getNetworkLpmuAccessStageLabel('loading_key')"));
  assert.equal(h.run("getNetworkLpmuAccessStageLabel('lpmu_new_phase')"),h.run("t('networkPage.lpmuAccessStageUnknown')"));
  for(const [alias,key] of [['pending','lpmuAccessStageQueued'],['running','lpmuAccessStageExecute']])
   assert.equal(h.run(`getNetworkLpmuAccessStageLabel(normalizeNetworkLpmuAccessStatus({stage:'${alias}',running:true}).phase)`),h.run(`t('networkPage.${key}')`));
 });
 test(`${lang}: unknown script output stays diagnostic and does not become an inferred cause`,async()=>{
  const h=await ready(lang);
  const result=h.run(`(()=>{const base={stage:'failed',running:false,exit_code:1,last_error:'setup-smart-route failed (exit=1): remote script failed'};return {
   unknown:normalizeNetworkLpmuAccessStatus({...base,output_tail:'✗ 错误：LPMU link check failed'}),
   known:normalizeNetworkLpmuAccessStatus({...base,output_tail:'✗ 错误：没有找到可用的互联网连接'}),
   future:normalizeNetworkLpmuAccessStatus({...base,reason:'future_reason',output_tail:'✗ 错误：没有找到可用的互联网连接'}),
   unpack:normalizeNetworkLpmuAccessStatus({...base,exit_code:0,reason:'access_unpack_failed',internet_confirmed:false,configuration_complete:false}),
   expected:t('networkPage.lpmuAccessNoInternet')};})()`);
  assert(!/LPMU|link check/.test(result.unknown.summary));assert.equal(result.known.summary,result.expected);
  assert.notEqual(result.future.summary,result.expected);assert.notEqual(result.unpack.summary,h.run("t('networkPage.lpmuAccessUnconfirmed')"));
  assert.equal(result.unknown.technicalError,'setup-smart-route failed (exit=1): remote script failed');
 });
 test(`${lang}: only the latest explicit script error supplies the cause`,async()=>{
  const h=await ready(lang);
  const known='✗ 错误：没有找到可用的互联网连接',unknown='✗ 错误：加载模块失败',sudo='sudo: 1 incorrect password attempt';
  const generic=h.run("describeFailure({op:'network.lpmuresult',result:{code:7,error:'setup-smart-route failed (exit=1): remote script failed',data:{reason:'access_script_failed'}},write:false}).message");
  for(const [output,expected] of [
   [known+'\n'+unknown,generic],[sudo+'\n'+unknown,generic],
   [known+'\nsudo: remote command not found',generic],
   [unknown+'\n'+known,h.run("t('networkPage.lpmuAccessNoInternet')")],
   [unknown+'\n'+sudo,h.run("t('networkPage.lpmuAccessPasswordIncorrect')")],
   [known+'\n当前路由表：\ndefault via 10.10.99.100',h.run("t('networkPage.lpmuAccessNoInternet')")]
  ]){
   h.ctx.reviewOutput=output;
   const result=h.run("normalizeNetworkLpmuAccessStatus({stage:'failed',running:false,reason:'access_script_failed',last_error:'setup-smart-route failed (exit=1): remote script failed',output_tail:reviewOutput})");
   assert.equal(result.summary,expected,output);assert.equal(result.outputTail,output);
  }
 });
 test(`${lang}: transport uncertainty is not converted into a task allocation failure`,async()=>{
  const h=await ready(lang);
  const result=h.run("describeFailure({op:'network.lpmuaccess',error:new ApiOperationError({code:6,error:'Failed to create LPMU access task',data:{reason:'access_task_unavailable'}},'lpmu.access.start',{kind:'network',uncertain:true}),write:true})");
  assert.equal(result.uncertain,true);assert(!/内存|memory/.test(result.message));
 });
 test(`${lang}: shipped UI names and firmware reason catalogue are complete`,async()=>{
  const h=await ready(lang),data={};
  require('node:vm').runInNewContext(read(`js/lang/${lang}.js`),{i18n:{registerLanguage:(_,d)=>Object.assign(data,d)}});
  const scan=(o)=>{for(const v of Object.values(o)){if(typeof v==='string')assert(!hasRoleCode(v),v);else scan(v);}};scan(data);
  const c=fs.readFileSync(require('node:path').resolve(__dirname,'../../components/ts_api/src/ts_api_lpmu_access.c'),'utf8');
  // Old firmware remains a supported producer even when a native reason changes.
  const reasons=[...new Set([...[...c.matchAll(/"(access_[a-z_]+)"/g)].map(x=>x[1]),
   ...h.run('NETWORK_ACCESS_LEGACY_REASONS.map(([,reason])=>reason)')])];
  assert(reasons.length>0);
  assert.deepEqual(Object.keys(data.failure.reasons).filter(k=>k.startsWith('access_')).sort(),reasons.sort());
  for(const reason of reasons)assert(data.failure.reasons[reason]?.why,reason);
 });
}
