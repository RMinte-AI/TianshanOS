// Source evidence complements, but never substitutes for, browser/device tests.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const acorn=require('../prompts/node_modules/acorn');
const root=path.resolve(process.argv[2]),out=process.argv[3];
const entries=[],functions=[],calls=[],storage=[];
function walk(node,owner,file,source){
 if(!node||typeof node!=='object')return;
 if(node.type==='FunctionDeclaration'){owner=node.id.name;functions.push({file,name:owner,line:node.loc.start.line,end:node.loc.end.line});}
 if(node.type==='CallExpression'){
  const callee=source.slice(node.callee.start,node.callee.end);
  if(/^(api\.|router\.register|.*localStorage\.)/.test(callee))calls.push({file,owner,line:node.loc.start.line,code:source.slice(node.start,node.end)});
 }
 if(node.type==='TemplateElement'||node.type==='Literal'&&typeof node.value==='string'){
  const text=node.type==='TemplateElement'?node.value.raw:node.value;
  for(const m of text.matchAll(/<(button|a|input|select|textarea|form|h[123]|table)\b[^>]*>|\bon(?:click|change|input|submit|drop|dragstart)\s*=\s*["'][^"']*/g)){
   entries.push({id:`UI-${String(entries.length+1).padStart(5,'0')}`,file,owner,line:node.loc.start.line+text.slice(0,m.index).split('\n').length-1,markup:m[0],status:'NOT_RUN',after_evidence:null});
  }
 }
 for(const [key,value]of Object.entries(node)){if(key==='loc')continue;if(Array.isArray(value))value.forEach(n=>walk(n,owner,file,source));else if(value&&typeof value==='object')walk(value,owner,file,source);}
}
for(const file of ['js/app.js','js/api.js','js/router.js','js/terminal.js','js/dragSort.js']){
 const source=fs.readFileSync(path.join(root,file),'utf8');walk(acorn.parse(source,{ecmaVersion:'latest',locations:true}),null,file,source);
 for(const m of source.matchAll(/localStorage\.(?:getItem|setItem|removeItem)\(['"]([^'"]+)/g))storage.push({file,key:m[1]});
}
const html=fs.readFileSync(path.join(root,'index.html'),'utf8');
for(const m of html.matchAll(/<(button|a|input|select|textarea|form|h[123]|table)\b[^>]*>/g))entries.push({id:`UI-${String(entries.length+1).padStart(5,'0')}`,file:'index.html',owner:'shell',line:html.slice(0,m.index).split('\n').length,markup:m[0],status:'NOT_RUN',after_evidence:null});
const hashes={};function hashDir(dir){for(const entry of fs.readdirSync(dir,{withFileTypes:true})){const p=path.join(dir,entry.name);if(entry.isDirectory())hashDir(p);else hashes[path.relative(root,p)]=crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');}}hashDir(root);
fs.writeFileSync(out,JSON.stringify({scope:'AST-assisted source candidate inventory; dynamic branches and visible reachability require runtime evidence',root,entries,functions,calls,storage,hashes},null,2));
console.log(`${entries.length} entry candidates; ${functions.length} functions; ${calls.length} API/storage/route calls. Runtime status remains NOT_RUN.`);
