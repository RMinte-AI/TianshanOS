from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
f=TTFont('fonts/Quantify-Bold.ttf');gs=f.getGlyphSet();cm=f.getBestCmap();upm=f['head'].unitsPerEm
x=0;d=[]
for ch in 'TianshanOS':
    g=cm[ord(ch)];pen=SVGPathPen(gs,ntos=lambda v:('%.1f'%v).rstrip('0').rstrip('.'))
    tp=TransformPen(pen,(1,0,0,-1,x,0));gs[g].draw(tp);d.append(pen.getCommands());x+=gs[g].width
print(upm,x)
open('wm.txt','w').write(str(upm)+' '+str(x)+'\n'+''.join(d))
print(len(''.join(d)))
