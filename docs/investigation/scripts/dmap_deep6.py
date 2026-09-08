"""READ-ONLY. Settle .scene cell ordering by matching non-square parts against their .Part text (all 8 index conventions)."""
import os, re, struct
GD = "F:/Games/Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
def part_txt(name):
    t = rd(os.path.join(GD,"map","ScenePart",name)).decode("latin-1"); m={}
    for g in re.finditer(r"Cell\[(\d+),(\d+)\]=\{(-?\d+),(-?\d+),(-?\d+)\}", t): m[(int(g.group(1)),int(g.group(2)))]=(int(g.group(3)),int(g.group(4)),int(g.group(5)))
    W=int(re.search(r"Width=(\d+)",t).group(1)); H=int(re.search(r"Height=(\d+)",t).group(1)); return W,H,m
def scene_parts(name):
    b=rd(os.path.join(GD,"map","Scene",name)); n=struct.unpack_from("<I",b,0)[0]; p=4; out=[]
    for i in range(n):
        f=cstr(b[p:p+256]); t=cstr(b[p+256:p+320]); hdr=struct.unpack_from("<9i",b,p+320); W,H=hdr[3],hdr[4]
        cells=struct.unpack_from("<%di"%(W*H*3),b,p+356); out.append((t,hdr,W,H,cells)); p=p+356+W*H*12
    return out
convs = {
 "idx=j*W+i (row-major, Cell[i,j])":      lambda i,j,W,H: j*W+i,
 "idx=i*H+j (col-major)":                 lambda i,j,W,H: i*H+j,
 "idx=j*W+(W-1-i) (row-major, x mirrored)":lambda i,j,W,H: j*W+(W-1-i),
 "idx=(H-1-j)*W+i (row-major, y mirrored)":lambda i,j,W,H: (H-1-j)*W+i,
 "idx=(H-1-j)*W+(W-1-i)":                 lambda i,j,W,H: (H-1-j)*W+(W-1-i),
 "idx=(W-1-i)*H+j":                       lambda i,j,W,H: (W-1-i)*H+j,
 "idx=i*H+(H-1-j)":                       lambda i,j,W,H: i*H+(H-1-j),
 "idx=(W-1-i)*H+(H-1-j)":                 lambda i,j,W,H: (W-1-i)*H+(H-1-j),
}
for scene, partfile in [("wbridge.scene","wbridge01.Part"),("wbridge1.scene","wbridge01.Part"),("bridgeA.scene","bridge05.Part"),("bridgeA.scene","bridge07.Part"),("stand08.scene","stand08.Part"),("stand05.scene","stand05.Part")]:
    W,H,txt = part_txt(partfile)
    for title,hdr,w2,h2,cells in scene_parts(scene):
        if (w2,h2)!=(W,H): continue
        res={k:all(cells[3*f(i,j,W,H)]==txt[(i,j)][0] and cells[3*f(i,j,W,H)+2]==txt[(i,j)][2] for i in range(W) for j in range(H)) for k,f in convs.items()}
        hits=[k for k,v in res.items() if v]
        print(f"{scene:16s} part '{title}' {W}x{H} vs {partfile}: MATCH -> {hits if hits else 'none'}")
        if not hits:
            print("   scene masks (raw order, first 2 rows of W):", [cells[3*k] for k in range(min(2*W,len(cells)//3))])
            print("   Part text row j=0:", [txt[(i,0)][0] for i in range(W)], " row j=1:", [txt[(i,1)][0] for i in range(W)])
        break
