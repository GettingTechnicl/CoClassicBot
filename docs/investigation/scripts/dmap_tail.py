"""READ-ONLY .DMap tail analyzer. Reads game map files, prints structure. Writes nothing."""
import json, os, struct, sys, re

GD = r"F:\Games\Classic Conquer 2.0"

def map_file(doc_id):
    with open(os.path.join(GD, "ini", "GameMap.json"), "rb") as f:
        arr = json.loads(f.read().decode("utf-8", "ignore"))
    for e in arr:
        if e.get("DocumentId") == doc_id:
            return e.get("FileName"), e
    return None, None

def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1", "ignore")

def strings(b, minlen=4):
    return [m.group().decode("latin-1") for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, b)]

def analyze(doc_id):
    rel, entry = map_file(doc_id)
    print(f"\n===== map {doc_id}: {entry} =====")
    if not rel:
        print("  (no GameMap.json entry)"); return
    path = os.path.join(GD, rel.replace("/", "\\"))
    print(f"  file: {path}  exists={os.path.exists(path)}")
    if not os.path.exists(path):
        return
    d = open(path, "rb").read()
    print(f"  size: {len(d)} bytes (0x{len(d):X})")
    version, pad = struct.unpack_from("<II", d, 0)
    puzzle = cstr(d[8:8+260])
    w, h = struct.unpack_from("<II", d, 0x10C)
    print(f"  version={version} pad={pad} width={w} height={h}")
    print(f"  puzzlePath[260] @0x08 = {puzzle!r}")
    row = w*6 + 4
    cells_end = 0x114 + h*row
    print(f"  cell grid: 0x114 .. 0x{cells_end:X}  ({h*row} bytes)")
    tail = d[cells_end:]
    print(f"  TAIL after cell grid: {len(tail)} bytes (0x{len(tail):X})  <-- what the bot's parser skips")
    if not tail:
        return

    # hex head of tail
    print("  tail hex (first 0x80):")
    for off in range(0, min(len(tail), 0x80), 16):
        chunk = tail[off:off+16]
        print(f"    +0x{off:04X}: " + " ".join(f"{b:02X}" for b in chunk) + "  " + "".join(chr(b) if 32 <= b < 127 else "." for b in chunk))

    print("  ASCII strings in tail:")
    for s in strings(tail):
        print(f"    {s!r}")

    # attempt structured decode (TQ DMap: portals then layers)
    p = 0
    def u32():
        nonlocal p
        v = struct.unpack_from("<I", tail, p)[0]; p += 4; return v
    def i32():
        nonlocal p
        v = struct.unpack_from("<i", tail, p)[0]; p += 4; return v
    try:
        n_portal = u32()
        print(f"  [decode] portalCount={n_portal}")
        if n_portal < 1000:
            for i in range(n_portal):
                x, y, pid = i32(), i32(), i32()
                if i < 15: print(f"     portal[{i}] x={x} y={y} id={pid}")
        n_layer = u32()
        print(f"  [decode] layerCount={n_layer}  (tail offset now 0x{p:X})")
        for i in range(min(n_layer, 200)):
            t = u32()
            start = p
            if t == 1:      # terrain/scene overlay: file[260], x, y
                fn = cstr(tail[p:p+260]); p += 260; x, y = i32(), i32()
                print(f"     layer[{i}] type=1 SCENE file={fn!r} at ({x},{y})")
            elif t == 4:    # sound: file[260], x, y, vol, range
                fn = cstr(tail[p:p+260]); p += 260; x, y, vol, rng = i32(), i32(), i32(), i32()
                print(f"     layer[{i}] type=4 SOUND file={fn!r} at ({x},{y}) vol={vol} range={rng}")
            elif t == 10:   # 3d effect: file[64], x, y
                fn = cstr(tail[p:p+64]); p += 64; x, y = i32(), i32()
                print(f"     layer[{i}] type=10 3DEFFECT file={fn!r} at ({x},{y})")
            elif t == 15:   # 3d effect new: file[64], x, y, z?
                fn = cstr(tail[p:p+64]); p += 64; x, y, z = i32(), i32(), i32()
                print(f"     layer[{i}] type=15 3DEFFECTNEW file={fn!r} at ({x},{y},{z})")
            else:
                print(f"     layer[{i}] type={t} UNKNOWN at tail+0x{start-4:X} -> stopping structured decode")
                print("     next 64 bytes: " + " ".join(f"{b:02X}" for b in tail[start:start+64]))
                break
        print(f"  [decode] consumed 0x{p:X} of 0x{len(tail):X} tail bytes; remaining={len(tail)-p}")
    except Exception as e:
        print(f"  [decode] stopped: {e} at tail+0x{p:X}")

for mid in [int(a) for a in sys.argv[1:]] or [1010, 1002]:
    analyze(mid)
