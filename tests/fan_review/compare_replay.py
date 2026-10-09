#!/usr/bin/env python3
"""Compare independent replay without confusing float display rounding with behavior."""
import csv,sys
with open(sys.argv[1]) as a,open(sys.argv[2]) as b:
 expected=list(csv.DictReader(a));actual=list(csv.DictReader(b))
assert len(expected)==len(actual)
rounding=[]
for x,y in zip(expected,actual):
 assert x.keys()==y.keys()
 for k in x:
  if k=='scenario':assert x[k]==y[k];continue
  tolerance=0.10001 if k=='predicted_c' else 1e-6 if k in ('slope','gain') else 0
  assert abs(float(x[k])-float(y[k]))<=tolerance,(x['scenario'],x['second'],k,x[k],y[k])
  if x[k]!=y[k]:rounding.append((x['scenario'],x['second'],k,x[k],y[k]))
print('PASS',len(actual),'rows; requests/applied output/protection exact; tolerated float differences:',len(rounding))
