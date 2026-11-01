const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let server,browser,base;
before(async()=>{
 server=http.createServer((req,res)=>{
  const rel=decodeURIComponent(new URL(req.url,'http://local').pathname),file=path.resolve(root,'.'+(rel==='/'?'/index.html':rel));
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}const ext=path.extname(file);res.setHeader('Content-Type',({'.js':'application/javascript','.html':'text/html','.css':'text/css'})[ext]||'application/octet-stream');res.end(data);});
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true});
});
after(async()=>{await browser?.close();await new Promise(resolve=>server?.close(resolve));});
async function pageFor(language='en-US',handler){
 const context=await browser.newContext({locale:language,viewport:{width:1280,height:900}});
 await context.addInitScript(({language})=>{
   localStorage.setItem('ts_language',language);
   class Socket {static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}
   window.WebSocket=Socket;
 },{language});
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
   const u=new URL(route.request().url());
   if(u.origin!==base){await route.abort();return;}
   if(u.pathname.startsWith('/api/')){await route.fulfill({json:{code:0,data:{}}});return;}
   if(handler&&await handler(route,u))return;
   if (process.env.PROMPT_TEST_BASELINE && /^\/js\/(app|api|router|lang\/(en-US|zh-CN))\.js$/.test(u.pathname)) {
     const name=u.pathname.split('/').pop();await route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(process.env.PROMPT_TEST_BASELINE,name),'utf8')});return;
   }
   await route.continue();
 });
 return {page,context,errors};
}


for(const language of ['zh-CN','en-US']) {
 test('EXEC binary UTF-8, gap and authoritative result '+language,async()=>{
  const {page,context,errors}=await pageFor(language);try {
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   const data=await page.evaluate(()=>{
    closeLoginModal();document.getElementById('page-content').innerHTML='<section id="exec-result-section"><pre id="exec-result"></pre><button id="cancel-exec-btn"></button><div id="match-result-panel"></div></section>';
    const output=document.getElementById('exec-result');let owner=claimExecDisplay('ssh',output);acknowledgeExecSession(owner,7);
    const bytes=new TextEncoder().encode('中文"\\\n');let offset=0;
    for(const raw of [bytes.slice(0,1),bytes.slice(1,4),bytes.slice(4)]){
     handleSshExecMessage({type:'ssh_exec_output',session_id:7,data_b64:btoa(String.fromCharCode(...raw)),output_offset:offset,output_length:raw.length,is_stderr:false});offset+=raw.length;
    }
    handleSshExecMessage({type:'ssh_exec_done',session_id:7,status:'success',execution_status:'success',success:true,exit_code:0,output_bytes:offset,conditions_complete:true});
    const complete=output.textContent;output.textContent='';owner=claimExecDisplay('ssh',output);acknowledgeExecSession(owner,8);
    handleSshExecMessage({type:'ssh_exec_output',session_id:8,data_b64:'eA==',output_offset:10,output_length:1,is_stderr:false});
    handleSshExecMessage({type:'ssh_exec_done',session_id:8,status:'unknown',execution_status:'success',success:false,exit_code:0,output_bytes:11,conditions_complete:false,collection_truncated:true});
    return {complete,gap:output.textContent,finished:owner.finished,cancelHidden:document.getElementById('cancel-exec-btn').style.display==='none',correct:complete.includes('中文"\\\n'),replacement:complete.includes('�')};
   });
   assert(data.correct&&!data.replacement&&data.finished&&data.cancelHidden);assert.match(data.gap,language==='zh-CN'?/部分日志未收到/:/Some logs did not reach/);assert.match(data.gap,language==='zh-CN'?/退出码 0/:/confirmed exit code 0/);assert.match(data.gap,language==='zh-CN'?/条件判断无法完整确认/:/Conditions cannot be fully confirmed/);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
 for(const outputBytes of [0,40]) test('EXEC reopen queries exact result without repeating command '+language+' bytes='+outputBytes,async()=>{
  const {page,context,errors}=await pageFor(language);try {
   await page.goto(base);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate((outputBytes)=>{
    closeLoginModal();document.getElementById('page-content').innerHTML='<section id="exec-result-section"><pre id="exec-result"></pre><button id="cancel-exec-btn"></button><div id="match-result-panel"></div></section>';
    api.token='fixture';const owner=claimExecDisplay('ssh',document.getElementById('exec-result'));
    rememberExecReceipt(owner,{session_id:9,boot_id:'a'.repeat(32),result_key:'b'.repeat(32)});releaseExecDisplay();window.queryCalls=[];
    api.call=async(name,params,method)=>{queryCalls.push({name,params,method});return {code:0,data:{type:'ssh_exec_done',session_id:9,status:'success',execution_status:'success',success:true,exit_code:0,output_bytes:outputBytes}};};
    restoreExecResult();
   },outputBytes);
   await page.waitForFunction(()=>execDisplayOwner?.finished);
   const data=await page.evaluate(()=>{const calls=queryCalls,text=document.getElementById('exec-result').textContent;api.clearAuth();return {calls,text,receipt:sessionStorage.getItem(SSH_PENDING_RESULT)};});
   assert.equal(data.calls.length,1);assert.equal(data.calls[0].name,'ssh.exec_status');assert.equal(data.calls[0].method,'POST');assert.equal(data.calls[0].params.result_key,'b'.repeat(32));assert.equal(data.receipt,null);if(outputBytes)assert.match(data.text,language==='zh-CN'?/部分日志未收到/:/Some logs did not reach/);else assert.doesNotMatch(data.text,language==='zh-CN'?/部分日志未收到/:/Some logs did not reach/);assert.deepEqual(errors,[]);
  }finally{await context.close();}
 });
}
