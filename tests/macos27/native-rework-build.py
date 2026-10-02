"""Build an immutable local candidate with the existing production tools; never flash."""
from pathlib import Path
import datetime,gzip,hashlib,importlib.util,json,os,shutil,subprocess,sys
repo=Path(__file__).resolve().parents[2]
root=repo/'output/macos27-20260929-native-rework'/(sys.argv[1] if len(sys.argv)>1 else 'build')
root.mkdir(exist_ok=False)
source=repo/'components/ts_webui/web';web=root/'web_optimized'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
source_identity={str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()}
shutil.copytree(source,web)
commands=[]
def run(command,log):
 with (root/log).open('w') as output: result=subprocess.run(command,cwd=repo,stdout=output,stderr=subprocess.STDOUT)
 commands.append({'command':command,'exitCode':result.returncode,'log':log})
 (root/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 if result.returncode:raise RuntimeError('Build failed; inspect '+log)
run(['python3','tools/minify_web.py',str(web),'--gzip'],'minify.log')
python='/Users/massif/.espressif/python_env/idf5.5_py3.12_env/bin/python'
generator='/Users/massif/esp/v5.5.2/esp-idf/components/spiffs/spiffsgen.py'
run([python,generator,'0x300000',str(web),str(root/'www.bin'),'--page-size','256','--obj-name-len','32','--meta-len','4','--use-magic','--use-magic-len'],'spiffs.log')
spec=importlib.util.spec_from_file_location('spiffsgen',generator);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
conf=m.SpiffsBuildConfig(256,m.SPIFFS_PAGE_IX_LEN,4096,m.SPIFFS_BLOCK_IX_LEN,4,32,m.SPIFFS_OBJ_ID_LEN,m.SPIFFS_SPAN_IX_LEN,True,True,'little',True,True,False)
image=m.SpiffsFS(0x300000,conf);files=[]
for directory,_,names in os.walk(web):
 for name in names:
  p=Path(directory)/name;relative=p.relative_to(web).as_posix();raw=p.read_bytes();image.create_file('/'+relative,str(p));files.append({'path':relative,'bytes':len(raw),'sha256':sha(p)})
  if name.endswith('.gz'):assert gzip.decompress(raw)==p.with_suffix('').read_bytes(),relative
assert image.to_binary()==(root/'www.bin').read_bytes()
assert source_identity=={str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()},'Source changed during build'
allocated=sum(sum(not isinstance(p,m.SpiffsObjLuPage) for p in block.pages) for block in image.blocks)
capacity=(0x300000//4096)*(4096//256-conf.OBJ_LU_PAGES_PER_BLOCK)
baseline=json.loads((repo/'output/macos27-20260928/build-evidence.json').read_text())['baseline']
original=repo/'output/macos27-20260928/baseline/web_optimized'
assert {x['path']:x['sha256'] for x in baseline['files']}=={str(p.relative_to(original)):sha(p) for p in original.rglob('*') if p.is_file()},'Original build bytes changed'
assert sha(repo/'output/macos27-20260928/baseline/www.bin')==baseline['imageHash']
result={'at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':'CANDIDATE','sourceIdentity':source_identity,'files':files,'fileBytes':sum(x['bytes'] for x in files),'gzipBytes':sum(x['bytes'] for x in files if x['path'].endswith('.gz')),'imageBytes':(root/'www.bin').stat().st_size,'imageHash':sha(root/'www.bin'),'allocatedDataAndIndexPages':allocated,'allocatedDataAndIndexBytes':allocated*256,'availableDataAndIndexPages':capacity-allocated,'availablePageBytes':(capacity-allocated)*256,'scope':'Local official minify/gzip/SPIFFS tools, verified gzip contents and generator bookkeeping. Available pages exclude lookup pages; not hardware esp_spiffs_info or writable capacity. No deployment. Runtime, initial-request subset and performance acceptance separate.'}
result['deltaFromOriginal']={'fileBytes':result['fileBytes']-baseline['fileBytes'],'gzipBytes':result['gzipBytes']-baseline['gzipBytes'],'allocatedDataAndIndexPages':allocated-baseline['allocatedDataAndIndexPages']}
(root/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ['sourceIdentity','files']},indent=2))
