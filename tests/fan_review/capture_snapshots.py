#!/usr/bin/env python3
"""Prepared for a separately authorized sample test. GET-only bounded snapshots."""
import argparse,json,time,math,urllib.request
from pathlib import Path
from datetime import datetime,timezone
p=argparse.ArgumentParser(description='Read-only fan/temperature snapshots; not full telemetry or firmware execution timing.')
p.add_argument('--base-url',required=True)
p.add_argument('--seconds',type=int,default=180)
p.add_argument('--interval',type=float,default=1.0)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not 1<=a.seconds<=600 or not 1<=a.interval<=60:p.error('seconds: 1..600; interval: 1..60 seconds')
base=a.base_url.rstrip('/')
if not base.startswith(('http://','https://')):p.error('base-url must be http(s)')
start=time.monotonic();previous_timestamp=None
with a.output.open('x') as log:
 for index in range(math.ceil(a.seconds/a.interval)):
  if time.monotonic()-start>=a.seconds:break
  row={'index':index,'host_utc':datetime.now(timezone.utc).isoformat(),'elapsed_sec':round(time.monotonic()-start,3),'snapshot_only':True}
  for key,path in [('fan','/api/v1/fan/status'),('temperature','/api/v1/temp/status')]:
   sent=time.monotonic()
   try:
    request=urllib.request.Request(base+path,method='GET')
    with urllib.request.urlopen(request,timeout=2) as response:
     raw=response.read(65537)
    if len(raw)>65536:raise ValueError('response exceeds 64KiB')
    row[key]=json.loads(raw)
    if not isinstance(row[key],dict):raise ValueError('response is not an API object')
   except Exception as e:row[key]={'unavailable':True,'error':str(e)}
   row[key+'_round_trip_ms']=round((time.monotonic()-sent)*1000,3)
  temperature_data=row['temperature'].get('data')
  timestamp=temperature_data.get('current_timestamp_ms') if isinstance(temperature_data,dict) and row['temperature'].get('code')==0 else None
  row['sensor_report_changed']=None if timestamp is None else timestamp!=previous_timestamp
  if timestamp is not None:previous_timestamp=timestamp
  # These requests are sequential and not atomic. Unavailable intervals stay unavailable.
  log.write(json.dumps(row,ensure_ascii=False)+'\n');log.flush()
  pause=min(a.interval,max(0,a.seconds-(time.monotonic()-start)))
  if pause:time.sleep(pause)
