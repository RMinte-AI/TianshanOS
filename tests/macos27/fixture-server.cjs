// Development-only visual fixture. Never packaged in the device WebUI.
// Every API request terminates here; no proxy or device connection exists.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const populated = {...require('./profiles.cjs'), ...(process.argv[5] ? JSON.parse(fs.readFileSync(process.argv[5], 'utf8')) : {})};
const root = path.resolve(process.argv[2]);
const port = Number(process.argv[3] || 8765);
const log = process.argv[4];
const observation = fs.readFileSync(path.join(__dirname,'observation.js'),'utf8');
const network = {eth:{connected:true,link_up:true,ip:'192.0.2.10',netmask:'255.255.255.0',gateway:'192.0.2.1',dns:'192.0.2.1',mac:'02:00:00:00:00:01'},wifi:{connected:false,ssid:'macOS27 fixture'},ap:{enabled:true,ssid:'macOS27 fixture AP',ip:'192.0.2.20',channel:6,clients:0}};
const data = {
 'auth/status':{valid:true,username:'root',level:'root'},
 'system/info':{chip:{model:'ESP32-S3 (fixture)',cores:2},app:{version:'BASELINE-FIXTURE',idf_version:'5.5.2'},uptime_ms:3600000},
 'system/memory':{internal:{total:327680,free:196608},psram:{total:8388608,free:6291456}},
 'system/cpu':{cores:[{usage:18},{usage:22}]},
 'time/info':{year:2026,synced:true,source:'ntp',timezone:'CST-8'},
 'network/status':network,
 'network/eth/status':network.eth,
 'network/wifi/scan':{networks:[{ssid:'macOS27 fixture',rssi:-52,authmode:3,channel:6}]},
 'network/wifi/ap/stations':{stations:[]},
 'network/lpmu_access/status':{state:'idle'},
 'dhcp/status':{running:true,enabled:true,clients:0},'dhcp/clients':{clients:[]},
 'nat/status':{enabled:false},
 'power/status':{power_chip:{voltage_v:19.2,current_a:0.42,power_w:8.064},voltage:{supply_v:20.2}},
 'power/protection/status':{initialized:true,running:true,state:'normal'},
 'power/protection/config':{low_voltage:12.6,recovery_voltage:18,shutdown_delay:60,recovery_hold:5,fan_stop_delay:30},
 'fan/status':{fans:[0,1].map(id=>({id,mode:'auto',duty:40,rpm:1840,temperature:42,auto_state:'baseline',guard_temperature:44,predicted_temperature:43,slope_c_per_min:0.1}))},
 'service/list':{services:[{name:'http',state:'RUNNING',healthy:true},{name:'led',state:'RUNNING',healthy:true}]},
 'led/list':{devices:['board','touch','matrix'].map(name=>({name,brightness:128,count:name==='matrix'?256:1,layout:name==='matrix'?'matrix':'strip',width:16,height:16,current:{on:true,color:{r:255,g:136,b:0}}}))},
 'led/effect/list':{effects:[]},'led/filter/list':{filters:[]},
 'ui/widgets/get':{widgets:[],refresh_interval:3000},
 'device/usb/status':{target:'agx'},'device/status':{power:true,state:'running',devices:[]},'device/ping':{success:true,reachable:true},
 'storage/status':{sd:{mounted:true,total:1073741824,used:1048576},spiffs:{mounted:true,total:2097152,used:1048576}},
 'storage/list':{entries:[{name:'fixture.txt',type:'file',size:1024},{name:'images',type:'dir',size:0}]},
 'automation/status':{state:'running',rules_count:0,sources_count:0,variables_count:0,uptime_ms:3600000},
 'automation/rules/list':{rules:[],loaded:true},'automation/sources/list':{sources:[],loaded:true},'automation/actions/list':{actions:[],loaded:true},'automation/variables/list':{variables:[]},
 'ssh/commands/list':{commands:[]},'ssh/hosts/list':{hosts:[]},'ssh/known_hosts/list':{hosts:[]},
 'key/list':{keys:[]},'ssh/key/list':{keys:[]},
 'ota/version':{version:'BASELINE-FIXTURE'},'ota/status':{state:'idle'},
 'ota/server/get':{url:'https://fixture.invalid/firmware'},
 'ota/progress':{state:'idle',progress:0},
 'ota/partitions':{running:{label:'ota_0',address:65536,size:3145728,version:'FIXTURE-A'},next:{label:'ota_1',address:3211264,size:3145728,version:'FIXTURE-B',is_bootable:true},can_rollback:true},
 'cert/status':{has_private_key:false,has_certificate:false,has_ca_chain:false,validity:'missing',https:{running:false,port:443}},
 'hosts/list':{hosts:[]},
 'temp/status':{temperature_c:42,valid:true,source:'variable'},
 'temp/bind':{bound_variables:[{name:'fixture.value',weight:1,value:42,valid:true}]},
 'fan/config':{curve:[{temp:30,duty:20},{temp:60,duty:70}],hysteresis:2,min_interval:1000,min_duty:20,max_duty:100},
 'led/color_correction/get':{enabled:true,white_point:{red_scale:1,green_scale:1,blue_scale:1},gamma:{gamma:2.2}},
 'config/get':{value:1},
 'config/pack/export_cert':{fingerprint:'SHA256:fixture-only',cn:'fixture.invalid',certificate:'LOCAL FIXTURE — NOT A USABLE CERTIFICATE'},
 'config/pack/list':{files:[{name:'fixture.tscfg',size:1024,signer:'fixture-only',is_official:false,valid:true}]},
};
network.ethernet={...network.eth,status:'connected'};
network.wifi_sta=network.wifi;network.wifi_ap=network.ap;
const harness = `<script>
// Explicit synthetic session, only injected by this local fixture server.
const fixtureQuery=new URLSearchParams(location.search);
const fixtureRole=fixtureQuery.get('role')||'root';
if(fixtureQuery.has('lang'))localStorage.setItem('ts_language',fixtureQuery.get('lang'));
if(fixtureRole!=='guest'){localStorage.setItem('ts_token','local-fixture-'+fixtureRole);localStorage.setItem('ts_username',fixtureRole);localStorage.setItem('ts_level',fixtureRole);localStorage.setItem('ts_expires',String(Date.now()+86400000));}else{['ts_token','ts_username','ts_level','ts_expires'].forEach(k=>localStorage.removeItem(k));}
${observation}
window.WebSocket=class {
 static OPEN=1; static CONNECTING=0;
 constructor(url) {
  this.url=url;this.readyState=0;this.sent=[];
  if(window.__macos27Observation){window.__macos27Observation.sockets.created++;window.__macos27Observation.sockets.active++;}
  setTimeout(()=>{this.readyState=1;this.onopen?.();},0);
 }
 emit(message){setTimeout(()=>{if(this.readyState===1)this.onmessage?.({data:JSON.stringify(message)});},0);}
 send(raw){
  const message=JSON.parse(raw); this.sent.push(message);
  if(message.topic==='system.dashboard'&&message.type==='subscribe')this.emit({type:'data',topic:message.topic,data:{cpu:{cores:[{id:0,usage:18},{id:1,usage:22}],total_usage:20}}});
  else if(message.type==='terminal_start')this.emit({type:'connected',prompt:'fixture> '});
  else if(message.type==='log_get_history')this.emit({type:'log_history',logs:fixtureQuery.has('logStress')?Array.from({length:20},(_,i)=>({timestamp:1790611200000+i*1000,level:i%5+1,tag:'fixture-tag-'+('long-'.repeat(12)),task:'fixture-task-'+('long-'.repeat(20)),message:'message '+i+' · 可读日志内容 '.repeat(10)+' https://example.invalid/'+('path'.repeat(20))})):[1,2,3,4,5].map(level=>({level,tag:'fixture',message:'Local synthetic log level '+level,timestamp:1790611200000,task:'fixture-task'}))});
  else if(message.type==='ping')this.emit({type:'pong'});
  else if(message.type==='terminal_input'){
   this.emit({type:'output',data:'LOCAL FIXTURE OUTPUT: '+message.data});this.emit({type:'done'});
  }
 }
 close(){if(window.__macos27Observation&&!this.fixtureClosed){window.__macos27Observation.sockets.closed++;window.__macos27Observation.sockets.active--;this.fixtureClosed=true;}this.readyState=3;}
};
if(fixtureQuery.has('measure')){
 const longs=[];new PerformanceObserver(list=>longs.push(...list.getEntries().map(e=>({start:e.startTime,duration:e.duration})))).observe({type:'longtask',buffered:true});
 let ready=null;const observer=new MutationObserver(()=>{const selectors={'/':'#brightness-matrix','/network':'#net-eth-ip','/files':'#file-list tr','/terminal':'.xterm','/automation':'#actions-list p','/commands':'#commands-list','/security':'#cert-status-text','/ota':'#ota-current-version'};const selector=selectors[location.hash.slice(1)||'/'];if(ready===null&&selector&&document.querySelector(selector)){ready=performance.now();}});observer.observe(document.documentElement,{childList:true,subtree:true});
 addEventListener('load',()=>setTimeout(()=>{observer.disconnect();const result={userAgent:navigator.userAgent,viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio},ready,paints:performance.getEntriesByType('paint').map(e=>({name:e.name,start:e.startTime})),longs,heap:performance.memory?.usedJSHeapSize,resources:performance.getEntriesByType('resource').map(e=>({name:e.name,encoded:e.encodedBodySize,decoded:e.decodedBodySize,transfer:e.transferSize,duration:e.duration})),navigation:performance.getEntriesByType('navigation').map(e=>({encoded:e.encodedBodySize,decoded:e.decodedBodySize,transfer:e.transferSize,duration:e.duration}))};const output=document.createElement('pre');output.style.cssText='position:fixed;bottom:0;left:0;width:300px;height:20px;overflow:auto;z-index:99999;background:white;color:black;font-size:10px';output.id='fixture-metrics';output.textContent=JSON.stringify(result);document.body.append(output);},3000));
}
</script>`;
http.createServer((req,res)=>{
 const url=new URL(req.url,'http://localhost');
 let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{
  if(log)fs.appendFileSync(log,JSON.stringify({time:Date.now(),method:req.method,path:url.pathname,query:url.search,body})+'\n');
  if(url.pathname.startsWith('/api/')){
   const endpoint=url.pathname.replace('/api/v1/','');
   res.setHeader('Content-Type','application/json');
   if(/macos27_fixture=(populated|provisioned)/.test(req.headers.cookie||'') && populated[endpoint]){res.end(JSON.stringify({code:0,data:populated[endpoint]}));return;}
   if(req.headers.cookie?.includes('macos27_fixture=provisioned')){
    const provisioned={'cert/status':{has_private_key:true,has_certificate:true,has_ca_chain:true,validity:'valid',https:{running:true,port:443},cert_info:{subject_cn:'fixture.invalid',issuer_cn:'Fixture CA',validity:'valid'}},'cert/get_certificate':{cert_pem:'FIXTURE ONLY — not a usable certificate'},'config/pack/info':{can_export:true,device_type:'Developer',cert_cn:'fixture.invalid',pack_version:1}};
    if(provisioned[endpoint]){res.end(JSON.stringify({code:0,data:provisioned[endpoint]}));return;}
   }
   if(['automation/sources/import','automation/rules/import','automation/actions/import','ssh/commands/import','ssh/hosts/import'].includes(endpoint)){
    const input=JSON.parse(body||'{}');
    if(input.preview===true){const valid=!String(input.tscfg).includes('INVALID');res.end(JSON.stringify({code:0,message:valid?'Synthetic preview only':'Synthetic invalid signature',data:{valid,id:'fixture-import',signer:'LOCAL SYNTHETIC SIGNER',official:false,exists:true,note:'UI branch fixture only; no cryptographic verification'}}));return;}
   }
   if(endpoint==='auth/status'){const token=JSON.parse(body||'{}').token||'';const role=token.endsWith('admin')?'admin':'root';res.end(JSON.stringify({code:0,data:{valid:true,username:role,level:role,password_changed:true}}));return;}
   if(endpoint==='auth/login'){const input=JSON.parse(body||'{}');res.end(JSON.stringify({code:0,data:{token:'local-fixture-'+(input.username==='root'?'root':'admin'),username:input.username,level:input.username==='root'?'root':'admin',password_changed:true,expires_in:86400}}));return;}
   if(endpoint==='auth/logout'){res.end(JSON.stringify({success:true}));return;}
   if(Object.hasOwn(data,endpoint)){res.end(JSON.stringify({code:0,data:data[endpoint]}));return;}
   if(log)fs.appendFileSync(log,JSON.stringify({unknown:true,method:req.method,endpoint})+'\n');
   res.writeHead(501);res.end(JSON.stringify({code:'NOT_SUPPORTED',message:'Unspecified local fixture endpoint: '+endpoint}));return;
  }
  const file=path.resolve(root,'.'+(url.pathname==='/'?'/index.html':url.pathname));
  if(url.searchParams.has('state'))res.setHeader('Set-Cookie','macos27_fixture='+url.searchParams.get('state')+'; Path=/; SameSite=Strict');
  if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  try{let content=fs.readFileSync(file);if(file.endsWith('/index.html'))content=Buffer.from(content.toString().replace('<head>','<head>'+harness));
   res.setHeader('Content-Type',({'.html':'text/html','.js':'application/javascript','.css':'text/css','.woff2':'font/woff2','.png':'image/png','.ico':'image/x-icon'})[path.extname(file)]||'application/octet-stream');
   if(fs.existsSync(file+'.gz')&&req.headers['accept-encoding']?.includes('gzip')){content=file.endsWith('/index.html')?zlib.gzipSync(content,{level:9}):fs.readFileSync(file+'.gz');res.setHeader('Content-Encoding','gzip');}
   res.end(content);
  }catch{res.writeHead(404).end();}
 });
}).listen(port,'127.0.0.1',()=>console.log(`Fixture only: http://127.0.0.1:${port}; source ${root}`));
