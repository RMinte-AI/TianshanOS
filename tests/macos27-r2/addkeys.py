#!/usr/bin/env python3
"""向 zh-CN / en-US 语言包同时新增 key（只新增，绝不改已有 key）。
  python3 tests/macos27-r2/addkeys.py <区块> <key> "<中文>" "<English>" [<key> "<中文>" "<English>" ...]
新增项写入区块的开头，并追加到 output/macos27-r2/new-keys.md 供审阅。若 key 已存在则报错退出。"""
import re, sys, os, json, subprocess
REPO='/Users/massif/TianshanOS'
LANG=os.path.join(REPO,'components/ts_webui/web/js/lang')
def q(v): return "'"+v.replace('\\','\\\\').replace("'","\\'").replace('\n','\\n')+"'"
def flat_keys(path):
    js=r"""const vm=require('vm'),fs=require('fs');let cap;vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),{i18n:{registerLanguage:(c,o)=>{cap=o}}});
function flat(o,p,out){for(const[k,v]of Object.entries(o)){const key=p?p+'.'+k:k;if(v&&typeof v==='object')flat(v,key,out);else out[key]=v}return out}
console.log(JSON.stringify(flat(cap,'',{})))"""
    return json.loads(subprocess.check_output(['node','-e',js,path]))
def main():
    section=sys.argv[1]; rest=sys.argv[2:]
    assert len(rest)%3==0 and rest, '参数应为 key 中文 English 三元组'
    items=[(rest[i],rest[i+1],rest[i+2]) for i in range(0,len(rest),3)]
    for code,idx in (('zh-CN',1),('en-US',2)):
        path=os.path.join(LANG,code+'.js')
        have=flat_keys(path)
        for it in items:
            if f'{section}.{it[0]}' in have: sys.exit(f'{code}: {section}.{it[0]} 已存在，拒绝覆盖')
        s=open(path,encoding='utf-8').read()
        m=list(re.finditer(r'^    '+re.escape(section)+r': \{\n',s,re.M))
        assert len(m)==1, f'{code}: 区块 {section} 匹配到 {len(m)} 处'
        ins=''.join(f'        {it[0]}: {q(it[idx])},\n' for it in items)
        s=s[:m[0].end()]+ins+s[m[0].end():]
        open(path,'w',encoding='utf-8').write(s)
        after=flat_keys(path)
        for it in items: assert after[f'{section}.{it[0]}']==it[idx], f'{code}: 回读不一致 {it[0]}'
    log=os.path.join(REPO,'output/macos27-r2/new-keys.md')
    new=not os.path.exists(log)
    with open(log,'a',encoding='utf-8') as f:
        if new: f.write('# 新增语言包 key（中英同时新增，现有 key 未改）\n\n| key | 中文 | English |\n|---|---|---|\n')
        for it in items: f.write(f'| {section}.{it[0]} | {it[1]} | {it[2]} |\n')
    print('已新增', ', '.join(f'{section}.{it[0]}' for it in items))
main()
