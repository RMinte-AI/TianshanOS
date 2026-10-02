#!/usr/bin/env python3
"""按中文文案找现有语言包 key：python3 findkey.py "别名" "隐藏" ...  （精确匹配优先，其次包含）。--en 同时显示英文值。"""
import json, os, subprocess, sys
REPO = '/Users/massif/TianshanOS'
LANG = os.path.join(REPO, 'components/ts_webui/web/js/lang')
JS = r"""const vm=require('vm'),fs=require('fs');let cap;vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),{i18n:{registerLanguage:(c,o)=>{cap=o}}});
function flat(o,p,out){for(const[k,v]of Object.entries(o)){const key=p?p+'.'+k:k;if(v&&typeof v==='object')flat(v,key,out);else out[key]=v}return out}
console.log(JSON.stringify(flat(cap,'',{})))"""
def load(code): return json.loads(subprocess.check_output(['node', '-e', JS, os.path.join(LANG, code + '.js')]))
zh, en = load('zh-CN'), load('en-US')
show_en = '--en' in sys.argv
for q in [a for a in sys.argv[1:] if a != '--en']:
    exact = [k for k, v in zh.items() if isinstance(v, str) and v.strip() == q]
    part = [k for k, v in zh.items() if isinstance(v, str) and q in v and k not in exact]
    print(f'## {q}')
    for k in exact: print(f'  = {k}  {("· " + str(en.get(k))) if show_en else ""}')
    for k in part[:8]: print(f'  ~ {k}  「{zh[k][:40]}」{("· " + str(en.get(k))) if show_en else ""}')
    if not exact and not part: print('  （无）')
