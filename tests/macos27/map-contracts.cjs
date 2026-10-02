const fs=require('node:fs'), assert=require('node:assert/strict');
const [before,after,out]=process.argv.slice(2);
const a=JSON.parse(fs.readFileSync(before)),b=JSON.parse(fs.readFileSync(after));
const mapping={scope:'Every source inventory entry mapped 1:1; runtime remains independently recorded',counts:{},rows:[]};
for(const kind of ['functions','bindings','calls','branches','templates']) {
    assert.equal(a[kind].length,b[kind].length,kind+' inventory count');
    const seen=new Set();
    for(let i=0;i<a[kind].length;i++) {
        const old=a[kind][i],next=b[kind][i];
        assert.equal(old.id,next.id);assert.equal(old.owner,next.owner);assert.equal(old.file,next.file);assert(!seen.has(next.id));seen.add(next.id);
        for(const field of ['test','target','conditions'])assert.deepEqual(old[field],next[field],kind+' '+old.id+' '+field);
        if(kind==='calls')assert.equal(old.code,next.file==='index.html'?next.code.replaceAll('20260929-macos27-v3','20260928-local-xterm'):next.code,old.id+' call contract');
        const clean=x=>Object.fromEntries(Object.entries(x).filter(([k])=>!['line','end'].includes(k)));
        const exact=JSON.stringify(clean(old))===JSON.stringify(clean(next));
        mapping.rows.push({id:old.id,kind,owner:old.owner,file:old.file,beforeLine:old.line,afterLine:next.line,structural:'PASS',content:exact?'UNCHANGED':'PRESENTATION_REVIEW_REQUIRED',runtime:'NOT_RUN'});
    }
    mapping.counts[kind]=seen.size;
}
fs.writeFileSync(out,JSON.stringify(mapping,null,2)+'\n');console.log(mapping.counts);
