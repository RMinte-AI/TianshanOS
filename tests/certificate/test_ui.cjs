const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync('components/ts_webui/web/js/app.js','utf8');
function fn(name) {
    // Use complete production functions, bounded by the next top-level declaration.
    const match = new RegExp('^(?:async )?function '+name+'\\(', 'm').exec(source);
    assert(match, name);
    const rest = source.slice(match.index);
    const next = /\n(?:async )?function \w+\(/.exec(rest);
    return next ? rest.slice(0,next.index) : rest;
}
const translations = {};
for (const lang of ['zh-CN','en-US']) vm.runInNewContext(fs.readFileSync(`components/ts_webui/web/js/lang/${lang}.js`,'utf8'), {i18n:{registerLanguage:(id,d)=>translations[id]=d}});
let locale = 'zh-CN';
const elements = new Map();
function el(id) {if (!elements.has(id)) elements.set(id,{textContent:'',value:'test PEM',disabled:false,style:{},classList:{add(){},remove(){}}});return elements.get(id);}
let posts=0, refreshFail=false, response={code:0}, reject=false;
const ctx = vm.createContext({console, setTimeout:()=>{}, document:{getElementById:el}, window:{},
    t:(key,params={})=>{let v=key.split('.').reduce((o,k)=>o?.[k],translations[locale])??key;return v.replace(/\{(\w+)\}/g,(_,k)=>params[k]??`{${k}}`);},
    api:{certInstall:async()=>{posts++;if(reject)throw new Error('timeout');return response;},certInstallCA:async()=>{posts++;return response;}},
    refreshCertStatus:async()=>{if(refreshFail)throw new Error('refresh failed');}
});
for(const name of ['certText','certError','certValidity','certExpiry','installCertificate','installCAChain','installCertMaterial'])vm.runInContext(fn(name),ctx);
(async()=>{
    assert.equal(ctx.certError({message:'M',error:'E'},'F'),'M');
    assert.equal(ctx.certError({message:' ',error:'E'},'F'),'E');
    assert.equal(ctx.certError({error:true},'F'),'F');
    assert.equal(ctx.certText('missing.key','fallback'),'fallback');
    for(locale of ['zh-CN','en-US']) {
        for(const key of ['none','invalid','time_unverified','not_yet_valid','valid','expired']) assert(!ctx.certValidity(key).includes('pkiRepair.'));
        assert(ctx.certValidity(undefined));
        assert.equal(ctx.certExpiry({is_valid:true,days_until_expiry:20}), ''); // old firmware
        assert.equal(ctx.certExpiry({validity:'time_unverified',seconds_until_expiry:null}), '');
        assert(!ctx.certExpiry({validity:'expired',seconds_until_expiry:-1}).includes('pkiRepair.'));
        response={code:7,error:'<img src=x onerror=alert(1)>'};await ctx.installCertificate();
        assert(el('cert-install-result').textContent.includes('<img'));assert(!el('cert-install-submit').disabled);
        response={code:0};refreshFail=true;await ctx.installCertificate();assert(!el('cert-install-submit').disabled);
        assert(!el('cert-install-result').textContent.includes('failed'));
        reject=true;const before=posts;await ctx.installCertificate();assert.equal(posts,before+1);assert(!el('cert-install-submit').disabled);reject=false;
        response={code:3,error:true};await ctx.installCAChain();assert(!el('ca-install-result').textContent.includes('true'));assert(!el('cert-ca-submit').disabled);
    }
    console.log('PASS UI: error typing, bilingual/fallback, text output, uncertain POST, button recovery, refresh separation');
})().catch(e=>{console.error(e);process.exitCode=1;});
