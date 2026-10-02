const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const acorn=require('../prompts/node_modules/acorn');
const [before,after]=process.argv.slice(2);
// Expand only after reading each actual presentation diff. No global color erasure.
const auditedOwners=new Set(['loadSystemPage','updateCpuInfo','loadCommandsPage','refreshCommandsList','refreshSources','refreshRules']);
const changes=[];
const builtMode=process.argv.includes('--built');
// The existing JS minifier strips blank lines and indentation inside templates.
// Transform only exact reviewed edit fragments; never normalize compared AST nodes.
const builtFragment=s=>s.split('\n').map(line=>line.trim()).filter(Boolean).join('\n');
const reviewedEdits=require('./presentation-edits.json').map(e=>builtMode&&!e.preserveFragmentEdges?{...e,before:builtFragment(e.before),after:builtFragment(e.after)}:e);
assert(reviewedEdits.every(e=>e.before.length && e.after.length),'Reviewed edits require nonempty exact anchors');
function presentation(s) {
    return s.replace(/style="([^"]*)"/g, (_,style)=>'style="'+style.replace(/(font-size|color)\s*:\s*[^;]+(?=;|$)/g,'$1:<presentation>')+'"');
}
function compare(a,b,owner='global',at='root') {
    if(a===b)return;
    if(a?.type==='FunctionDeclaration')owner=a.id.name;
    if(typeof a==='string'&&typeof b==='string') {
        assert(auditedOwners.has(owner)||reviewedEdits.some(e=>e.owner===owner),'Unaudited owner changed: '+owner);
        let restored=b;
        for(const edit of reviewedEdits.filter(e=>e.owner===owner).reverse())restored=restored.replaceAll(edit.after,edit.before);
        assert.equal(auditedOwners.has(owner)?presentation(a):a,auditedOwners.has(owner)?presentation(restored):restored,'Non-presentation change: '+owner+' '+at);
        changes.push({owner,at,before:a,after:b});return;
    }
    assert.equal(typeof a,typeof b,at);assert.equal(a===null,b===null,at);
    if(!a||typeof a!=='object')assert.deepEqual(a,b,at);
    else {
        const keys=o=>Object.keys(o).filter(k=>!['start','end','loc','raw'].includes(k));
        assert.deepEqual(keys(a),keys(b),at);
        for(const k of keys(a))compare(a[k],b[k],owner,at+'.'+k);
    }
}
const source=dir=>fs.readFileSync(path.join(dir,'js/app.js'),'utf8');
const parse=s=>acorn.parse(s,{ecmaVersion:'latest'});
compare(parse(source(before)),parse(source(after)));
const protectedFiles=[];
function walk(dir,rel='') {
    for(const item of fs.readdirSync(dir,{withFileTypes:true})) {
        const name=path.join(rel,item.name);
        if(item.isDirectory())walk(path.join(dir,item.name),name);
        else if(!['css/style.css','index.html','js/app.js'].includes(builtMode?name.replace(/\.gz$/,''):name)) {
            const readProtected=dir=>{const bytes=fs.readFileSync(path.join(dir,name));return builtMode&&name.endsWith('.gz')?require('node:zlib').gunzipSync(bytes):bytes;};
            assert.deepEqual(readProtected(before),readProtected(after),'Protected file: '+name);
            protectedFiles.push(name);
        }
    }
}
walk(before);
// The checker must reject behavioral and device-color mutations, not just pass B.
const original=source(before);
const mutations=[['request',"api.getSystemInfo()","api.getMemoryInfo()"],['timer','interval: 1000','interval: 2000'],['permission','api.isRoot()','api.isLoggedIn()'],['data color',"color: '#9ca3af'","color: '#087cf0'"],['hidden control','font-size:0.75em;padding:2px 8px','display:none;padding:2px 8px']];
const negative=[];
for(const [name,old,value]of mutations){
    assert(original.includes(old),'Negative control missing: '+name);
    let rejected=false;
    try{compare(parse(original),parse(original.replace(old,value)));}catch{rejected=true;}
    assert(rejected,'Negative control accepted: '+name);negative.push({name,rejected});
}


// Entry markup and inline i18n are byte-identical after the explicitly approved
// critical CSS, asset version, decorative brand and memory caption changes.
const indexEdits=require('./presentation-index-edits.json');
const readIndex=dir=>{
    let source=fs.readFileSync(path.join(dir,'index.html'),'utf8');
    for(const edit of indexEdits) {
        assert(edit.before.length && edit.after.length,'Entry edits require exact anchors');
        source=source.replaceAll(edit.after,edit.before);
    }
    return source;
};
const brandOld='<img src="/images/tslogo-48.png" alt="TianshanOS Logo" class="logo-icon">';
const brandNew='<svg class="logo-icon" aria-hidden="true" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 30" fill="none"><path d="M3 24 12 6l5 10 3-5 8 13H3Z" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><path d="m9 12 3 3 3-3M7 24l7-9 6 9" stroke="currentColor" stroke-width="1.2"/></svg>';
const normalizeIndex=s=>s.replace(/<style>[\s\S]*?<\/style>/,'<style>CRITICAL CSS</style>').replaceAll('20260929-macos27-v3-r2','20260928-local-xterm').replace(brandNew,brandOld).replace('id="memory-detail-timestamp" style="font-size:12px;color:var(--text-secondary)"','id="memory-detail-timestamp" style="font-size:0.8em;color:#9ca3af"');
assert(readIndex(after).includes(brandNew),'Approved SVG changed');
assert.equal(normalizeIndex(readIndex(before)),normalizeIndex(readIndex(after)),'Entry contract changed');
for(const [name,from,to]of [['navigation','href="#/files"','href="#/network"'],['language key','data-i18n="nav.system"','data-i18n="nav.files"'],['submit event','id="login-form"','id="changed-form"']]){
    const mutated=readIndex(after).replace(from,to);assert.notEqual(normalizeIndex(readIndex(before)),normalizeIndex(mutated),'Entry negative accepted: '+name);
}

console.log(JSON.stringify({status:'PASS',scope:builtMode?'Built AST and protected decompressed contents; gzip timestamps excluded; not device proof':'AST behavior and protected bytes only; not runtime or device proof',entryContract:"PASS", entryNegative:["navigation","language key","submit event"],protectedFiles,changes,negative},null,2));
