"""READ-ONLY. Confirm bottom-right anchoring against Twin City river; verify cell ordering on a non-square part."""
import os, re, struct
GD = "F:/Games/Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
d = rd(os.path.join(GD,"map","map","newplain.DMap")); w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
def base(x,y): return 9 if not (0<=x<w and 0<=y<h) else struct.unpack_from("<H", d, 0x114+y*row+x*6)[0]

# scene parts of bridgeA with (dx,dy,W,H)
b = rd(os.path.join(GD,"map","Scene","bridgeA.scene")); n=struct.unpack_from("<I",b,0)[0]; p=4; parts=[]
for i in range(n):
    hdr=struct.unpack_from("<9i", b, p+320); W,H=hdr[3],hdr[4]
    cells=struct.unpack_from("<%di"%(W*H*3), b, p+356); parts.append((hdr[6],hdr[7],W,H,cells)); p=p+356+W*H*12
ax,ay=648,682
print("== bridgeA parts (dx,dy,W,H) and predicted footprint under BOTTOM-RIGHT anchor: x in [ax+dx-W+1, ax+dx], y in [ay+dy-H+1, ay+dy] ==")
foot={}
for dx,dy,W,H,cells in parts:
    x1,x0 = ax+dx, ax+dx-W+1; y1,y0 = ay+dy, ay+dy-H+1
    print(f"   dx={dx:4d} dy={dy} {W}x{H} -> x[{x0}..{x1}] y[{y0}..{y1}]")
    for j in range(H):
        for i in range(W):
            # candidate mapping: Cell[i,j] -> (x0+i, y0+j)  (row-major, index j*W+i)
            foot[(x0+i,y0+j)] = cells[3*(j*W+i)]
print("\n== Twin City base grid x=596..652 (cols) by y=668..688 (rows): '.'=walkable '#'=blocked; overlay: 'o'=bridge cell mask0, 'x'=bridge cell mask1, 'A'=anchor ==")
print("         " + "".join(str(x%10) for x in range(596,653)))
for y in range(668,689):
    s=""
    for x in range(596,653):
        if (x,y)==(ax,ay): s+="A"
        elif (x,y) in foot: s+= "o" if foot[(x,y)]==0 else "x"
        else: s+= "." if base(x,y)==0 else "#"
    print(f"  y={y}  "+s)

# bridgeB-L at (196,548): just show raw base grid window (no part offsets computed here)
bx,by=196,548
b2 = rd(os.path.join(GD,"map","Scene","bridgeB-L.scene")); n2=struct.unpack_from("<I",b2,0)[0]; p=4; parts2=[]
for i in range(n2):
    hdr=struct.unpack_from("<9i", b2, p+320); W,H=hdr[3],hdr[4]; parts2.append((hdr[6],hdr[7],W,H)); p=p+356+W*H*12
print(f"\n== bridgeB-L.scene parts (dx,dy,W,H): {parts2}  anchor=({bx},{by}) ==")
foot2=set()
for dx,dy,W,H in parts2:
    for i in range(W):
        for j in range(H): foot2.add((bx+dx-W+1+i, by+dy-H+1+j))
xs=[x for x,_ in foot2]; ys=[y for _,y in foot2]
print(f"   predicted footprint x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}]")
print("         " + "".join(str(x%10) for x in range(min(xs)-4, max(xs)+5)))
for y in range(min(ys)-3, max(ys)+4):
    print(f"  y={y}  " + "".join("A" if (x,y)==(bx,by) else ("o" if (x,y) in foot2 else ("." if base(x,y)==0 else "#")) for x in range(min(xs)-4, max(xs)+5)))

# cell ordering check: bridge05 part (5x9) in bridgeA.scene vs bridge05.Part text
t = rd(os.path.join(GD,"map","ScenePart","bridge05.Part")).decode("latin-1")
txt={}
for m in re.finditer(r"Cell\[(\d+),(\d+)\]=\{(-?\d+),(-?\d+),(-?\d+)\}", t): txt[(int(m.group(1)),int(m.group(2)))]=int(m.group(3))
for dx,dy,W,H,cells in parts:
    if (W,H)==(5,9):
        rm = all(cells[3*(j*W+i)]==txt.get((i,j)) for i in range(W) for j in range(H))
        cm = all(cells[3*(i*H+j)]==txt.get((i,j)) for i in range(W) for j in range(H))
        print(f"\n== cell ordering (bridge05 5x9): binary index j*W+i == Part Cell[i,j]? {rm};  index i*H+j == Cell[i,j]? {cm} ==")
        break
