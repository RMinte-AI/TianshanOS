// Source inventory and exact syntax-tree contract. Runtime evidence is separate.
const fs = require('node:fs');
const path = require('node:path');
const acorn = require('../prompts/node_modules/acorn');
const [root, output] = process.argv.slice(2);
const inventory = {scope:'Source contracts; not runtime coverage',functions:[],bindings:[],calls:[],branches:[],templates:[]};
const files = ['js/app.js','js/api.js','js/router.js','js/terminal.js','js/dragSort.js','index.html'];
for (const file of files) {
    const full = fs.readFileSync(path.join(root,file),'utf8');
    const sources = file.endsWith('.html') ? [...full.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(m=>({source:m[1],offset:full.slice(0,m.index).split('\n').length-1})) : [{source:full,offset:0}];
    for (const {source,offset} of sources) {
        const code = n=>source.slice(n.start,n.end);
        function visit(n, owner='shell', conditions=[]) {
            if (!n || typeof n!=='object') return;
            if (n.type==='FunctionDeclaration') {
                owner=n.id.name;
                inventory.functions.push({file,owner,line:n.loc.start.line+offset,end:n.loc.end.line+offset});
            }
            if (n.type==='IfStatement'||n.type==='ConditionalExpression') {
                inventory.branches.push({file,owner,line:n.loc.start.line+offset,test:code(n.test)});
            }
            if (n.type==='CallExpression') {
                const callee=code(n.callee);
                if (/api\.|router\.|Storage\.|addEventListener|removeEventListener|setInterval|clearInterval|setTimeout|clearTimeout|subscribe|unsubscribe|querySelector|getElementById/.test(callee)) {
                    inventory.calls.push({file,owner,line:n.loc.start.line+offset,code:code(n),conditions});
                }
            }
            if (n.type==='AssignmentExpression' && /\.(?:on\w+|innerHTML|className)$/.test(code(n.left))) inventory.bindings.push({file,owner,line:n.loc.start.line+offset,target:code(n.left),value:code(n.right),conditions});
            if (n.type==='TemplateLiteral' && /<(?:button|input|select|textarea|div|table)\b/.test(code(n))) inventory.templates.push({file,owner,line:n.loc.start.line+offset,source:code(n),conditions});
            for (const [key,v] of Object.entries(n)) {
                if (key==='loc') continue;
                const next = (n.type==='IfStatement'||n.type==='ConditionalExpression') && ['consequent','alternate'].includes(key) ? [...conditions,(key==='alternate'?'NOT ':'')+code(n.test)] : conditions;
                if (Array.isArray(v)) v.forEach(x=>visit(x,owner,next));
                else if(v&&typeof v==='object')visit(v,owner,next);
            }
        }
        visit(acorn.parse(source,{ecmaVersion:'latest',locations:true}));
    }
}
for (const [kind,rows] of Object.entries(inventory)) if(Array.isArray(rows))rows.forEach((r,i)=>r.id=kind.toUpperCase()+'-'+String(i+1).padStart(5,'0'));
fs.writeFileSync(output,JSON.stringify(inventory,null,2)+'\n');
console.log(Object.fromEntries(Object.entries(inventory).filter(([,v])=>Array.isArray(v)).map(([k,v])=>[k,v.length])));
