const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '../../components/ts_webui/web');
const read = file => fs.readFileSync(path.join(root,file),'utf8');
class Element {
    constructor(tag, doc) { this.tagName=tag.toUpperCase();this.doc=doc;this.children=[];this.style={};this.dataset={};this.attributes={};this.className='';this.textContent='';this.value='';this.disabled=false;this.hidden=false;
        this.classList={add:(...ns)=>{this.className=[...new Set([...this.className.split(' '),...ns])].join(' ').trim();},remove:(...ns)=>{this.className=this.className.split(' ').filter(n=>!ns.includes(n)).join(' ');},contains:n=>this.className.split(' ').includes(n),toggle:(n,on)=>on?this.classList.add(n):this.classList.remove(n)};
    }
    set id(v){this._id=v;this.doc.elements.set(v,this);} get id(){return this._id;}
    setAttribute(k,v){this.attributes[k]=v;if(k.startsWith('data-'))this.dataset[k.slice(5).replace(/-([a-z])/g,(_,c)=>c.toUpperCase())]=v;}
    getAttribute(k){return this.attributes[k];} hasAttribute(k){return k in this.attributes;}
    appendChild(n){n.parent=this;this.children.push(n);return n;} append(...ns){ns.forEach(n=>this.appendChild(n));}
    replaceChildren(...ns){this.children=[];this.append(...ns);this.textContent='';}
    remove(){if(this.parent)this.parent.children=this.parent.children.filter(n=>n!==this);if(this.id)this.doc.elements.delete(this.id);}
    removeChild(n){n.remove();} querySelectorAll(s){return this.children.filter(n=>s==='*'||s.startsWith('.')&&n.classList.contains(s.slice(1)));}
    querySelector(s){return this.querySelectorAll(s)[0]||null;}
    click(){this.onclick?.();} focus(){} scrollIntoView(){}
}
function harness(language='zh-CN') {
    const listeners={}, elements=new Map(), timers=new Map();let now=0,id=0;
    const doc={elements,documentElement:{},visibilityState:'visible',addEventListener(){},removeEventListener(){},
        createElement:tag=>new Element(tag,doc),getElementById:id=>elements.get(id)||null,
        querySelectorAll:()=>[],querySelector:s=>s==='main'?doc.main:null};
    doc.body=new Element('body',doc);doc.head=new Element('head',doc);doc.main=new Element('main',doc);
    const el=(id,tag='div')=>{let n=elements.get(id);if(!n){n=doc.createElement(tag);n.id=id;doc.body.appendChild(n);}return n;};
    ['app','lang-name','lang-menu','ws-status','page-content'].forEach(id=>el(id));
    const storage=new Map([['ts_language',language]]);
    const logs=[];
    const context={document:doc,navigator:{language},localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,String(v)),removeItem:k=>storage.delete(k)},
        console:{log(){},debug(){},warn:(...x)=>logs.push(x),error:(...x)=>logs.push(x)},
        setTimeout:(fn,ms=0)=>{timers.set(++id,{fn,at:now+ms});return id;},clearTimeout:n=>timers.delete(n),
        setInterval:(fn,ms)=>{timers.set(++id,{fn,at:now+ms,repeat:ms});return id;},clearInterval:n=>timers.delete(n),
        Date:class extends Date{static now(){return now;}},CustomEvent:class{constructor(type,init={}){this.type=type;this.detail=init.detail;}},
        JSON,AbortController,URL,URLSearchParams,Blob,TextDecoder,TextEncoder,Map,Set,Promise,Error,SyntaxError,
        location:{host:'127.0.0.1',protocol:'http:',hash:''},CSS:{escape:s=>s},
        addEventListener:(k,f)=>(listeners[k]??=[]).push(f),removeEventListener(){},
        dispatchEvent:event=>(listeners[event.type]||[]).forEach(f=>f(event)),
        confirm:()=>true,prompt:()=>null,WebSocket:class{static OPEN=1;static CONNECTING=0;},
        fetch:async()=>{throw new Error('Network must be explicitly mocked');}};
    context.window=context;const ctx=vm.createContext(context);
    const run=code=>vm.runInContext(code,ctx);
    run(read('index.html').match(/<script>\s*([\s\S]*?)<\/script>/)[1]);
    for(const l of ['zh-CN','en-US'])run(read(`js/lang/${l}.js`));
    const script=doc.head.children.at(-1);script.onload();
    const ready=async()=>{await ctx.languageReady;};
    const advance=async(ms)=>{const end=now+ms;for(;;){let next=[...timers].filter(([,t])=>t.at<=end).sort((a,b)=>a[1].at-b[1].at)[0];if(!next)break;let[n,t]=next;now=t.at;if(t.repeat)t.at+=t.repeat;else timers.delete(n);await t.fn();await Promise.resolve();}now=end;};
    const load=()=>{run(read('js/api.js'));run(read('js/terminal.js'));run(read('js/app.js'));};
    return {ctx,doc,el,run,load,ready,advance,timers,logs,read};
}
module.exports={harness,read,root};
