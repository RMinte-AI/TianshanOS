const {test}=require('node:test');
const assert=require('node:assert/strict');
const {harness}=require('./harness.cjs');
async function setup(lang){const h=harness(lang);await h.ready();h.load();return h;}
for(const lang of ['zh-CN','en-US']) {
 test(`${lang}: actual inline translator, cold language, missing/objects/parameters and confirmations`, async()=>{
    const h=await setup(lang),{ctx,run}=h;
    assert.equal(ctx.getLanguage(),lang);
    assert(!ctx.t('ui.confirmRollback').includes('ui.'));
    for(const [key,params]of [['missing.key',{}],['common',{}],['toast.ledConfigSavedWithAnim',{device:'matrix'}]]) {
        assert.equal(ctx.t(key,params),ctx.i18n.unavailable());let confirmations=0;ctx.confirm=()=>{confirmations++;return true;};
        assert.equal(ctx.confirmAction(ctx.t(key,params)),false);assert.equal(confirmations,0);
    }
    assert(!ctx.t('toast.ledConfigSavedWithAnim',{device:'matrix',animation:'rainbow'}).includes('{animation}'));
    assert(ctx.t('dataWidget.dataExpressionHint').includes('${'));
    assert.equal(ctx.confirmAction(ctx.t('ui.confirmRollback')),true);
 });
 test(`${lang}: REST business failure remains returned; strict operation rejects with original metadata`,async()=>{
    const h=await setup(lang),{ctx,run}=h;
    ctx.fetch=async()=>({ok:true,status:200,text:async()=>JSON.stringify({code:3,error:'fixture permission'})});
    const result=await run("api.request('/storage/delete','POST')");assert.equal(result.code,3);assert.equal(result.rawMessage,'fixture permission');
    assert(result.message.includes('fixture permission'));assert(result.message!==result.rawMessage);
    assert.throws(()=>ctx.requireApiSuccess(result,'storage.delete'),e=>e.code===3&&e.operation==='storage.delete'&&e.rawMessage==='fixture permission');
    assert.throws(()=>ctx.requireApiSuccess({success:true},'storage.delete'));
    ctx.fetch=async()=>({ok:false,status:503,text:async()=>JSON.stringify({code:0,error:'server down'})});
    await assert.rejects(run("api.request('/x')"),e=>e.httpStatus===503);
    ctx.fetch=async()=>({ok:true,status:200,text:async()=>'{invalid'});
    await assert.rejects(run("api.request('/x','POST')"),e=>e.kind==='format'&&e.uncertain===true&&e.httpStatus===200);
 });
 test(`${lang}: timeout covers response body; result stays unknown for POST`,async()=>{
    const h=await setup(lang),{ctx,run}=h;
    ctx.fetch=async(url,{signal})=>({ok:true,status:200,text:()=>new Promise((resolve,reject)=>signal.addEventListener('abort',()=>reject(Object.assign(new Error('aborted'),{name:'AbortError'}))))});
    const request=run("api.request('/ssh/exec_stream','POST',{},20)");await new Promise(resolve=>setImmediate(resolve));
    const check=assert.rejects(request,e=>e.kind==='timeout'&&e.uncertain&&e.operation==='/ssh/exec_stream');await h.advance(20);await check;
 });
 test(`${lang}: toast timer replacement, priority, safe multiline text`,async()=>{
    const h=await setup(lang);h.ctx.showToast('short','info',3000);await h.advance(2500);
    h.ctx.showToast('<img src=x onerror=alert(1)>\nwarning','error',10000);await h.advance(500);
    let toast=h.el('toast');assert(toast.classList.contains('show'));assert(toast.textContent.includes('<img'));assert.equal(toast.innerHTML,undefined);
    h.ctx.showToast('routine success','success',3000);assert(toast.textContent.includes('warning'));
    await h.advance(9499);assert(toast.classList.contains('show'));await h.advance(1);assert(!toast.classList.contains('show'));
 });
 test(`${lang}: actual mutations do not report success or close dialogs on business failures`,async()=>{
    const h=await setup(lang);h.run("window.mutationCalls=0;api.storageMkdir=api.storageRename=api.storageDelete=api.storageMount=api.storageUnmount=api.keyGenerate=api.fanSet=api.serviceRestart=async()=>{mutationCalls++;return {code:3,error:'denied'};}; api.call=async()=>{mutationCalls++;return {code:3,error:'denied'};}; refreshFilesPage=async()=>{};");
    for(const id of ['keygen-modal','newfolder-modal','rename-modal','new-folder-name','rename-original-path','rename-input','keygen-id','keygen-type','keygen-comment','keygen-alias','keygen-exportable','keygen-hidden'])h.el(id).value='fixture';
    // function uses rename-original, not a duplicate of production logic
    h.el('rename-original').value='/sdcard/old';h.el('rename-name').value='new';
    for(const code of ["createNewFolder()","doRename()","deleteFile('/sdcard/a')","mountSdCard()","unmountSdCard()","generateKey()","setFanMode(0,'auto')","serviceAction('fixture','restart')","saveLedConfig('matrix')"]){
        const before=h.ctx.mutationCalls;await h.run(code);assert(h.ctx.mutationCalls>before,code+' reached production API');assert(!h.el('toast').classList.contains('toast-success'),code);
    }
    for(const id of ['keygen-modal','newfolder-modal','rename-modal'])assert(!h.el(id).classList.contains('hidden'));
 });
 test(`${lang}: batch deletion all / partial / no success with business errors`,async()=>{
    const h=await setup(lang);h.run('refreshFilesPage=async()=>{};');
    for(const outcomes of [[0,0],[0,3],[3,3]]){
        h.ctx.outcomes=[...outcomes];h.run("selectedFiles.clear();selectedFiles.add('/a');selectedFiles.add('/b');api.storageDelete=async()=>({code:outcomes.shift(),error:'denied'});");
        const result=await h.run('batchDelete()');assert.equal(result.success,outcomes.filter(x=>x===0).length);assert.equal(result.failures.length,outcomes.filter(x=>x!==0).length);
    }
 });
 test(`${lang}: downloads propagate actual initiation failures and retain batch details`,async()=>{
    const h=await setup(lang);let clicks=0;const create=h.doc.createElement;
    h.doc.createElement=tag=>{let el=create(tag);if(tag==='a')el.click=()=>{clicks++;};return el;};
    h.run("api.fileDownload=async(path)=>{if(path==='/bad')throw new Error('download failed');return new Blob(['content']);};");
    assert.equal((await h.run("downloadFile('/bad',true)")).started,false);
    h.run("selectedFiles.clear();selectedFiles.add('/ok');selectedFiles.add('/bad');");h.doc.querySelector=s=>s==='main'?h.doc.main:{dataset:{type:'file'}};
    let result=await h.run('batchDownload()');assert.equal(result.success,1);assert.equal(result.failures.length,1);assert.equal(clicks,1);
    h.doc.createElement=tag=>{let el=create(tag);if(tag==='a')el.click=()=>{throw new Error('not started');};return el;};
    assert.equal((await h.run("downloadFile('/ok',true)")).started,false);
 });
 test(`${lang}: file names are text; upload/verification/apply remain separate`,async()=>{
    const h=await setup(lang);h.el('upload-list');h.run("currentFilePath='/sdcard'; filesToUpload=[];api.fileUpload=async(path)=>{if(path.includes('bad'))throw new Error('upload failed');return {status:'uploaded',config_pack:{valid:false,result_message:'invalid signature'}};};");
    const filename='<img src=x onerror=alert(1)>.tscfg';h.ctx.handleFileSelect({target:{files:[{name:filename,size:12},{name:'bad',size:1}]}});
    assert.equal(h.el('upload-list').children[0].children[0].textContent,filename);
    let result=await h.run('uploadFiles()');assert.equal(result.success,1);assert.equal(result.failures.length,1);
    assert(!h.el('upload-modal').classList.contains('hidden'));assert.equal(h.el('upload-list').children[0].children.length,3);
 });
 test(`${lang}: SSH actual event handler honors terminal states and diagnoses conflicts`,async()=>{
    const h=await setup(lang);for(const id of ['exec-result','cancel-exec-btn','match-result-panel','match-status-badge','match-final-status'])h.el(id);
    const cases=[['match_failed',0,false,'sshExpectedMissing'],['match_failed',0,true,'sshFailureOutput'],['timeout',0,false,'sshTimeout'],['cancelled',0,false,'sshCancelled'],['unknown',0,false,'sshUnknown'],['success',7,false,'sshUnknown'],['match_success',7,false,'sshSuccess'],['success',0,false,'sshSuccess']];
    for(const[status,exit_code,fail_matched,key]of cases){
        h.run('currentExecSessionId=17;toastDeadline=0;');h.ctx.handleSshExecMessage({type:'ssh_exec_done',session_id:17,status,exit_code,fail_matched});
        assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.'+key));assert.equal(h.el('match-final-status').textContent,h.ctx.t('promptRepair.'+key));
    }
    h.run('currentExecSessionId=17;toastDeadline=0;');
    h.ctx.handleSshExecMessage({type:'ssh_exec_cancelled',session_id:17});
    assert.equal(h.run('currentExecSessionId'),null);
    assert(h.el('exec-result').textContent.includes(h.ctx.t('promptRepair.sshCancelOutput')));
    assert.match(h.el('exec-result').textContent,lang==='zh-CN'?/远端命令是否终止尚未确认/:/remote command termination is not confirmed/);
    assert.equal(h.el('toast').textContent,h.ctx.t('toast.commandCancelled'));
    assert.equal(h.ctx.sshTerminalResult({status:'match_failed',success:true}).key,'sshUnknown');
    h.run('currentExecSessionId=17;toastDeadline=0;');h.ctx.handleSshExecMessage({type:'ssh_exec_match',session_id:17,is_final:true,fail_matched:true});
    assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.sshFailureOutput'));
 });
 test(`${lang}: background command timeout is unconfirmed and never resubmitted`,async()=>{
    const h=await setup(lang);for(const id of ['exec-result','exec-result-section','cancel-exec-btn','nohup-actions'])h.el(id);
    h.run("selectedHostId='test';sshCommands={test:[{id:'id',name:'name',command:'echo test',nohup:true}]};window._cmdHostsList=[{id:'test',username:'user',host:'fixture',port:22}];window.calls=0;api.call=async()=>{window.calls++;throw new ApiOperationError({},'ssh.exec_stream',{kind:'timeout',uncertain:true});};");
    await h.run('executeCommand(0)');assert.equal(h.ctx.calls,1);assert(h.el('exec-result').textContent.includes(h.ctx.t('promptRepair.sshSubmissionUnknown')));
    h.run("api.call=async()=>{window.calls++;return {code:5,error:'timeout'};};");await h.run('executeCommand(0)');assert.equal(h.ctx.calls,2);assert(h.el('exec-result').textContent.includes(h.ctx.t('promptRepair.sshSubmissionUnknown')));
 });
 test(`${lang}: terminal restoration waits for acknowledgement; timeout/failure stay distinct`,async()=>{
    const h=await setup(lang);h.run("window.term=new WebTerminal('test');term.writeln=()=>{};term.writePrompt=()=>{};window.sent=[];term.ws={readyState:1,send:v=>sent.push(v)};");
    h.ctx.term.handleMessage({type:'error',message:'Not a terminal session'});assert.equal(h.ctx.sent.length,1);assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.terminalRestoring'));
    h.ctx.term.handleMessage({type:'connected'});assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.terminalRestored'));assert(h.ctx.term.connected);
    h.ctx.term.handleMessage({type:'error',message:'Not a terminal session'});await h.advance(10000);assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.terminalRestoreTimeout'));
    h.run('toastDeadline=0;');h.ctx.term.handleMessage({type:'error',message:'Not a terminal session'});h.ctx.term.handleMessage({type:'error',message:'denied'});
    assert.equal(h.el('toast').textContent,h.ctx.t('promptRepair.terminalRestoreFailed'));
 });
 test(`${lang}: ping results are reachability only; failed probe is unknown and no pulse replay`,async()=>{
    const h=await setup(lang);h.el('lpmu-power-btn');
    for(const [response,state]of [[{code:0,data:{reachable:false}},'offline'],[{code:0,data:{reachable:true}},'online'],[{code:4,error:'busy'},'unknown'],[{code:0,data:{}},'unknown']]){
        h.ctx.response=response;h.run('api.call=async()=>response;');await h.run('refreshLpmuState()');assert.equal(h.run('lpmuState'),state);
    }
    let calls=[];h.ctx.callMock=async(name)=>{calls.push(name);throw new Error('probe failed');};h.run('api.call=callMock;startLpmuStatePolling(true);');await h.advance(60000);
    assert.equal(h.run('lpmuState'),'unknown');assert(calls.every(n=>n==='device.ping'));assert(!h.el('lpmu-power-btn').title.includes('已关闭'));
 });
 test(`${lang}: websocket label matches color across language changes and power warnings persist`,async()=>{
    const h=await setup(lang);h.ctx.renderWsStatus(true);assert(h.el('ws-status').classList.contains('connected'));
    h.ctx.renderWsStatus(false);assert.equal(h.el('ws-status').title,h.ctx.t('network.disconnected'));
    h.ctx.handlePowerEvent({state:'LOW_VOLTAGE',voltage:12.3,countdown:60});assert(!h.el('power-notice').hidden);
    await h.advance(10000);assert(!h.el('power-notice').hidden);assert(h.el('power-notice').textContent.includes('12.30'));
 });
}
for(const lang of ['zh-CN','en-US']) {
 test(`${lang}: upload and download aggregates cover all success and all failure`,async()=>{
  const h=await setup(lang);h.el('upload-list');h.el('upload-modal');
  for(const code of [0,7]) {
   h.ctx.outcome=code;h.run("currentFilePath='/sdcard'; filesToUpload=[];api.fileUpload=async()=>{if(outcome)throw new Error('failed');return {status:'uploaded'};};");
   h.ctx.handleFileSelect({target:{files:[{name:'a',size:1},{name:'b',size:1}]}});
   const result=await h.run('uploadFiles()');assert.equal(result.success,code?0:2);assert.equal(result.failures.length,code?2:0);
   h.run("selectedFiles.clear();selectedFiles.add('/a');selectedFiles.add('/b');api.fileDownload=async()=>{if(outcome)throw new Error('failed');return new Blob(['ok']);};");
   h.doc.querySelector=s=>s==='main'?h.doc.main:{dataset:{type:'file'}};
   const d=await h.run('batchDownload()');assert.equal(d.success,code?0:2);assert.equal(d.failures.length,code?2:0);
  }
 });
 test(`${lang}: key deployment requires successful business code and distinguishes verification`,async()=>{
  const h=await setup(lang);for(const id of ['deploy-host','deploy-user','deploy-port','deploy-password','deploy-result','deploy-btn'])h.el(id).value='fixture';
  h.run("currentDeployKeyId='key';loadSshHostsData=async()=>{};");
  for(const [result,key]of [[{code:0,data:{deployed:true,verified:true}},'keyVerified'],[{code:0,data:{deployed:true,verified:false}},'keyUnverified'],[{code:3,error:'denied',data:{deployed:true,verified:true}},null]]){
   h.ctx.fixture=result;h.run('api.sshCopyid=async()=>fixture;');await h.run('deployKey()');
   if(key)assert.equal(h.el('deploy-result').textContent,h.ctx.t('promptRepair.'+key,{id:'key',target:'fixture@fixture'}));
   else assert(h.el('deploy-result').classList.contains('error'));
  }
 });
 test(`${lang}: real LED/status renderers localize finite built-in identifiers`,async()=>{
  const h=await setup(lang);
  for(const name of ['rainbow','color_cycle','breathe_wave','coderain','unknown'])assert(!h.ctx.effectDisplayName(name).includes('promptRepair.'));
  for(const name of ['pulse','grayscale','fade-in','color-shift','unknown'])assert(!h.ctx.filterDisplayName(name).includes('ledPage.'));
  for(const name of ['RUNNING','STOPPED','STARTING','Blocked','future'])assert(!h.ctx.userStateLabel(name).includes('promptRepair.'));
  h.run("api.call=async()=>({code:0,data:{animation:'rainbow'}});");await h.run("saveLedConfig('matrix')");
  assert.equal(h.el('toast').textContent,h.ctx.t('toast.ledConfigSavedWithAnim',{device:h.ctx.getDeviceDescription('matrix'),animation:h.ctx.effectDisplayName('rainbow')}));
 });
}
for(const lang of ['zh-CN','en-US']) {
 test(`${lang}: widget dynamic families render real labels and literal braces in names remain valid`,async()=>{
  const h=await setup(lang);h.el('dw-manager-main');h.ctx.showAddWidgetPanel();
  assert(!h.el('dw-manager-main').innerHTML.includes(h.ctx.i18n.unavailable()));
  assert(!/dataWidget\.(?:type|preset)/.test(h.el('dw-manager-main').innerHTML));
  assert(h.ctx.confirmAction(h.ctx.t('ui.confirmDeleteCmd',{name:'file{my_name}'})));
  for(const phase of ['PLATFORM','HAL','DRIVER','NETWORK','UNKNOWN'])assert(!h.ctx.servicePhaseLabel(phase).includes('promptRepair.'));
 });
 test(`${lang}: unknown stop result never escalates to forced termination`,async()=>{
  const h=await setup(lang);for(const id of ['exec-result-section','exec-result','cancel-exec-btn','nohup-actions'])h.el(id);
  h.run("selectedHostId='test';sshCommands={test:[{name:'fixture'}]};window._cmdHostsList=[{id:'test',host:'fixture'}];updateServiceStatusInList=()=>{};");
  for(const outputs of [[''],['RUNNING:123',''],['RUNNING:123','STILL_RUNNING','']]){
   h.ctx.outputs=[...outputs];h.run("window.calls=[];api.call=async(name,args)=>{calls.push(args.command);return {code:0,data:{stdout:outputs.shift()}};};");
   await h.run("stopServiceProcess(0,'fixture')");assert.equal(h.ctx.calls.length,outputs.length);
   if(outputs.length<3)assert(h.ctx.calls.every(c=>!c.includes('kill -9')));
   assert(!h.el('toast').classList.contains('toast-success'));
  }
 });
}

