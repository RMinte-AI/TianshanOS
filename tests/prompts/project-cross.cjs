/* Bidirectional bridge: final production C frames -> actual JS socket dispatcher ->
 * actual onData/start/exit requests -> real C ws_handler + operation + SSH drivers.
 * Network/libssh2/xterm rendering are boundaries; no lifecycle logic is replaced. */
const assert=require('node:assert/strict'),{spawn}=require('node:child_process'),readline=require('node:readline'),fs=require('node:fs');
const path=require('node:path'),{harness,read:sourceRead}=require('./harness.cjs');
const read=file=>process.env.PROJECT_WEB_ROOT?fs.readFileSync(path.join(process.env.PROJECT_WEB_ROOT,file),'utf8'):sourceRead(file);
async function engine(language,browserMode){
 if(!browserMode){const h=harness(language);await h.ready();h.load();await h.run(`i18n.setLanguage('${language}')`);return {run:async s=>h.run(s),close:async()=>{}};}
 const {chromium}=require('playwright');const browser=await chromium.launch({channel:'chrome',headless:true});
 const page=await browser.newPage();const errors=[];page.on('pageerror',err=>errors.push(err.message));
 await page.addInitScript(language=>{localStorage.setItem('ts_language',language);window.WebSocket=class{static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){}};},language);
 await page.route('**/*',async r=>{
  const url=new URL(r.request().url());if(url.origin!=='http://fixture.local'){await r.abort();return;}
  if(url.pathname.startsWith('/api/')){await r.fulfill({json:{code:0,data:{}}});return;}
  const rel=url.pathname==='/'?'index.html':url.pathname.slice(1);
  try{await r.fulfill({body:read(rel),contentType:rel.endsWith('.js')?'application/javascript':rel.endsWith('.css')?'text/css':rel.endsWith('.html')?'text/html':'application/octet-stream'});}catch{await r.abort();}
 });
 await page.goto('http://fixture.local/');await page.waitForFunction(()=>i18n.isReady());
 await page.evaluate(()=>{const f=document.createElement('div');f.id='fixture';document.body.append(f);});
 return {run:s=>page.evaluate(s),close:async()=>{await browser.close();assert.deepEqual(errors,[]);}};
}
async function main(){
 const baseline=process.argv.includes('--baseline'),browserMode=process.argv.includes('--browser');
 const language=process.env.CROSS_LANGUAGE||'en-US',e=await engine(language,browserMode);
 let c;const records=[];let exit,err='',pending,chain=Promise.resolve();
 try{
 await e.run(`window.sent=[];window.lines=[];window.input=null;
 window.WebSocket=class {static OPEN=1;static CONNECTING=0;constructor(){this.readyState=1;}send(s){sent.push(JSON.parse(s));}close(){this.readyState=3;}};
 window.term=new WebTerminal('fixture');term.terminal={write:s=>lines.push(s),writeln:s=>lines.push(s),onData:f=>input=f,dispose:()=>{}};
 term.setupInputHandler();term.connect();term.ws.onopen();`);
 c=spawn('bash',['tests/ws_subscriptions/run_project_bridge.sh'],{cwd:path.resolve(__dirname,'../..'),env:process.env});
 c.stderr.on('data',b=>err+=b);
 const done=new Promise(r=>c.on('close',code=>{exit=code;r(code);if(pending){pending.reject(new Error(err||'C exited '+code));pending=null;}}));
 readline.createInterface({input:c.stdout}).on('line',line=>{
  chain=chain.then(async()=>{const m=JSON.parse(line);records.push(m);
   if(m.frame && m.fd===1)await e.run(`term.ws.onmessage({data:${JSON.stringify(m.bytes || JSON.stringify(m.frame))}})`);
   if('state'in m||m.finished){const p=pending;pending=null;p?.resolve(m);}
  }).catch(error=>{pending?.reject(error);pending=null;});
 });
 const step=msg=>new Promise((resolve,reject)=>{assert(!pending);pending={resolve,reject};c.stdin.write(JSON.stringify(msg)+'\n');});
 const sendNext=async()=>{const message=await e.run('sent.shift()');assert(message,'frontend generated no request');return step({message});};
 const start=async()=>{await e.run(`term.startSshShell({host:'192.0.2.1',user:'fake',password:'fake',port:22})`);return sendNext();};
 const state=()=>e.run(`({mode:term.sshMode,connecting:term.sshConnecting,restoring:term.restoring,connected:term.connected,lines:[...lines],sent:[...sent]})`);
 await sendNext();await start();assert((await state()).mode);
 const before=(await state()).lines.length;const afterSignal=await step({message:{type:'ssh_signal',signal:'TERM'}});
 const s=await state();const recovered=s.mode&&!s.restoring&&!s.lines.slice(before).some(x=>x==='tianshan> ');
 await e.run(`input('x\\r')`);const next=await e.run('sent.shift()');
 if(baseline){assert(!recovered && next.type!=='ssh_input');await step({cmd:'finish'});await done;assert.equal(exit,0,err);console.log('CONFIRMED H1: actual C request error leaves op OPEN but JS exits SSH and next input is not routed to SSH');return;}
 assert(recovered,'request failure changed active SSH mode/prompt/recovery');assert.equal(next.type,'ssh_input');
 let cs=await step({message:next});assert.equal(cs.writes,afterSignal.writes+1,'next frontend input must execute on original C Shell');
 // J1: real C protection notification must not fabricate local SSH input.
 const j1Before=await state(),j1Frames=records.length,j1Identity=cs.identity,j1Writes=cs.writes;
 await e.run(`term.inputBuffer='local-buffer';term.cursorPosition=3;`);
 const j1Fields=()=>e.run(`JSON.stringify([term.connected,term.sshMode,term.sshConnecting,term.sshDisconnecting,term.restoring,term.inputBuffer,term.cursorPosition])`);
 const j1Snapshot=await j1Fields();cs=await step({cmd:'power'});
 const j1After=await state(),j1Display=j1After.lines.slice(j1Before.lines.length);
 assert(records.slice(j1Frames).some(r=>r.fd===1&&r.frame?.type==='power_event'),'actual C power frame absent');
 assert.equal(j1Display.length,1);assert(j1Display[0].startsWith('\r\n'));
 assert(!j1Display.join('').includes('tianshan>')&&!j1Display.join('').includes('local-buffer')&&!j1Display.join('').includes('\x1b[K'));
 assert.equal(await j1Fields(),j1Snapshot);assert.deepEqual(j1After.sent,j1Before.sent);
 await e.run(`input('j')`);const j1Request=await e.run('sent.shift()');assert.equal(j1Request.type,'ssh_input');
 cs=await step({message:j1Request});assert.equal(cs.identity,j1Identity);assert.equal(cs.writes,j1Writes+1);
 await e.run(`input('\\x1c')`);await sendNext();assert(!(await state()).mode);assert.equal((await step({cmd:'drain'})).channels,0);
 assert((await state()).lines.slice(j1Before.lines.length).includes('tianshan> '));
 await e.run(`term.inputBuffer='';term.cursorPosition=0;`);cs=await start();assert((await state()).mode);
 console.log('PASS J1 C power event -> actual JS -> original SSH input -> normal exit and channel cleanup');
 for(const message of [{type:'ssh_resize',width:0,height:20},{type:'ssh_resize',width:80.5,height:20}]){await step({message});assert((await state()).mode);}
 await step({cmd:'resize_reject'});await step({message:{type:'ssh_resize',width:80,height:24}});assert((await state()).mode);await step({cmd:'reset_driver'});
 // Another page cannot input, disconnect, signal or resize the current owner.
 const n=cs.writes;for(const message of [{type:'ssh_input',data:'wrong'},{type:'ssh_disconnect'},{type:'ssh_signal',signal:'INT'},{type:'ssh_resize',width:80,height:24},{type:'ssh_connect',host:'192.0.2.2',user:'other'}]){cs=await step({fd:2,message});assert.equal(cs.writes,n);assert.equal(cs.channels,1);assert((await state()).mode);}
 // A rejected duplicate start on the owner also must not terminate its live op.
 await step({message:{type:'ssh_connect',host:'192.0.2.1',user:'fake'}});assert((await state()).mode);
 await e.run(`input('\\x1c')`);await sendNext();assert(!(await state()).mode);assert((await state()).connected);
 await e.run(`input('help\\r')`);const local=await e.run('sent.shift()');assert.equal(local.type,'terminal_input');await step({message:local});
 await step({message:{type:'terminal_stop'}});await e.run(`input('help\\r')`);await sendNext();assert((await state()).restoring);
 const restore=await e.run('sent.shift()');assert.equal(restore.type,'terminal_start');await step({message:restore});assert((await state()).connected && !(await state()).restoring);
 await start();assert((await state()).mode);await step({cmd:'write_fail'});await e.run(`input('x')`);await sendNext();assert(!(await state()).mode);assert(!(await state()).restoring);assert((await state()).lines.some(x=>/unconfirmed|尚未确认/.test(x)));
 await step({cmd:'reset_driver'});await start();assert((await state()).mode);await step({cmd:'eof'});assert(!(await state()).mode);
 await step({cmd:'reset_driver'});await step({cmd:'setup_fail'});await start();assert(!(await state()).mode && !(await state()).connecting);await step({cmd:'reset_driver'});await start();assert((await state()).mode);
 // Failed notification encoding does not change active request ownership.
 await step({cmd:'encode_fail'});await step({message:{type:'ssh_signal',signal:'TERM'}});await step({cmd:'encode_resume'});assert((await state()).mode);await e.run(`input('z')`);await sendNext();
 // A control failure crosses output preparation and a genuine close: exactly one terminal,
 // queued output precedes it, then next frontend action is local (no automatic replay).
 const crossing=records.length;await step({cmd:'error_close'});await step({message:{type:'ssh_signal',signal:'TERM'}});assert(!(await state()).mode);
 const crossed=records.slice(crossing).filter(r=>r.frame&&r.fd===1).map(r=>r.frame);
 assert.equal(crossed.filter(f=>f.type==='ssh_status'&&f.status==='closed').length,1);
 assert(crossed.findIndex(f=>f.type==='ssh_output')<crossed.findIndex(f=>f.type==='ssh_status'&&f.status==='closed'));
 await start();assert((await state()).mode);
 // Direct request notification send failure cannot recursively change the operation.
 await step({cmd:'send_fail'});await step({message:{type:'ssh_signal',signal:'TERM'}});await step({cmd:'send_resume'});assert((await state()).mode);
 await e.run(`input('q')`);await sendNext();
 // Output accepted while HTTPD is unavailable follows the existing original-operation
 // failure path; capacity recovers with no new business input, and resources settle.
 await step({cmd:'queue_fail'});await step({cmd:'output'});await step({cmd:'queue_resume'});assert(!(await state()).mode);
 await step({cmd:'finish'});await done;assert.equal(exit,0,err);
 // Old socket frames/close and callbacks after destroy cannot change new instance.
 const stale=await e.run(`(()=>{const old=term.ws;term.connect();term.connected=true;term.sshMode=true;const fresh=term.ws;old.onmessage({data:JSON.stringify({type:'ssh_status',status:'error'})});old.onclose({code:1006});const held=term.sshMode&&term.connected&&term.ws===fresh;term.destroy();fresh.onmessage({data:JSON.stringify({type:'connected'})});return held&&!term.connected;})()`);assert(stale);
 console.log(`PASS ${browserMode?'Chrome':'Node'} ${language}: actual C frames + JS dispatcher + next C requests; recoverable/fatal/start/EOF/owner/encoding/socket paths; C resources zero`);
 }finally{
  if(process.env.CROSS_OUTPUT)fs.writeFileSync(process.env.CROSS_OUTPUT,JSON.stringify({records,stderr:err,exit},null,2));
  if(c && exit===undefined)c.kill('SIGTERM');await e.close();
 }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
