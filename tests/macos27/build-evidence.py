"""Reproduce generator bookkeeping; remaining pages are not promised runtime free space."""
import os,json,gzip,hashlib,importlib.util,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('spiffsgen','/Users/massif/esp/v5.5.2/esp-idf/components/spiffs/spiffsgen.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
base=Path(sys.argv[1] if len(sys.argv)>1 else 'output/macos27-20260928');result={}
for side in ['baseline','after']:
 root=base/side/'web_optimized';conf=m.SpiffsBuildConfig(256,m.SPIFFS_PAGE_IX_LEN,4096,m.SPIFFS_BLOCK_IX_LEN,4,32,m.SPIFFS_OBJ_ID_LEN,m.SPIFFS_SPAN_IX_LEN,True,True,'little',True,True,False);image=m.SpiffsFS(0x300000,conf);files=[]
 for directory,_,names in os.walk(root):
  for name in names:
   p=Path(directory)/name;raw=p.read_bytes();image.create_file('/'+p.relative_to(root).as_posix(),str(p));files.append({'path':p.relative_to(root).as_posix(),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()})
   if name.endswith('.gz'):assert gzip.decompress(raw)==p.with_suffix('').read_bytes()
 binary=image.to_binary();assert binary==(base/side/'www.bin').read_bytes()
 data_pages=sum(sum(not isinstance(p,m.SpiffsObjLuPage)for p in b.pages)for b in image.blocks);capacity=(0x300000//4096)*(4096//256-conf.OBJ_LU_PAGES_PER_BLOCK)
 result[side]={'files':files,'fileBytes':sum(x['bytes']for x in files),'gzipBytes':sum(x['bytes']for x in files if x['path'].endswith('.gz')),'imageBytes':len(binary),'imageHash':hashlib.sha256(binary).hexdigest(),'allocatedDataAndIndexPages':data_pages,'allocatedDataAndIndexBytes':data_pages*256,'availableDataAndIndexPages':capacity-data_pages,'availablePageBytes':(capacity-data_pages)*256,'note':'Generator page accounting, excludes reserved lookup pages. Not esp_spiffs_info or runtime writable capacity; hardware BLOCKED.'}
json.dump(result,open(base/'build-evidence.json','w'),indent=2)
for side,d in result.items():print(side,{k:v for k,v in d.items()if k!='files'})
