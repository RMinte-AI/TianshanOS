#!/usr/bin/env python3
"""Offline vendor/build verification with the project's fixed SPIFFS generator."""
import gzip, hashlib, importlib.util, json, os, pathlib
root=pathlib.Path(__file__).resolve().parents[2]
build=pathlib.Path(os.environ['V3_BUILD']);web=build/'esp-idf/ts_webui/web_optimized'
doc=root/'docs/repair/xterm-local'
records=json.loads((doc/'vendor.json').read_text());report=[]
for r in records:
 rel='vendor/xterm/'+r['file'];source=(root/'components/ts_webui/web'/rel).read_bytes();actual=(web/rel).read_bytes()
 assert source==actual and hashlib.sha256(actual).hexdigest()==r['sha256']
 item={'file':rel,'raw':len(actual),'sha256':r['sha256']}
 if rel.endswith(('.js','.css')):
  packed=(web/(rel+'.gz')).read_bytes();assert gzip.decompress(packed)==source;item['gzip']=len(packed)
 report.append(item)
spec=importlib.util.spec_from_file_location('spiffsgen',os.environ['IDF_PATH']+'/components/spiffs/spiffsgen.py');s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
cfg=s.SpiffsBuildConfig(256,s.SPIFFS_PAGE_IX_LEN,4096,s.SPIFFS_BLOCK_IX_LEN,4,32,s.SPIFFS_OBJ_ID_LEN,s.SPIFFS_SPAN_IX_LEN,True,True,'little',True,True,False)
image=s.SpiffsFS(0x300000,cfg)
for directory,_,files in os.walk(web):
 for name in files:
  p=pathlib.Path(directory)/name;image.create_file('/'+str(p.relative_to(web)),str(p))
assert image.to_binary()==(build/'www.bin').read_bytes()
for r in report:
 assert ('/'+r['file']).encode() in (build/'www.bin').read_bytes()
 if 'gzip' in r: assert ('/'+r['file']+'.gz').encode() in (build/'www.bin').read_bytes()
flash=json.loads((build/'flasher_args.json').read_text());assert any(pathlib.Path(v).name=='www.bin' for v in flash['flash_files'].values())
result={'files':report,'runtime_raw':sum(r['raw'] for r in report if 'gzip'in r),'runtime_gzip':sum(r.get('gzip',0) for r in report),'vendor_raw_including_licenses':sum(r['raw'] for r in report),'spiffs':{'partition_bytes':image.img_size,'allocated_blocks':len(image.blocks),'block_bytes':4096,'unallocated_blocks':image.blocks_lim-len(image.blocks),'note':'generator allocated blocks including metadata, not runtime filesystem free-space API'},'www_sha256':hashlib.sha256((build/'www.bin').read_bytes()).hexdigest(),'flash_files':flash['flash_files'],'spiffs_reconstruction':'byte-identical'}
(doc/'build-assets.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
