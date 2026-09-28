#!/usr/bin/env python3
"""Use the actual fixed-SDK compile commands; no target execution."""
import os,json,pathlib,shlex,subprocess,tarfile,tempfile
root=pathlib.Path(__file__).resolve().parents[2];build=pathlib.Path(os.environ.get('V3_BUILD','/tmp/tianshan-project-v3-build/build'));doc=root/'docs/repair/ws-project-v3'
commands=json.loads((build/'compile_commands.json').read_text());result={}
with tempfile.TemporaryDirectory(prefix='ts-v3-size-') as d:
 temp=pathlib.Path(d)
 for rel,typ in [('components/ts_security/src/ts_ssh_shell.c','struct ts_ssh_shell_s'),('components/ts_webui/src/ts_webui_ws.c','ssh_shell_context_t')]:
  entry=next(c for c in commands if c['file']==str(root/rel));base=shlex.split(entry['command']);result[rel]={}
  for kind in ['before','after']:
   if kind=='before':
    with tarfile.open(doc/'baseline.tar.gz') as t:source=t.extractfile(rel).read().decode()
   else:source=(root/rel).read_text()
   src=temp/(kind+'-'+pathlib.Path(rel).name);src.write_text(source+'\nconst unsigned char v3_object_size[sizeof('+typ+')]={0};\n')
   obj=src.with_suffix('.o');args=base[:];args[args.index('-o')+1]=str(obj);args[args.index('-c')+1]=str(src);args+=['-fstack-usage']
   subprocess.run(args,cwd=entry['directory'],check=True,capture_output=True)
   nm=str(pathlib.Path(args[0]).with_name('xtensa-esp32s3-elf-nm'));nm=nm if pathlib.Path(nm).exists() else str(pathlib.Path(args[0]).with_name('xtensa-esp-elf-nm'))
   symbols=subprocess.check_output([nm,'-S',str(obj)],text=True)
   size=int(next(l.split()[1] for l in symbols.splitlines() if l.endswith(' v3_object_size')),16)
   stack={l.split('\t')[0].rsplit(':',1)[-1]:l.split('\t')[1:] for l in src.with_suffix('.su').read_text().splitlines()}
   result[rel][kind]={'object_bytes':size,'stack_usage':stack}
(doc/'target-resource.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:{x:v[x]['object_bytes'] for x in v} for k,v in result.items()},indent=2))
