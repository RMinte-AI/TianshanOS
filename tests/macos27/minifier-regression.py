"""Regression checks for the two user-authorized CSS minifier corrections."""
import json,runpy
from pathlib import Path
minify=runpy.run_path('tools/minify_web.py')['minify_css']
cases={'.x:not([type="radio"]) { width: 100%; }':'.x:not([type="radio"]){width:100%}', '.x { margin: calc(60px + 20px); }':'.x{margin:calc(60px + 20px)}', '@supports not (display: grid) { .a {display: block;} }':'@supports not (display:grid){.a{display:block}}', '@media screen and (min-width: 320px) {.a + .b {color: red;}}':'@media screen and (min-width:320px){.a + .b{color:red}}'}
rows=[]
for source,expected in cases.items():
    actual=minify(source);assert actual==expected,(source,actual,expected)
    rows.append({'source':source,'actual':actual,'status':'PASS'})
Path('output/macos27-20260929-card-revision/minifier-regression.json').write_text(json.dumps(rows,indent=2)+'\n')
print('4 CSS syntax regressions PASS')
