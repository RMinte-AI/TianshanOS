const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');const {root}=require('./harness.cjs');
let browser;before(async()=>{browser=await chromium.launch({channel:'chrome',headless:true});});after(async()=>{await browser?.close();});
for(const lang of ['zh-CN','en-US'])test(`${lang}: cached old API, app and language are replaced by new URLs`,async()=>{
 const index=fs.readFileSync(path.join(root,'index.html'),'utf8');
 const api=fs.readFileSync(path.join(root,'js/api.js'),'utf8'),app=fs.readFileSync(path.join(root,'js/app.js'),'utf8');
 const oldVersion='0.6.1-lpmu2';const newVersion=index.match(/TS_ASSET_VERSION = '([^']+)'/)[1];assert.notEqual(newVersion,oldVersion);
 const oldIndex=index.replace(/(TS_ASSET_VERSION = ')[^']+/,(_,prefix)=>prefix+oldVersion).replace(/(\/js\/(?:api|app)\.js\?v=)[^\"]+/g,(_,prefix)=>prefix+oldVersion);
 const currentUrls={
  '/js/api.js':index.match(/src="(\/js\/api\.js\?[^"]+)"/)[1],
  '/js/app.js':index.match(/src="(\/js\/app\.js\?[^"]+)"/)[1],
  [`/js/lang/${lang}.js`]:`/js/lang/${lang}.js?v=${newVersion}`
 };
 for(const [name,url] of Object.entries(currentUrls))assert.notEqual(url,name+'?v='+oldVersion);
 const oldApi=api.replace(/        const request_id = 'kg-'[\s\S]*?return this.call\('key.generate',[^\n]+/,"        return this.call('key.generate', { id, type, comment, exportable, alias, hidden }, 'POST');");assert.notEqual(oldApi,api);
 const start=app.indexOf('async function generateKey()'),end=app.indexOf('\nfunction formatTimestamp(',start);
 const oldApp=app.slice(0,start)+`async function generateKey() {
  const id=document.getElementById('keygen-id').value.trim();
  requireApiSuccess(await api.keyGenerate(id,document.getElementById('keygen-type').value), 'keyGenerate');
  hideGenerateKeyModal();showToast(t('toast.keyGenerated',{name:id}), 'success');await refreshSecurityPage();
 }
 `+app.slice(end);
 let upgraded=false;const assets=[],posts=[];let keys=[];
 const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://local'),json=data=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify({code:0,data}));};
  if(url.pathname.startsWith('/api/')){
   if(url.pathname==='/api/v1/key/generate'){let body='';req.on('data',x=>body+=x);req.on('end',()=>{const p=JSON.parse(body);posts.push(p);keys.push({id:p.id,type:p.type});json({generated:true,id:p.id,type:p.type});});return;}
   if(url.pathname==='/api/v1/key/list')return json({keys});
   if(url.pathname==='/api/v1/ssh/hosts/list'||url.pathname==='/api/v1/hosts/list')return json({hosts:[]});
   return json({});
  }
  // No Playwright routing: test the device's immutable cache behavior itself.
  res.setHeader('Cache-Control',url.pathname==='/'?'no-cache, no-store, must-revalidate':'public, max-age=604800, immutable');
  if(url.pathname==='/'){res.setHeader('Content-Type','text/html');res.end(upgraded?index:oldIndex);return;}
  if(['/js/api.js','/js/app.js',`/js/lang/${lang}.js`].includes(url.pathname))assets.push(req.url);
  if(url.pathname==='/js/api.js'||url.pathname==='/js/app.js'){res.setHeader('Content-Type','application/javascript');res.end(url.pathname==='/js/api.js'?(upgraded?api:oldApi):(upgraded?app:oldApp));return;}
  const file=path.resolve(root,'.'+url.pathname);if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
  fs.readFile(file,(e,data)=>{if(e){res.writeHead(404).end();return;}res.setHeader('Content-Type',({'.js':'application/javascript','.css':'text/css'})[path.extname(file)]||'application/octet-stream');
   if(!upgraded&&url.pathname===`/js/lang/${lang}.js`)data=Buffer.from(data.toString().replace(/    keyGeneration: \{[\s\S]*?\n\},\n\n/,''));res.end(data);
  });
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const context=await browser.newContext();await context.addInitScript(lang=>{localStorage.setItem('ts_language',lang);class Socket{static OPEN=1;constructor(){this.readyState=0;}send(){}close(){}}window.WebSocket=Socket;},lang);
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
 const visit=async suffix=>{await page.goto(base+suffix);await page.waitForFunction(()=>i18n.isReady());await page.evaluate(async()=>{closeLoginModal();await loadSecurityPage();showGenerateKeyModal();});};
 try{
  await visit('/');assert(!(await page.evaluate(()=>api.keyGenerate.toString())).includes('request_id'));await page.locator('#keygen-id').fill('old');await page.locator('#keygen-submit').click();await page.waitForFunction(()=>document.getElementById('keygen-modal').classList.contains('hidden'));assert(!posts[0].request_id);
  const first=[...assets];await visit('/?cached');assert.deepEqual(assets,first);
  upgraded=true;await visit('/?upgrade');for(const name of ['/js/api.js','/js/app.js',`/js/lang/${lang}.js`]){assert(assets.includes(name+'?v='+oldVersion));assert(assets.includes(currentUrls[name]));}
  await page.locator('#keygen-id').fill('fresh');await page.locator('#keygen-submit').click();await page.waitForFunction(()=>api.keyGenerationTrace?.events.some(e=>e.stage==='ui_done'));assert.match(posts[1].request_id,/^kg-/);assert((await page.locator('#keys-table-body').textContent()).includes('fresh'));assert.deepEqual(errors,[]);
 }finally{await context.close();await new Promise(r=>server.close(r));}
});
