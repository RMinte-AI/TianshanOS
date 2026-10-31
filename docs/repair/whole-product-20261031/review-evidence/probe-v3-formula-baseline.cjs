const fs=require('fs'),vm=require('vm'),crypto=require('crypto');
const root='docs/repair/whole-product-20261031/review-evidence/';
const path='components/ts_webui/web/js/app.js', src=fs.readFileSync(path,'utf8');
const start=src.indexOf('function evaluateExpression(expression, variables) {'),end=src.indexOf('function sanitizeWidgetIcon(',start);
if(start<0||end<0)throw Error('source boundaries');
const fn=vm.runInNewContext(src.slice(start,end)+'\nevaluateExpression;',{console:{warn(){}}});
const fixture=JSON.parse(fs.readFileSync(root+'v3-formula-contract-cases.json','utf8'));
const rows=fixture.positive_cases.filter(c=>c.kind==='baseline_compatibility').map(c=>{
 const actual=fn(c.expression,c.variables);
 const matches=typeof actual==='number'&&typeof c.expected==='number'?Math.abs(actual-c.expected)<1e-10:actual===c.expected;
 return {...c,actual,matches};
});
const output={kind:'BENIGN_BASELINE_COMPATIBILITY_PROBE_ONLY',source:path,source_sha256:crypto.createHash('sha256').update(src).digest('hex'),cases:rows,all_match:rows.every(r=>r.matches),not_executed:'Blocked expressions and targeted corrections; no repaired code, firmware or device execution'};
fs.writeFileSync(root+'v3-formula-baseline-results.json',JSON.stringify(output,null,2)+'\n');
console.log(JSON.stringify({baseline_cases:rows.length,matching:rows.filter(r=>r.matches).length,all_match:output.all_match}));
if(!output.all_match)process.exitCode=1;
