import struct, glob, os, math
import rail_scan as rs
def score(cells,pw,ph,fx,fy,bx,by,b,W,H,rb):
    s=0
    for j in range(ph):
        for i in range(pw):
            tx,ty=bx+i,by+j
            if not(0<=tx<W and 0<=ty<H): continue
            m=cells[(ph-1-j if fy else j)*pw+(pw-1-i if fx else i)]
            if m:   # overlay-blocked cell
                bm=struct.unpack_from("<H",b,0x114+ty*rb+tx*6)[0]
                if bm: s+=1     # coincides with base-blocked => 'agreement'
    return s
def wins(hyp_a, hyp_b, skip=("skymaze1","skymaze2","skymaze3","star01","star02","star03","star04","star05","star06","star07","star08","star09","star10")):
    a_w=b_w=tie=0
    per={}
    for p in sorted(glob.glob(rs.ROOT+r"\map\map\*.DMap")):
        name=os.path.basename(p)[:-5]
        if name in skip: continue
        b=open(p,"rb").read()
        if len(b)<0x114: continue
        W,H=struct.unpack_from("<II",b,0x10C)
        if not(0<W<=4096 and 0<H<=4096): continue
        rb=W*6+4; tail=0x114+H*rb
        if len(b)<tail+8: continue
        pc=struct.unpack_from("<I",b,tail)[0]; off=tail+4+pc*12
        if off+4>len(b): continue
        lc=struct.unpack_from("<I",b,off)[0]; off+=4
        aw=bw=0
        for _ in range(lc):
            if off+4>len(b): break
            t=struct.unpack_from("<I",b,off)[0]
            if t not in rs.sizes: break
            if t==1:
                f=b[off+4:off+264].split(b"\0")[0].decode("latin1"); sx,sy=struct.unpack_from("<ii",b,off+264)
                for pw,ph,pdx,pdy,cells in (rs.load_scene(f) or []):
                    bx=sx+pdx-pw+1; by=sy+pdy-ph+1
                    sa=score(cells,pw,ph,*hyp_a,bx,by,b,W,H,rb); sb=score(cells,pw,ph,*hyp_b,bx,by,b,W,H,rb)
                    if sa>sb: aw+=1
                    elif sb>sa: bw+=1
            off+=rs.sizes[t]
        if aw or bw: per[name]=(aw,bw)
        a_w+=aw; b_w+=bw
    return a_w,b_w,per
def p_two_sided(k,n):
    # exact binomial two-sided p for k wins of n at p=0.5
    from math import comb
    tail=sum(comb(n,i) for i in range(0,min(k,n-k)+1))/2**n
    return min(1.0,2*tail)
for label,(ha,hb) in {"identity vs flipY":((0,0),(0,1)),"identity vs flipX":((0,0),(1,0)),"identity vs flipBoth":((0,0),(1,1))}.items():
    a,bw,per=wins(ha,hb)
    n=a+bw
    print(f"{label}: identity wins {a}, other wins {bw} (of {n} decided parts)  p={p_two_sided(min(a,bw),n):.2e}")
    print("   per-map (identity,other):", {k:v for k,v in per.items() if sum(v)>=8})
