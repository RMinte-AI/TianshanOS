// Capture final-build text over its actual composited background. Local fixture only.
// This collects text samples; icons, control boundaries and placeholder/value text
// require separate evidence. It does not assign whole-page accessibility PASS.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const root = 'output/macos27-20260929-native-rework';
const out = process.argv[2];
if (!out || !out.startsWith(root + '/') || fs.existsSync(out)) throw Error('New evidence directory required');
fs.mkdirSync(out, {recursive: true});
const sourceMode = process.argv.includes('--source');
const buildName = process.argv.find(x => x.startsWith('--build='))?.slice(8) || 'build-v2';
const port = process.argv.find(x => x.startsWith('--port='))?.slice(7) || (sourceMode ? '18783' : '18791');
if (!/^build(?:-v\d+)?$/.test(buildName) || !/^\d{4,5}$/.test(port)) throw Error('Invalid local build or port');
if (sourceMode && process.argv.some(x => x.startsWith('--build='))) throw Error('Choose source or build');
const origin = 'http://127.0.0.1:' + port;
const web = sourceMode ? 'components/ts_webui/web' : root + '/' + buildName + '/web_optimized';
const selected = process.argv.find(x => x.startsWith('--surfaces='))?.slice(11).split(',');
const modes = process.argv.find(x => x.startsWith('--modes='))?.slice(8).split(',') || ['normal'];
if (!modes.length || new Set(modes).size !== modes.length || modes.some(x => !['normal','contrast','transparency','motion','no-filter'].includes(x))) throw Error('Invalid text-sampling mode; forced colors require a separate masking method');
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const identity = () => Object.fromEntries(['index.html','css/style.css','js/app.js'].map(p => [p,hash(web+'/'+p)]));
const result = {started:new Date().toISOString(),origin,web,before:identity(),rows:[],errors:[],completed:false,
    method:'DPR 1 CSS-pixel screenshots; text-fill-only masking within inspected surface; per-text-node visible client rectangles. Non-unit ancestor opacity or filters are not accepted by analyzer.'};
const save = () => fs.writeFileSync(path.join(out,'raw.json'),JSON.stringify(result,null,2));
const scenarios = [
    ['system','/','#page-content'], ['network','/network','#page-content'],
    ['files','/files','#page-content'], ['automation','/automation','#page-content'],
    ['security','/security','#page-content'],
    ['ota','/ota','#page-content'], ['commands','/commands','#page-content'],
    ['terminal','/terminal','#page-content'],
    ['logs','/logs','#terminal-logs-modal .modal-content'],
    ['timezone','/','#timezone-modal .modal-content','[onclick="showTimezoneModal()"]'],
    ['memory','/','#memory-detail-modal .modal-content','[onclick="showMemoryDetailModal()"]'],
    ['keygen','/security','#keygen-modal .modal-content','[onclick="showGenerateKeyModal()"]'],
    ['widget-ring','/','.dw-manager-modal','[onclick="showWidgetManager()"]',`[onclick="showWidgetEditPanel('fixture-ring')"]`]
];
if (selected && selected.some(name => !scenarios.some(s => s[0] === name))) throw Error('Unknown surface');
const activeScenarios = selected ? scenarios.filter(s => selected.includes(s[0])) : scenarios;
result.scope = {sourceMode, surfaces: activeScenarios.map(s => s[0]), modes,
    fallbackMethod:'Isolated Chromium media emulation. no-filter disables the existing supports block in served HTML/CSS only; not a genuine old-engine test. No OS preferences changed.'};
