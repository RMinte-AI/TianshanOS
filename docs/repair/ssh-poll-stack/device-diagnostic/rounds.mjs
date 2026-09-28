import fs from 'node:fs';
const dir='/tmp/tianshan-ssh-stack-diagnostic',base='http://10.10.99.97';
const password=process.env.TS_TEST_PASSWORD;if(!password)throw Error('Missing credential');
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function snapshot(stage){let r={time:new Date().toISOString(),stage};for(const n of ['tasks','memory','info']){const a=await fetch(base+'/api/v1/system/'+n,{signal:AbortSignal.timeout(5000)});r[n]=await a.json();if(r[n].code!==0)throw Error('API failure '+n);}fs.appendFileSync(dir+'/automatic-samples.jsonl',JSON.stringify(r)+'\n');return r;}
const pollers=r=>r.tasks.data.tasks.filter(t=>t.name==='ssh_poll');
function faults(){return fs.readFileSync(dir+'/serial.log','utf8').split('\n').filter(l=>/stack overflow|stack canary|Guru Meditation|CORRUPT HEAP|assert failed|watchdog|heap poisoning|LoadProhibited|StoreProhibited|Unhandled debug exception|stack smashing/i.test(l));}
for(let round=2;round<=10;round++){
 const before=await snapshot('round'+round+'_before');if(pollers(before).length)throw Error('Existing SSH task: stop without takeover');
 let messages=[],output='',connected=false,closed=false,err=null;
 const ws=new WebSocket('ws://10.10.99.97/ws');
 ws.addEventListener('message',e=>{try{let m=JSON.parse(e.data);messages.push(m);if(m.type==='ssh_output'){output+=m.data||'';fs.appendFileSync(dir+'/output-round'+round+'.txt',m.data||'');}if(m.type==='ssh_status'){if(m.status==='connected')connected=true;if(m.status==='closed')closed=true;if(m.status==='error')err=Error(m.message);}if(m.type==='error')err=Error(m.message||m.error);}catch(e){err=e;}});
 ws.addEventListener('error',()=>{err=Error('WebSocket error');});
 async function wait(fn,ms=15000){const end=Date.now()+ms;while(!fn()){if(err)throw err;if(Date.now()>end)throw Error('Stage timed out; no command retry');await sleep(50);}}
 const send=m=>ws.send(JSON.stringify(m));
 try{
  await wait(()=>ws.readyState===1);
  send({type:'ssh_connect',host:'10.10.99.98',port:22,user:'rm01',password});
  await wait(()=>connected);await wait(()=>/rm01@agx:.*\$/.test(output));
  const welcome=await snapshot('round'+round+'_welcome');if(pollers(welcome).length!==1)throw Error('Wrong poller count');
  output='';const marker='TIANSHAN_DIAG_VERIFIED_'+round;
  send({type:'ssh_input',data:"echo '"+marker+"'\r"});
  await wait(()=>output.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g,'').split(/\r?\n/).some(l=>l.trim()===marker));
  const active=await snapshot('round'+round+'_input');if(pollers(active).some(t=>t.stack_hwm<2048))throw Error('Insufficient stack margin');
  send({type:'ssh_disconnect'});await wait(()=>closed);
  let after;
  for(let i=0;i<20;i++){after=await snapshot('round'+round+'_closed');if(!pollers(after).length)break;await sleep(100);}
  if(pollers(after).length)throw Error('Poller not reclaimed');
  if(after.info.data.uptime_ms<before.info.data.uptime_ms)throw Error('Device rebooted');
  const bad=faults();if(bad.length)throw Error('Serial fault: '+bad[0]);
  const result={round,minimumObservedHwm:Math.min(...pollers(welcome).map(t=>t.stack_hwm),...pollers(active).map(t=>t.stack_hwm)),free_heap:after.memory.data.free_heap,psram_free:after.memory.data.psram.free,statuses:messages.filter(m=>m.type==='ssh_status').map(m=>m.status),echoVerified:true,pollerGone:true};
  fs.writeFileSync(dir+'/round'+round+'-automatic.json',JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{
  if(connected&&!closed&&ws.readyState===1){send({type:'ssh_disconnect'});await sleep(1000);}
  ws.close();
 }
 await sleep(500);
}
console.log('NINE_AUTOMATIC_ROUNDS_COMPLETE');
