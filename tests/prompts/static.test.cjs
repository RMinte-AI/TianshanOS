const {test}=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm');
const acorn=require('acorn');const {read,harness}=require('./harness.cjs');
let langs={};for(const lang of ['zh-CN','en-US'])vm.runInNewContext(read(`js/lang/${lang}.js`),{i18n:{registerLanguage:(k,v)=>langs[k]=v}});
const lookup=(l,k)=>k.split('.').reduce((v,p)=>v?.[p],langs[l]);
const slots=v=>[...new Set([...v.matchAll(/(?<!\$)\{(\w+)\}/g)].map(m=>m[1]))].sort();
function walk(n,fn){if(!n||typeof n!=='object')return;fn(n);for(const v of Object.values(n))if(Array.isArray(v)){v.forEach(c=>walk(c,fn));}else if(v&&typeof v==='object')walk(v,fn);}
function choices(n){if(n?.type==='Literal'&&typeof n.value==='string')return[n.value];if(n?.type==='ConditionalExpression')return[...choices(n.consequent),...choices(n.alternate)];return[];}
const calls=[];
for(const f of ['js/app.js','js/api.js','js/terminal.js','js/router.js'])walk(acorn.parse(read(f),{ecmaVersion:'latest',locations:true}),n=>{if(n.type==='CallExpression'&&['t','certText'].includes(n.callee.name))for(const key of choices(n.arguments[0]))calls.push({f,line:n.loc.start.line,key,params:n.arguments[n.callee.name==='certText'?2:1]});});
const html=read('index.html');
for(const f of ['index.html','js/app.js','js/terminal.js'])for(const m of read(f).matchAll(/data-i18n(?:-title|-placeholder)?="([^"]+)"/g))if(!m[1].includes('${'))calls.push({f,key:m[1]});
for(const m of read('js/app.js').matchAll(/(?:labelKey|descKey): '([^']+)'/g))calls.push({f:'dynamic widget option',key:m[1]});
// Explicit finite dynamic families; unknown names use neutral runtime fallbacks.
for(const family of ['promptRepair','runtimeRepair'])for(const key of Object.keys(langs['zh-CN'][family]))calls.push({f:'dynamic '+family,key:family+'.'+key,dynamic:true});
test('actual UI literal/conditional and finite dynamic keys exist in both languages with matching contracts',()=>{
 let failures=[];
 for(const c of calls){const a=lookup('zh-CN',c.key),b=lookup('en-US',c.key);
  if(typeof a!=='string'||typeof b!=='string'){failures.push(`${c.f}:${c.line}: missing ${c.key}`);continue;}
  if(JSON.stringify(slots(a))!==JSON.stringify(slots(b)))failures.push(`${c.key}: differing parameters ${slots(a)} / ${slots(b)}`);
  if(c.params?.type==='ObjectExpression'){
   const provided=c.params.properties.map(p=>p.key?.name||p.key?.value);
   for(const key of slots(a))if(!provided.includes(key))failures.push(`${c.f}:${c.line}: ${c.key} missing parameter ${key}`);
  }else if(!c.params&&!c.dynamic&&slots(a).length)failures.push(`${c.f}:${c.line}: ${c.key} missing all parameters ${slots(a)}`);
 }
 assert.deepEqual([...new Set(failures)],[]);
 console.log(`Checked ${calls.length} references including conditional and finite dynamic families.`);
});
test('production i18n is the inline entry; no second translator script is loaded',()=>{assert(!html.includes('src="/js/i18n.js'));assert(html.includes('window.i18n = i18n'));});
