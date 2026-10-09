"""Collector regression; all HTTP is mocked, no device/network access."""
import json,runpy,sys,tempfile
from pathlib import Path
from unittest.mock import patch
clock=0
calls=[]
def monotonic():return clock
def sleep(seconds):
 global clock
 clock+=seconds
class Reply:
 def __init__(self,body):self.body=body
 def __enter__(self):return self
 def __exit__(self,*args):pass
 def read(self,limit):return json.dumps(self.body).encode()[:limit]
def read(request,timeout):
 calls.append(request)
 assert request.get_method()=='GET'
 if request.full_url.endswith('/temp/status'):
  return Reply({'code':0,'data':{'current_timestamp_ms':1000}} if clock!=1 else {'code':9})
 return Reply({'code':0,'data':{'fans':[{'id':0,'duty':37,'target_duty':70,'duty_valid':True}]}})
with tempfile.TemporaryDirectory() as tmp:
 output=Path(tmp)/'capture.jsonl'
 with patch.object(sys,'argv',['capture','--base-url','http://fixture.invalid','--seconds','3','--output',str(output)]),patch('time.monotonic',monotonic),patch('time.sleep',sleep),patch('urllib.request.urlopen',read):
  runpy.run_path(str(Path(__file__).with_name('capture_snapshots.py')),run_name='__main__')
 rows=[json.loads(line) for line in output.read_text().splitlines()]
 assert len(rows)==3 and len(calls)==6
 assert [r['sensor_report_changed'] for r in rows]==[True,None,False]
 assert all(r['snapshot_only'] for r in rows)
 print('PASS GET-only bounded mock capture: a missing observation stays unknown; a repeated report after the gap is not counted as new; no real network/device access')
