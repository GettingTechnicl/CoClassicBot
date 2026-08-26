import frida, sys

# Locate the ACTIVE CGameMap on v1074.
#
# Session-6 recorded that a genuine CGameMap carries vtable RVA 0x5CCB60
# (verified 8/8 slots point to code) with a DocumentId u32 at object +0x10.
# The static pointer at 0x699370 tracks the active map but is NOT itself a
# CGameMap (its vtable is not in the image) -- so this walks outward from it
# looking for an object that DOES carry the CGameMap vtable, and separately
# heap-scans for all CGameMap instances so the active one can be identified.
#
# usage: python frida_find_gamemap.py <pid> <expected_map_id>

pid = int(sys.argv[1])
expected = int(sys.argv[2]) if len(sys.argv) > 2 else 1002

session = frida.get_local_device().attach(pid)
script = session.create_script(r'''
const base = Process.mainModule.base;
const size = Process.mainModule.size;
const MAP_VT = base.add(0x5CCB60);

function readable(p) { try { p.readU8(); return true; } catch (e) { return false; } }
function inImage(p) {
    try { return !p.isNull() && p.compare(base) >= 0 && p.compare(base.add(size)) < 0; }
    catch (e) { return false; }
}
function isGameMap(p) {
    try {
        if (p.isNull() || !readable(p)) return false;
        return p.readPointer().equals(MAP_VT);
    } catch (e) { return false; }
}
function dumpMap(p) {
    const u32 = [];
    for (let off = 0; off < 0x240; off += 4) {
        try { u32.push(p.add(off).readU32()); } catch (e) { u32.push(null); }
    }
    const ptrs = [];
    for (let off = 0; off < 0x240; off += 8) {
        try {
            const q = p.add(off).readPointer();
            if (!q.isNull() && !inImage(q) && readable(q))
                ptrs.push(['0x' + off.toString(16), q.toString()]);
        } catch (e) {}
    }
    return { obj: p.toString(), u32: u32, ptrs: ptrs };
}

rpc.exports = {
    // Walk the scene object's pointer fields (2 levels) for a CGameMap.
    fromScene() {
        const hits = [];
        let scene;
        try { scene = base.add(0x699370).readPointer(); } catch (e) { return hits; }
        if (scene.isNull() || !readable(scene)) return hits;

        for (let off = 0; off < 0x400; off += 8) {
            let q;
            try { q = scene.add(off).readPointer(); } catch (e) { continue; }
            if (q.isNull() || inImage(q) || !readable(q)) continue;
            if (isGameMap(q)) { hits.push({ path: 'scene+0x' + off.toString(16), obj: q.toString() }); continue; }
            for (let off2 = 0; off2 < 0x200; off2 += 8) {
                let r;
                try { r = q.add(off2).readPointer(); } catch (e) { continue; }
                if (r.isNull() || inImage(r) || !readable(r)) continue;
                if (isGameMap(r))
                    hits.push({ path: 'scene+0x' + off.toString(16) + '->+0x' + off2.toString(16), obj: r.toString() });
            }
        }
        return hits;
    },

    // Heap-scan for every object carrying the CGameMap vtable.
    // Uses Memory.scanSync over enumerated rw- ranges: a manual per-address
    // JS loop is orders of magnitude slower and times out (session 5c).
    scanAll() {
        const found = [];
        // little-endian byte pattern of the CGameMap vtable pointer
        const bytes = MAP_VT.toMatchPattern();
        const ranges = Process.enumerateRanges({ protection: 'rw-', coalesce: false });
        for (const r of ranges) {
            if (r.size < 0x240 || r.size > 0x4000000) continue;
            let matches;
            try { matches = Memory.scanSync(r.base, r.size, bytes); } catch (e) { continue; }
            for (const m of matches) {
                if (found.length >= 64) break;
                if (isGameMap(m.address)) found.push(dumpMap(m.address));
            }
            if (found.length >= 64) break;
        }
        return found;
    },

    dump(addrStr) { return dumpMap(ptr(addrStr)); }
};
''')
script.load()
api = script.exports_sync

print('=== CGameMap objects reachable from the scene pointer (0x699370) ===')
for h in api.from_scene():
    print(f"  {h['path']}  ->  {h['obj']}")

print('\n=== heap scan for all CGameMap instances (vtable RVA 0x5CCB60) ===')
maps = api.scan_all()
print(f'found {len(maps)}')

for m in maps:
    u = m['u32']
    ids = [(i * 4, v) for i, v in enumerate(u) if v is not None and 100 <= v <= 2000]
    star = ' <== EXPECTED' if any(v == expected for _, v in ids) else ''
    print(f"\n  obj {m['obj']}{star}")
    print('    id-like fields: ' + ', '.join(f'+0x{o:03X}={v}' for o, v in ids[:12]))
    dims = []
    for i in range(len(u) - 1):
        a, b = u[i], u[i + 1]
        if a and b and 32 <= a <= 8000 and 32 <= b <= 8000:
            dims.append(f'+0x{i*4:03X}=({a},{b})')
    print('    dim-like pairs: ' + (', '.join(dims[:8]) or '(none)'))
    print('    heap ptrs: ' + ', '.join(f'+{o}' for o, _ in m['ptrs'][:10]))

session.detach()
