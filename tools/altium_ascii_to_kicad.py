# -*- coding: utf-8 -*-
import re,sys
SRC='/tmp/board.pcbdoc'; OUT=sys.argv[1] if len(sys.argv)>1 else '/tmp/out.kicad_pcb'
d=open(SRC,'r',encoding='latin-1').read()
recs=[r for r in d.split('|RECORD=') if r and not r.startswith('\n')]
def F(r):
    o={}
    for kv in r.split('|'):
        if '=' in kv:
            k,v=kv.split('=',1); o[k.strip()]=v.strip()
    return o
def mil(v):
    if v is None: return 0.0
    v=v.replace('mil','').strip()
    try: return float(v)
    except: return 0.0
recf=[(r.split('|',1)[0].strip(),F(r)) for r in recs]
# 网络: Net记录顺序=0基索引
nets=[f.get('NAME','net%d'%i) for i,(t,f) in enumerate(recf) if t=='Net']
# 坐标范围(找Y翻转基准)用所有 track/via/pad 的 mil 坐标
ys=[]; 
for t,f in recf:
    for k in ('Y','Y1','Y2'):
        if k in f: ys.append(mil(f[k]))
YMAX=max(ys) if ys else 0
MM=0.0254
def X(v): return round(mil(v)*MM,4)
def Y(v): return round((YMAX-mil(v))*MM,4)   # Y翻转
LAY={'TOP':'F.Cu','BOTTOM':'B.Cu','TOPOVERLAY':'F.SilkS','BOTTOMOVERLAY':'B.SilkS',
     'TOPPASTE':'F.Paste','BOTTOMPASTE':'B.Paste','TOPSOLDER':'F.Mask','BOTTOMSOLDER':'B.Mask',
     'MECHANICAL1':'Edge.Cuts','KEEPOUT':'Edge.Cuts','MULTILAYER':'F.Cu'}
def lay(a,default='Dwgs.User'): return LAY.get(a,default)
netidx={}  # altium net str -> kicad netcode
def kcnet(s):
    try: i=int(s)
    except: return 0
    if 0<=i<len(nets): return i+1
    return 0
out=[]
out.append('(kicad_pcb (version 20221018) (generator altium2kicad)')
out.append('  (general (thickness 1.6))')
out.append('  (paper "A3")')
out.append('  (layers')
klayers=[(0,'F.Cu','signal'),(31,'B.Cu','signal'),(36,'B.SilkS','user'),(37,'F.SilkS','user'),
 (38,'B.Paste','user'),(39,'F.Paste','user'),(40,'B.Mask','user'),(41,'F.Mask','user'),
 (44,'Edge.Cuts','user'),(45,'Margin','user'),(46,'B.CrtYd','user'),(47,'F.CrtYd','user'),(48,'B.Fab','user'),(49,'F.Fab','user'),(50,'Dwgs.User','user')]
for n,nm,ty in klayers: out.append('    (%d "%s" %s)'%(n,nm,ty))
out.append('  )')
out.append('  (setup (pad_to_mask_clearance 0))')
# 网络
out.append('  (net 0 "")')
for i,nm in enumerate(nets):
    safe=nm.replace('"','').replace('(','').replace(')','')
    out.append('  (net %d "%s")'%(i+1,safe))
# 板框: Board记录的 VX/VY 顶点 → Edge.Cuts 多段线
for t,f in recf:
    if t=='Board':
        pts=[]
        i=0
        while ('VX%d'%i) in f:
            pts.append((mil(f['VX%d'%i]),mil(f['VY%d'%i]))); i+=1
        for j in range(len(pts)-1):
            (x1,y1),(x2,y2)=pts[j],pts[j+1]
            out.append('  (gr_line (start %.4f %.4f) (end %.4f %.4f) (layer "Edge.Cuts") (width 0.15))'%(x1*MM,(YMAX-y1)*MM,x2*MM,(YMAX-y2)*MM))
        break
# 走线
nt=nv=npad=0
for t,f in recf:
    if t=='Track':
        L=lay(f.get('LAYER',''))
        w=max(mil(f.get('WIDTH','5'))*MM,0.05)
        if L in ('F.Cu','B.Cu'):
            out.append('  (segment (start %s %s) (end %s %s) (width %.4f) (layer "%s") (net %d))'%(X(f.get('X1')),Y(f.get('Y1')),X(f.get('X2')),Y(f.get('Y2')),w,L,kcnet(f.get('NET','0'))));nt+=1
        else:
            out.append('  (gr_line (start %s %s) (end %s %s) (layer "%s") (width %.4f))'%(X(f.get('X1')),Y(f.get('Y1')),X(f.get('X2')),Y(f.get('Y2')),L,w))
    elif t=='Via':
        dia=max(mil(f.get('DIAMETER','20'))*MM,0.2); drl=max(mil(f.get('HOLESIZE','10'))*MM,0.1)
        out.append('  (via (at %s %s) (size %.4f) (drill %.4f) (layers "F.Cu" "B.Cu") (net %d))'%(X(f.get('X')),Y(f.get('Y')),dia,drl,kcnet(f.get('NET','0'))));nv+=1
# 焊盘 → 每个一个 footprint
for t,f in recf:
    if t=='Pad':
        x=X(f.get('X')); y=Y(f.get('Y')); xs=max(mil(f.get('XSIZE','50'))*MM,0.1); yssz=max(mil(f.get('YSIZE','50'))*MM,0.1)
        hole=mil(f.get('HOLESIZE','0'))*MM
        shp={'ROUND':'circle','RECTANGLE':'rect','RECT':'rect','OCTAGONAL':'roundrect','ROUNDED':'roundrect'}.get(f.get('SHAPE','ROUND').upper(),'circle')
        rot=f.get('ROTATION','0')
        nv_=f.get('NET','0'); net=kcnet(nv_); nm=nets[int(nv_)] if nv_.isdigit() and 0<=int(nv_)<len(nets) else ''
        padlay= '"*.Cu" "*.Mask"' if hole>0 or f.get('LAYER')=='MULTILAYER' else ('"F.Cu" "F.Mask"' if f.get('LAYER')=='TOP' else '"B.Cu" "B.Mask"')
        ptype='thru_hole' if hole>0 else 'smd'
        out.append('  (footprint "pad:P" (layer "F.Cu") (at %s %s)'%(x,y))
        if hole>0:
            out.append('    (pad "%s" %s %s (at 0 0 %s) (size %.4f %.4f) (drill %.4f) (layers %s) (net %d "%s"))'%(f.get('NAME','1'),ptype,shp,rot,xs,yssz,max(hole,0.1),padlay,net,nm.replace('"','')))
        else:
            out.append('    (pad "%s" %s %s (at 0 0 %s) (size %.4f %.4f) (layers %s) (net %d "%s"))'%(f.get('NAME','1'),ptype,shp,rot,xs,yssz,padlay,net,nm.replace('"','')))
        out.append('  )'); npad+=1
out.append(')')
open(OUT,'w').write('\n'.join(out))
print('tracks=%d vias=%d pads=%d nets=%d → %s'%(nt,nv,npad,len(nets),OUT))