async function metadata(surface) {
    return surface.evaluate(root => {
        const rows=[];
        const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);
        const intersect=(a,b)=>({x:Math.max(a.x,b.x),y:Math.max(a.y,b.y),right:Math.min(a.right,b.right),bottom:Math.min(a.bottom,b.bottom)});
        let n;
        while((n=walker.nextNode())) {
            if(!n.textContent.trim())continue;
            const e=n.parentElement;
            if(e.closest('script,style,option,svg'))continue;
            const s=getComputedStyle(e);
            if(s.visibility!=='visible'||s.display==='none')continue;
            const range=document.createRange();range.selectNodeContents(n);
            let opacity=1;const unsupported=[];const clipping=[];
            for(let a=e;a;a=a.parentElement){
                const c=getComputedStyle(a);opacity*=Number(c.opacity);
                if(c.filter!=='none')unsupported.push('filter:'+c.filter);
                if(c.mixBlendMode!=='normal')unsupported.push('blend:'+c.mixBlendMode);
                const b=a.getBoundingClientRect();
                clipping.push({x:['hidden','clip','scroll','auto'].includes(c.overflowX)?b.left:0,
                    right:['hidden','clip','scroll','auto'].includes(c.overflowX)?b.right:innerWidth,
                    y:['hidden','clip','scroll','auto'].includes(c.overflowY)?b.top:0,
                    bottom:['hidden','clip','scroll','auto'].includes(c.overflowY)?b.bottom:innerHeight});
            }
            if(opacity===0)continue;
            const rects=[...range.getClientRects()].map(r=>{
                let b=intersect(r,{x:0,y:0,right:innerWidth,bottom:innerHeight});
                for(const clip of clipping)b=intersect(b,clip);
                return {x:b.x,y:b.y,width:b.right-b.x,height:b.bottom-b.y};
            }).filter(r=>{
                if(r.width<=1||r.height<=1)return false;
                // Exclude lines whose center is covered by a fixed header/overlay.
                const hit=document.elementFromPoint(r.x+r.width/2,r.y+r.height/2);
                return hit && (e===hit||e.contains(hit));
            });
            if(!rects.length)continue;
            const partlyOccluded=rects.some(r=>[
                [r.x+.5,r.y+.5],[r.x+r.width-.5,r.y+.5],
                [r.x+.5,r.y+r.height-.5],[r.x+r.width-.5,r.y+r.height-.5]
            ].some(([x,y])=>{const hit=document.elementFromPoint(x,y);return !hit||!(e===hit||e.contains(hit));}));
            if(partlyOccluded)unsupported.push('Partial text occlusion: rectangle contains another surface');
            rows.push({text:n.textContent.trim(),tag:e.tagName,id:e.id,classes:e.className,
                color:s.webkitTextFillColor||s.color,fontSize:parseFloat(s.fontSize),fontWeight:s.fontWeight,
                opacity,unsupported,disabled:!!e.closest(':disabled,[aria-disabled="true"]'),rects});
        }
        const c=getComputedStyle(root);
        return {text:rows,viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio},
            scroll:{page:scrollY,surface:root.scrollTop},material:{background:c.backgroundColor,filter:c.backdropFilter},
            media:{contrast:matchMedia('(prefers-contrast: more)').matches,transparency:matchMedia('(prefers-reduced-transparency: reduce)').matches,motion:matchMedia('(prefers-reduced-motion: reduce)').matches},
            controls:[...root.querySelectorAll('button')].map(e=>({text:e.textContent.trim(),classes:e.className,transition:getComputedStyle(e).transitionDuration}))};
    });
}
(async()=>{
    // The same built bytes must be served, including the current focus fix.
    for(const p of ['css/style.css','js/app.js']){
        const response=await fetch(origin+'/'+p);if(!response.ok)throw Error('HTTP '+response.status);
        if(!Buffer.from(await response.arrayBuffer()).equals(fs.readFileSync(web+'/'+p)))throw Error('Artifact mismatch '+p);
    }
    const browser=await chromium.launch({channel:'chrome',headless:true});result.browser=browser.version();
    try{
        for(const mode of modes)for(const width of [390,1440])for(const language of ['zh-CN','en-US'])for(const [name,route,selector,trigger,secondaryTrigger]of activeScenarios){
            const page=await browser.newPage({viewport:{width,height:900},deviceScaleFactor:1});
            page.on('pageerror',e=>result.errors.push({name,width,language,error:String(e)}));
            try{
                const feature={contrast:['prefers-contrast','more'],transparency:['prefers-reduced-transparency','reduce'],motion:['prefers-reduced-motion','reduce']}[mode];
                if(feature){const cdp=await page.context().newCDPSession(page);await cdp.send('Emulation.setEmulatedMedia',{features:[{name:feature[0],value:feature[1]}]});}
                let supportsReplacements=0;
                await page.route('**/*',async r=>{
                    const request=r.request(),url=new URL(request.url());
                    if(url.origin!==origin || (request.method()!=='GET' && !['/api/v1/auth/status','/api/v1/device/ping'].includes(url.pathname))){
                        result.errors.push({blockedRequest:request.method()+' '+url.href});return r.abort();
                    }
                    if(mode!=='no-filter'||!['document','stylesheet'].includes(request.resourceType()))return r.continue();
                    const response=await r.fetch(),body=await response.text();
                    const condition='@supports ((backdrop-filter: blur(1px)) or (-webkit-backdrop-filter: blur(1px)))';
                    supportsReplacements+=body.split(condition).length-1;
                    await r.fulfill({response,body:body.replaceAll(condition,'@supports (codex-unsupported-property: 1)')});
                });
                await page.goto(origin+'/?state=populated&lang='+language+'#'+route);
                await page.locator('#page-content').waitFor();
                await page.evaluate(()=>document.fonts.ready);
                if(trigger)await page.locator(trigger).first().click();
                if(secondaryTrigger)await page.locator(secondaryTrigger).first().click();
                const surface=page.locator(selector).first();await surface.waitFor();
                await page.waitForTimeout(350);
                for(const position of [0,0.5,1]){
                    await surface.evaluate((e,p)=>{
                        const target=e.matches('#page-content')?document.scrollingElement:
                            [...e.querySelectorAll('*'),e].find(x=>x.scrollHeight>x.clientHeight+2&&['auto','scroll'].includes(getComputedStyle(x).overflowY));
                        if(target)target.scrollTop=p*(target.scrollHeight-target.clientHeight);
                    },position);
                    await page.waitForTimeout(200);
                    const data=await metadata(surface);if(!data.text.length)throw Error('No visible text '+name);
                    if(feature&&!data.media[mode])throw Error('Media emulation did not apply: '+mode);
                    if(mode==='no-filter'&&!supportsReplacements)throw Error('No supports block disabled');
                    if(['contrast','transparency','no-filter'].includes(mode)&&data.material.filter!=='none')throw Error('Fallback still filters: '+mode);
                    if(mode==='motion'&&data.controls.some(c=>c.transition.split(',').some(t=>parseFloat(t)!==0))){result.errors.push({mode,name,width,language,state:data});throw Error('Reduced-motion button transition remains');}
                    const id=[...(mode==='normal'?[]:[mode]),name,language,width,position].join('-');
                    await page.screenshot({path:out+'/'+id+'.png',animations:'disabled'});
                    await surface.evaluate(e=>e.setAttribute('data-composed-text-scope',''));
                    await page.screenshot({path:out+'/'+id+'-background.png',animations:'disabled',
                        style:'[data-composed-text-scope], [data-composed-text-scope] * { -webkit-text-fill-color: transparent !important; text-shadow: none !important; }'});
                    await surface.evaluate(e=>e.removeAttribute('data-composed-text-scope'));
                    // Removing the temporary text mask can trigger the existing
                    // control transition. Observe the restored presentation,
                    // not its transient transparent text-fill frame.
                    await page.waitForTimeout(350);
                    const after=await metadata(surface);
                    // Reject moving/dynamic text samples rather than compare mismatched regions.
                    const stable=data.text.length===after.text.length;
                    data.text.forEach((text,index)=>{
                        text.stable=stable&&JSON.stringify(text)===JSON.stringify(after.text[index]);
                    });
                    result.rows.push({id,name,language,width,position,selector,mode,supportsReplacements,stable,...data,
                        unstableSamples:data.text.flatMap((text,index)=>text.stable?[]:[{index,after:after.text[index]||null}]),
                        screenshots:{normal:hash(out+'/'+id+'.png'),background:hash(out+'/'+id+'-background.png')}});save();
                }
            }finally{await page.close();}
        }
        result.after=identity();result.configurationUnchanged=JSON.stringify(result.before)===JSON.stringify(result.after);
        if(!result.configurationUnchanged||result.errors.length)throw Error('Identity changed or runtime/request errors occurred');
        result.completed=true;save();
    }catch(e){result.errors.push({fatal:String(e)});save();throw e;}finally{await browser.close();}
})().catch(e=>{result.errors.push({topLevel:String(e)});save();console.error(e);process.exitCode=1;});