for (const lang of ['zh-CN','en-US']) {
 test(`${lang}: J1 power alerts preserve local line, SSH ownership, transition guards and filtering`,async()=>{
  const h=await setup(lang),{run,ctx}=h;
  run(`window.j1=new WebTerminal('fixture');window.j1Writes=[];window.j1Sent=[];window.j1Input=null;
   j1.terminal={write:s=>j1Writes.push(s),writeln:s=>j1Writes.push(s),onData:f=>j1Input=f};
   j1.ws={readyState:WebSocket.OPEN,send:s=>j1Sent.push(JSON.parse(s))};j1.connected=true;
   j1.inputBuffer='abcd';j1.cursorPosition=2;j1.setupInputHandler();`);
  const alert={type:'power_event',event:'low_voltage',voltage:10.5};
  const snapshot=()=>run(`JSON.stringify([j1.connected,j1.sshMode,j1.sshConnecting,j1.sshDisconnecting,j1.restoring,j1.inputBuffer,j1.cursorPosition])`);
  const deliver=msg=>ctx.j1.handleMessage(msg||alert);
  const original=snapshot();deliver();deliver();assert.equal(snapshot(),original);assert.equal(ctx.j1Sent.length,0);
  const local=[...ctx.j1Writes];assert.equal(local.length,8);
  for(const offset of [0,4]){assert.equal(local[offset],'\r\x1b[K');assert(local[offset+1].includes(ctx.t('terminal.lowVoltageWarning',{voltage:'10.50'})));assert.equal(local[offset+2],'tianshan> abcd');assert.equal(local[offset+3],'\x1b[2D');}
  const cases=[{sshMode:true},{sshConnecting:true},{sshMode:true,sshDisconnecting:true},{restoring:true,connected:false},{restoring:true},{connected:false},{socket:3},{socket:null}];
  for(const changes of cases){
   run(`Object.assign(j1,{connected:true,sshMode:false,sshConnecting:false,sshDisconnecting:false,restoring:false});j1.ws={readyState:1,send:s=>j1Sent.push(JSON.parse(s))};j1Writes.length=0;`);
   for(const [key,value]of Object.entries(changes)){if(key==='socket'){if(value===null)ctx.j1.ws=null;else ctx.j1.ws.readyState=value;}else ctx.j1[key]=value;}
   const before=snapshot();deliver();deliver();assert.equal(snapshot(),before);assert.equal(ctx.j1Sent.length,0);
   assert.equal(ctx.j1Writes.length,2);for(const line of ctx.j1Writes){assert(line.startsWith('\r\n'));assert(!line.includes('tianshan>'));assert(!line.includes('abcd'));assert(!line.includes('\x1b[K'));assert(!line.includes('\x1b[2D'));}
   if(changes.sshConnecting||changes.sshDisconnecting||changes.connected===false){run('j1Input('+JSON.stringify('x\r')+')');assert.equal(ctx.j1Sent.length,0);}
  }
  run(`Object.assign(j1,{connected:true,sshMode:true,sshConnecting:false,sshDisconnecting:false,restoring:false});j1.ws={readyState:1,send:s=>j1Sent.push(JSON.parse(s))};j1Writes.length=0;`);
  deliver();run(`j1Input('z')`);assert.equal(ctx.j1Sent[0].type,'ssh_input');assert.equal(ctx.j1Sent[0].data,'z');
  run(`j1Input('\x1c')`);assert.equal(ctx.j1Sent[1].type,'ssh_disconnect');assert(ctx.j1.sshMode);assert(!ctx.j1Writes.join('').includes('tianshan>'));
  deliver({type:'ssh_status',status:'disconnecting'});deliver();assert(ctx.j1.sshMode);assert(!ctx.j1Writes.join('').includes('tianshan>'));
  deliver({type:'ssh_status',status:'closed'});assert(!ctx.j1.sshMode);assert.equal(ctx.j1Writes.filter(x=>x==='tianshan> ').length,1);
  ctx.j1Writes.length=0;deliver();assert.equal(ctx.j1Writes[2],'tianshan> abcd');assert.equal(ctx.j1Writes[3],'\x1b[2D');assert.equal(ctx.j1Sent.length,2);
  ctx.j1Writes.length=0;deliver({type:'power_event',event:'countdown_tick',countdown:31});assert.equal(ctx.j1Writes.length,0);
  for(const event of ['low_voltage','shutdown_start','protected','recovery_start','recovery_complete','debug_tick','state_changed','countdown_tick']){
   ctx.j1Writes.length=0;deliver({type:'power_event',event,countdown:30,voltage:10.5,state:'NORMAL'});assert.equal(ctx.j1Writes.length,4,event);
  }
  ctx.j1.destroyed=true;ctx.j1.terminal=new Proxy({}, {get(){throw Error('destroyed terminal accessed');}});
  assert.doesNotThrow(()=>ctx.j1.handlePowerEvent(alert));assert.doesNotThrow(()=>deliver());
  ctx.j1.destroyed=false;ctx.j1.terminal=null;assert.doesNotThrow(()=>ctx.j1.handlePowerEvent(alert));assert.equal(ctx.j1Sent.length,2);
 });
}
