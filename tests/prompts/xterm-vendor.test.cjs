const {test}=require('node:test');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),crypto=require('node:crypto'),zlib=require('node:zlib'),{execFileSync}=require('node:child_process');
test('fixed vendor integrity, skip minify, gzip identity, local loader only',()=>{
 const root='components/ts_webui/web',records=JSON.parse(fs.readFileSync('docs/repair/xterm-local/vendor.json'));
 assert.equal(records.length,5);assert.deepEqual(fs.readdirSync(root+'/vendor/xterm').sort(),records.map(r=>r.file).sort());
 const temp=fs.mkdtempSync(path.join(os.tmpdir(),'ts-xterm-'));
 try{
  fs.cpSync(root+'/vendor',temp+'/vendor',{recursive:true});
  fs.writeFileSync(temp+'/own.js','// own comment\n  const a = 1;\n');
  execFileSync('python3',['tools/minify_web.py',temp,'--gzip']);
  assert(!fs.readFileSync(temp+'/own.js','utf8').includes('own comment'));
  for(const r of records){const source=fs.readFileSync(root+'/vendor/xterm/'+r.file),built=fs.readFileSync(temp+'/vendor/xterm/'+r.file);
   assert.equal(source.length,r.bytes);assert.equal(crypto.createHash('sha256').update(source).digest('hex'),r.sha256);assert.deepEqual(built,source);
   if(/\.(js|css)$/.test(r.file))assert.deepEqual(zlib.gunzipSync(fs.readFileSync(temp+'/vendor/xterm/'+r.file+'.gz')),source);
   else assert(source.toString().includes('Permission is hereby granted'));
  }
  const js=fs.readFileSync(root+'/js/terminal.js','utf8');assert(!js.includes('cdn.jsdelivr.net'));assert(js.includes('encodeURIComponent(window.TS_ASSET_VERSION)'));
 }finally{fs.rmSync(temp,{recursive:true,force:true});}
});
test('production lazy loader: same-origin cold load, shared promise and retry',async()=>{
 const vm=require('node:vm'),nodes=[],requests=[];let fail=true;
 const context=vm.createContext({window:{TS_ASSET_VERSION:'test key'},setTimeout,clearTimeout,document:{
  querySelector(selector){return nodes.find(n=>selector===`${n.kind==='link'?'link[href':'script[src'}="${n.href||n.src}"]`)||null;},
  createElement(kind){return {kind,dataset:{},remove(){const i=nodes.indexOf(this);if(i>=0)nodes.splice(i,1);}};},
  head:{appendChild(n){nodes.push(n);requests.push(n.href||n.src);queueMicrotask(()=>fail?n.onerror():n.onload());}}
 }});
 vm.runInContext(fs.readFileSync('components/ts_webui/web/js/terminal.js','utf8'),context);
 assert.equal(requests.length,0);
 await assert.rejects(vm.runInContext('_xtermReady()',context),/Terminal resource unavailable/);
 fail=false;
 assert(vm.runInContext('_xtermReady() === _xtermReady()',context));await vm.runInContext('_xtermReady()',context);
 assert.equal(requests.length,4);assert.equal(nodes.length,3);
 assert(requests.every(u=>u.startsWith('/vendor/xterm/')&&u.endsWith('?v=test%20key')));
 await vm.runInContext('_xtermReady()',context);assert.equal(requests.length,4);
});
