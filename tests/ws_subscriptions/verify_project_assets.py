#!/usr/bin/env python3
"""Verify final optimized/gzip files and reproduce SPIFFS with the fixed SDK."""
import gzip,hashlib,json,os,pathlib,re,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[2]
build=pathlib.Path(os.environ.get('V3_BUILD','/tmp/tianshan-project-v3-build/build'));web=build/'esp-idf/ts_webui/web_optimized';src=root/'components/ts_webui/web';doc=pathlib.Path(os.environ.get('V3_RESULTS',str(root/'docs/repair/ws-project-v3')))
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
result={}
for rel in ['index.html','js/terminal.js','js/lang/zh-CN.js','js/lang/en-US.js']:
 p=web/rel;gz=p.with_suffix(p.suffix+'.gz');assert gzip.decompress(gz.read_bytes())==p.read_bytes()
 if rel.endswith('.js'):subprocess.run(['node','--check',str(p)],check=True)
 result[rel]={'source_sha256':sha(src/rel),'optimized_sha256':sha(p),'gzip_sha256':sha(gz),'optimized_bytes':p.stat().st_size}
asset_version=re.search(r"TS_ASSET_VERSION = '([^']+)'",(src/'index.html').read_text()).group(1)
assert asset_version in (web/'index.html').read_text()
assert 'sshControlUnsupported' in (web/'js/terminal.js').read_text()
with tempfile.TemporaryDirectory(prefix='ts-v3-spiffs-') as d:
 image=pathlib.Path(d)/'www.bin'
 cmd=['python3',os.environ['IDF_PATH']+'/components/spiffs/spiffsgen.py','0x300000',str(web),str(image),'--page-size=256','--obj-name-len=32','--meta-len=4','--use-magic','--use-magic-len']
 subprocess.run(cmd,check=True);assert image.read_bytes()==(build/'www.bin').read_bytes()
result['build']={p:{'sha256':sha(build/p),'bytes':(build/p).stat().st_size} for p in ['TianShanOS.bin','www.bin','TianShanOS.elf']}
result['spiffs_regeneration']='byte-identical from verified optimized directory'
(doc/'assets.json').write_text(json.dumps(result,indent=2))
print('PASS optimized syntax, gzip identity, cache key, and byte-identical SPIFFS reconstruction')
