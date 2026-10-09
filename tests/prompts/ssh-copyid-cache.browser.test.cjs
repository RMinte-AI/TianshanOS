const {test,before,after}=require('node:test');
const assert=require('node:assert/strict');
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('playwright');
const {root}=require('./harness.cjs');
let browser;
before(async()=>{browser=await chromium.launch({channel:'chrome',headless:true});});
after(async()=>{await browser?.close();});

for(const language of ['zh-CN','en-US']) {
 test(`${language}: upgrade replaces a cached old deployment API without clearing browser cache`,async()=>{
  const index=fs.readFileSync(path.join(root,'index.html'),'utf8');
  const api=fs.readFileSync(path.join(root,'js/api.js'),'utf8');
  const oldApi=api.replace(/(async sshCopyid[\s\S]*?trust_new: options.trust_new \?\? )true/, '$1false');
  assert.notEqual(oldApi,api);
  const oldIndex=index.replace(/(\/js\/api\.js\?v=)[^"]+/,(_,prefix)=>prefix+'0.6.0');
  const currentUrl=index.match(/src="(\/js\/api\.js\?[^"]+)"/)[1];
  assert.notEqual(currentUrl,'/js/api.js?v=0.6.0');
  let upgraded=false,deployed=false;
  const assetRequests=[],deployRequests=[],errors=[];
  const host={id:'fixture@192.0.2.99',host:'192.0.2.99',port:22,username:'fixture',keyid:'fixture-key'};
  const server=http.createServer((req,res)=>{
   const url=new URL(req.url,'http://local');
   const json=(code,data,message='')=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify({code,data,message}));};
   if(url.pathname.startsWith('/api/')) {
    if(url.pathname==='/api/v1/ssh/copyid') {
     let body='';req.setEncoding('utf8');req.on('data',part=>body+=part);req.on('end',()=>{
      const params=JSON.parse(body);deployRequests.push(params);
      if(params.trust_new!==true)return json(1002,{status:'new_host',fingerprint:'first'},'New host requires confirmation');
      deployed=true;json(0,{deployed:true,verified:true,registered:true,host_id:host.id});
     });return;
    }
    if(url.pathname==='/api/v1/ssh/hosts/list')return json(0,{hosts:deployed?[host]:[]});
    if(url.pathname==='/api/v1/hosts/list')return json(0,{hosts:[]});
    if(url.pathname==='/api/v1/key/list')return json(0,{keys:[]});
    return json(0,{});
   }
   // Same cache rules as the device. No Playwright route interception: it disables HTTP caching.
   res.setHeader('Cache-Control',url.pathname==='/'?'no-cache, no-store, must-revalidate':'public, max-age=604800, immutable');
   if(url.pathname==='/'){res.setHeader('Content-Type','text/html');res.end(upgraded?index:oldIndex);return;}
   if(url.pathname==='/js/api.js') {
    assetRequests.push(req.url);res.setHeader('Content-Type','application/javascript');res.end(upgraded?api:oldApi);return;
   }
   const file=path.resolve(root,'.'+url.pathname);
   if(!file.startsWith(root+'/')){res.writeHead(403).end();return;}
   fs.readFile(file,(error,data)=>{
    if(error){res.writeHead(404).end();return;}
    res.setHeader('Content-Type',({'.js':'application/javascript','.css':'text/css','.woff2':'font/woff2','.svg':'image/svg+xml'})[path.extname(file)]||'application/octet-stream');
    res.end(data);
   });
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base=`http://127.0.0.1:${server.address().port}`;
  const context=await browser.newContext({locale:language});
  await context.addInitScript(language=>{
   localStorage.setItem('ts_language',language);
   class Socket {static OPEN=1;static CONNECTING=0;constructor(){this.readyState=0;}send(){}close(){this.readyState=3;}}
   window.WebSocket=Socket;
  },language);
  const page=await context.newPage();page.on('pageerror',error=>errors.push(error.message));
  const openDeploy=async(url)=>{
   await page.goto(url);await page.waitForFunction(()=>i18n.isReady());
   await page.evaluate(async()=>{closeLoginModal();await loadSecurityPage();showDeployKeyModal('fixture-key');});
   await page.locator('#deploy-host').fill(host.host);await page.locator('#deploy-user').fill(host.username);
   await page.locator('#deploy-password').fill('fixture-password');
  };
  try {
   await openDeploy(base);await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('error'));
   assert.equal(deployRequests[0].trust_new,false);
   await openDeploy(base+'/?cached');
   assert.deepEqual(assetRequests,['/js/api.js?v=0.6.0']); // Prove the old file is served from cache.
   upgraded=true;await openDeploy(base+'/?upgraded');await page.locator('#deploy-btn').click();
   await page.waitForFunction(()=>document.getElementById('deploy-result').classList.contains('success'));
   assert.deepEqual(assetRequests,['/js/api.js?v=0.6.0',currentUrl]);
   assert.equal(deployRequests.length,2);assert.equal(deployRequests[1].trust_new,true);
   assert.equal(deployRequests[1].accept_changed,false);
   assert.equal(await page.locator('.confirm-sheet').count(),0);
   assert((await page.locator('#ssh-hosts-table-body').textContent()).includes(host.id));
   assert.deepEqual(errors,[]);
  } finally {await context.close();await new Promise(resolve=>server.close(resolve));}
 });
}
