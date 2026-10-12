const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync('components/ts_webui/web/js/app.js','utf8');
// The failure sentences are built by api.js (describeFailure and the error helpers it uses); run that code verbatim.
const apiSource = fs.readFileSync('components/ts_webui/web/js/api.js','utf8');
const failureCode = apiSource.slice(apiSource.indexOf('function apiErrorReason'), apiSource.indexOf('class TianShanAPI'));
assert(failureCode.includes('function describeFailure'));
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
function el(id) {if (!elements.has(id)) elements.set(id,{textContent:'',innerHTML:'',value:'test PEM',disabled:false,style:{},classList:{add(){},remove(){}}});return elements.get(id);}
let posts=0, refreshFail=false, response={code:0}, reject=false;
const lookup = key => key.split('.').reduce((o,k)=>o?.[k],translations[locale]);
const ctx = vm.createContext({console, setTimeout:()=>{}, document:{getElementById:el}, window:{},
    t:(key,params={})=>{let v=lookup(key)??key;return v.replace(/\{(\w+)\}/g,(_,k)=>params[k]??`{${k}}`);},
    // the page's strict translation helper: a text is complete only when every placeholder has a value
    i18n:{isReady:()=>true,complete:(key,params)=>{const v=lookup(key);return typeof v==='string'&&!Array.from(v.matchAll(/(?<!\$)\{(\w+)\}/g)).some(m=>!params||params[m[1]]===undefined||params[m[1]]===null);}},
    api:{certInstall:async()=>{posts++;if(reject)throw new Error('timeout');return response;},certInstallCA:async()=>{posts++;return response;}},
    refreshCertStatus:async()=>{if(refreshFail)throw new Error('refresh failed');}
});
vm.runInContext(failureCode,ctx);
for(const name of ['certText','certValidity','certExpiry','escapeHtml','failureHtml','packBox','installCertificate','installCAChain','installCertMaterial'])vm.runInContext(fn(name),ctx);
(async()=>{
    assert.equal(ctx.certText('missing.key','fallback'),'fallback');
    for(locale of ['zh-CN','en-US']) {
        for(const key of ['none','invalid','time_unverified','not_yet_valid','valid','expired']) assert(!ctx.certValidity(key).includes('pkiRepair.'));
        assert(ctx.certValidity(undefined));
        assert.equal(ctx.certExpiry({is_valid:true,days_until_expiry:20}), ''); // old firmware
        assert.equal(ctx.certExpiry({validity:'time_unverified',seconds_until_expiry:null}), '');
        assert(!ctx.certExpiry({validity:'expired',seconds_until_expiry:-1}).includes('pkiRepair.'));
        // a device text that looks like markup never becomes markup: the sentence is the catalogue's, the text only appears escaped in the folded details
        response={code:7,error:'<img src=x onerror=alert(1)>'};await ctx.installCertificate();
        assert(!el('cert-install-result').innerHTML.includes('<img'));assert(el('cert-install-result').innerHTML.includes('&lt;img'));
        assert(el('cert-install-result').innerHTML.includes(lookup('failure.ops.cert.install.failed')));assert(!el('cert-install-submit').disabled);
        response={code:0};refreshFail=true;await ctx.installCertificate();assert(!el('cert-install-submit').disabled);
        assert(!el('cert-install-result').textContent.includes('failed'));
        // a lost answer is "unconfirmed" (not "failed"), is posted once, and the button comes back
        reject=true;const before=posts;await ctx.installCertificate();assert.equal(posts,before+1);assert(!el('cert-install-submit').disabled);reject=false;
        assert(el('cert-install-result').innerHTML.includes(lookup('failure.ops.cert.install.unknown')));assert(!el('cert-install-result').innerHTML.includes(lookup('failure.ops.cert.install.failed')));
        response={code:3,error:true};await ctx.installCAChain();assert(!el('ca-install-result').innerHTML.includes('true'));assert(!el('cert-ca-submit').disabled);
    }
    console.log('PASS UI: failure sentences from the catalogue, bilingual/fallback, device text only escaped in details, uncertain POST, button recovery, refresh separation');
})().catch(e=>{console.error(e);process.exitCode=1;});
