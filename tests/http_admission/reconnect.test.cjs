const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
function fixture(){
 let now=0,id=0;const timers=new Map(),listeners=new Map(),sockets=[];
 const document={visibilityState:'visible',addEventListener(n,f){if(!listeners.has(n))listeners.set(n,new Set());listeners.get(n).add(f);},removeEventListener(n,f){listeners.get(n)?.delete(f);}};
 class Socket{static CONNECTING=0;static OPEN=1;static CLOSING=2;static CLOSED=3;constructor(){this.readyState=0;this.sent=[];sockets.push(this);}send(s){this.sent.push(JSON.parse(s));}open(){this.readyState=1;this.onopen?.();}close(code=1000){this.readyState=3;this.onclose?.({code});}message(x){this.onmessage?.({data:JSON.stringify(x)});}}
 const ctx=vm.createContext({api:{token:'test-token'},WebSocket:Socket,document,window:{location:{protocol:'http:'},addEventListener(){},removeEventListener(){}},getApiHost:()=> 'test-device',console:{log(){},error(){}},setTimeout(f,ms){const n=++id;timers.set(n,{f,t:now+ms});return n;},clearTimeout(n){timers.delete(n);},setInterval:()=>++id,clearInterval(){},t:(key,args)=>key+JSON.stringify(args||{})});
 const api=fs.readFileSync('components/ts_webui/web/js/api.js','utf8');vm.runInContext(api.slice(api.indexOf('class ConnectionRetry'),api.indexOf('// 全局实例'))+';this.Client=TianShanWS;',ctx);
 const terminal=fs.readFileSync('components/ts_webui/web/js/terminal.js','utf8');vm.runInContext(terminal.slice(terminal.indexOf('class WebTerminal'),terminal.indexOf('// 全局终端实例'))+';this.TerminalClient=WebTerminal;',ctx);
 return {ctx,sockets,timers,listeners,document,tick(ms){const end=now+ms;while(true){const due=[...timers].filter(([n,t])=>t.t<=end).sort((a,b)=>a[1].t-b[1].t)[0];if(!due)break;timers.delete(due[0]);now=due[1].t;due[1].f();}now=end;},visible(){document.visibilityState='visible';for(const fn of [...(listeners.get('visibilitychange')||[])])fn();}};
}
test('one socket, brief opens retain backoff, stable connection resets it',()=>{
 const f=fixture(),c=new f.ctx.Client();c.connect();c.connect();assert.equal(f.sockets.length,1);
 f.sockets[0].open();f.sockets[0].close(1006);f.tick(1000);assert.equal(f.sockets.length,2);
 f.sockets[1].open();f.sockets[1].close(1006);f.tick(1999);assert.equal(f.sockets.length,2);f.tick(1);assert.equal(f.sockets.length,3);
 f.sockets[2].open();f.tick(30000);f.sockets[2].close(1006);f.tick(1000);assert.equal(f.sockets.length,4);
});
test('disconnect cancels timers and hidden-page retry; stale callbacks cannot replace current socket',()=>{
 const f=fixture(),c=new f.ctx.Client();c.connect();const old=f.sockets[0];old.open();f.document.visibilityState='hidden';old.close(1006);f.tick(5000);assert.equal(f.sockets.length,1);assert.equal(f.listeners.get('visibilitychange').size,1);
 c.disconnect();f.visible();f.tick(30000);assert.equal(f.sockets.length,1);c.connect();assert.equal(f.sockets.length,2);old.onclose({code:1006});f.tick(30000);assert.equal(c.ws,f.sockets[1]);assert.equal(f.sockets.length,2);
});
test('capacity rejection gives one notice and backs off at least five seconds',()=>{
 const f=fixture(),messages=[],c=new f.ctx.Client(m=>messages.push(m));c.connect();f.sockets[0].open();f.sockets[0].message({type:'connection_busy'});f.sockets[0].close(1013);f.tick(4999);assert.equal(f.sockets.length,1);f.tick(1);assert.equal(f.sockets.length,2);
 f.sockets[1].open();f.sockets[1].message({type:'connection_busy'});f.sockets[1].close(1013);assert.equal(messages.length,1);f.tick(9999);assert.equal(f.sockets.length,2);f.tick(1);assert.equal(f.sockets.length,3);
});
test('terminal canceled retry never reopens after page leave, including hidden retry',()=>{
 const f=fixture(),c=new f.ctx.TerminalClient('terminal'),writes=[];c.writeln=s=>writes.push(s);c.connect();const socket=f.sockets[0];socket.open();socket.message({type:'connection_busy'});socket.close(1013);assert(writes.some(x=>x.includes('connectionBusy')));
 f.document.visibilityState='hidden';f.tick(5000);assert.equal(f.listeners.get('visibilitychange').size,1);c.disconnect();f.visible();f.tick(30000);assert.equal(f.sockets.length,1);c.connect();assert.equal(f.sockets.length,2);socket.onclose({code:1006});f.tick(30000);assert.equal(f.sockets.length,2);
});
