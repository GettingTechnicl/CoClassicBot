"""READ-ONLY. Does the bridgeB-L overlay on map 1002 cover the user's tile (167,543)? Show which part/cell and its mask."""
import os, struct
GD = "F:/Games/Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
d = rd(os.path.join(GD,"map","map","newplain.DMap")); w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
def base(x,y): return 9 if not (0<=x<w and 0<=y<h) else struct.unpack_from("<H", d, 0x114+y*row+x*6)[0]
b = rd(os.path.join(GD,"map","Scene","bridgeB-L.scene")); n=struct.unpack_from("<I",b,0)[0]; p=4; parts=[]
for i in range(n):
    title=cstr(b[p+256:p+320]); hdr=struct.unpack_from("<9i",b,p+320); W,H=hdr[3],hdr[4]
    cells=struct.unpack_from("<%di"%(W*H*3),b,p+356); parts.append((title,hdr[0],hdr[1],hdr[6],hdr[7],W,H,cells)); p=p+356+W*H*12
ax,ay=196,548
TX,TY=167,543
print(f"bridgeB-L.scene at ({ax},{ay}): {n} parts")
print(" part# title        pixOffX pixOffY  cellDX cellDY  W x H   footprint x[..]  y[..]")
foot={}
for k,(title,px,py,dx,dy,W,H,cells) in enumerate(parts):
    x0,x1=ax+dx-W+1, ax+dx; y0,y1=ay+dy-H+1, ay+dy
    print(f"  {k}   {title:12s} {px:7d} {py:7d}  {dx:6d} {dy:6d}  {W}x{H}   x[{x0}..{x1}] y[{y0}..{y1}]")
    for j in range(H):
        for i in range(W):
            foot[(x0+i,y0+j)]=(k,i,j,cells[3*(j*W+i)])
xs=sorted(set(x for x,_ in foot)); print(f"\nUNION footprint x[{xs[0]}..{xs[-1]}], contiguous columns with no gaps/overlaps? {len(xs)==xs[-1]-xs[0]+1 and all(sum(1 for (x,_) in foot if x==c)==9 for c in xs)}")
print(f"\nUser tile ({TX},{TY}): base mask={base(TX,TY)}  ->", end=" ")
if (TX,TY) in foot:
    k,i,j,m=foot[(TX,TY)]; print(f"COVERED by part {k} ('{parts[k][0]}', cellDX={parts[k][3]}) at part cell ({i},{j}) with overlay mask={m}  => {'WALKABLE (bridge deck)' if m==0 else 'blocked (rail)'}")
else: print("NOT covered")
print("\nmask-annotated grid x=137..200, y=537..551 ('.'=base walkable, '#'=base blocked, 'o'=overlay deck mask0, 'x'=overlay rail mask1, 'U'=user tile, 'A'=anchor):")
print("         "+"".join(str(x%10) for x in range(137,201)))
for y in range(537,552):
    s=""
    for x in range(137,201):
        if (x,y)==(TX,TY): s+="U"
        elif (x,y)==(ax,ay): s+="A"
        elif (x,y) in foot: s+="o" if foot[(x,y)][3]==0 else "x"
        else: s+="." if base(x,y)==0 else "#"
    print(f"  y={y}  {s}")
