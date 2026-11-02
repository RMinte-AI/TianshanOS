const fs = require('fs');
const vm = require('vm');
const crypto = require('crypto');
const srcPath='components/ts_webui/web/js/app.js';
const src=fs.readFileSync(srcPath,'utf8');
const start=src.indexOf('function evaluateExpression(expression, variables) {');
const end=src.indexOf('function sanitizeWidgetIcon(',start);
if(start<0||end<0) throw Error('source boundaries not found');
const fn=vm.runInNewContext(src.slice(start,end)+'\nevaluateExpression;', {console:{warn(){}}});
const variables={temp:42.36,used:0,name:'GPU',enabled:true};
const rows=[
 ['direct','${temp}',42.36],
 ['zero','${used}',0],
 ['convert','${temp} * 1.8 + 32',108.248],
 ['round','Math.round(${temp} * 10) / 10',42.4],
 ['clamp','Math.min(100, Math.max(0, ${temp}))',42.36],
 ['fixed','(${temp}).toFixed(1)','42.4'],
 ['condition','${temp} > 40 ? "偏热" : "正常"','偏热'],
 ['boolean','${enabled} && ${temp} >= 40 ? "偏热" : "正常"','偏热'],
 ['unit','${temp} + " °C"','42.36 °C'],
 ['label','${name} + " " + Math.round(${temp}) + " °C"','GPU 42 °C'],
 ['negative','(-${temp} + 2) / 2',-20.18]
].map(([id,expression,expected])=>{
 const actual=fn(expression,variables);
 const matches=typeof actual==='number'&&typeof expected==='number'?Math.abs(actual-expected)<1e-10:actual===expected;
 return {id,expression,variables,expected,actual,matches};
});
const output={kind:'BASELINE_COMPATIBILITY_PROBE_ONLY',source:srcPath,source_sha256:crypto.createHash('sha256').update(src).digest('hex'),function_first_line:src.slice(0,start).split('\n').length,method:'Actual baseline evaluateExpression in isolated Node VM; fixed benign math/text inputs only; not a firmware/browser/security or repaired-code test',cases:rows,all_match:rows.every(x=>x.matches)};
fs.writeFileSync('docs/repair/whole-product-20261031/review-evidence/formula-baseline-results.json',JSON.stringify(output,null,2)+'\n');
console.log(JSON.stringify({cases:rows.length,all_match:output.all_match,source_sha256:output.source_sha256}));
if(!output.all_match)process.exitCode=1;
