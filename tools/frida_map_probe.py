import frida, sys, json, time

# Read-only probe for the active CGameMap object on v1074.
#
# Session-6 finding: the active map is reached through a STATIC POINTER at
# RVA 0x699370 (verified by pointer-differential across a map change), NOT as
# a static object at Offsets::GAME_MAP (0x4E02E0), which the bot still uses.
# The object's FIELD offsets were never confirmed, so this dumps the head of
# the object and flags plausible candidates for:
#   - map id      (should match a DocumentId in data_maps.json, e.g. 1002)
#   - dimensions  (two ints, a few hundred to a few thousand)
#   - cell array  (a heap pointer)
#
# usage: python frida_map_probe.py <pid> [expected_map_id]

pid = int(sys.argv[1])
expected = int(sys.argv[2]) if len(sys.argv) > 2 else None

session = frida.get_local_device().attach(pid)
script = session.create_script(r'''
const base = Process.mainModule.base;
const size = Process.mainModule.size;

function inImage(p) {
    try { return !p.isNull() && p.compare(base) >= 0 && p.compare(base.add(size)) < 0; }
    catch (e) { return false; }
}
function readable(p) {
    try { p.readU8(); return true; } catch (e) { return false; }
}

rpc.exports = {
    probe(candidates) {
        const out = [];
        for (const rva of candidates) {
            let entry = { rva: '0x' + rva.toString(16), ptr: null, vtable: null, u32: [], ptrs: [] };
            try {
                const p = base.add(rva).readPointer();
                entry.ptr = p.toString();
                if (p.isNull() || !readable(p)) { out.push(entry); continue; }

                const vt = p.readPointer();
                entry.vtable = vt.toString();
                entry.vtableRva = inImage(vt) ? '0x' + vt.sub(base).toString(16) : null;

                for (let off = 0; off < 0x220; off += 4)
                    entry.u32.push(p.add(off).readU32());

                // any 8-byte-aligned slot holding a readable heap pointer
                for (let off = 0; off < 0x220; off += 8) {
                    try {
                        const q = p.add(off).readPointer();
                        if (!q.isNull() && !inImage(q) && readable(q))
                            entry.ptrs.push(['0x' + off.toString(16), q.toString()]);
                    } catch (e) {}
                }
            } catch (e) { entry.err = e.toString(); }
            out.push(entry);
        }
        return out;
    }
};
''')
script.load()

# 0x699370 = session-6 primary candidate, 0x6993B8 = the recorded runner-up,
# 0x4E02E0 = what the bot currently uses (expected to be wrong).
res = script.exports_sync.probe([0x699370, 0x6993B8, 0x4E02E0])

for e in res:
    print(f"\n===== static ptr RVA {e['rva']} -> {e['ptr']} =====")
    if e.get('err'):
        print('  err:', e['err']); continue
    if not e['u32']:
        print('  (unreadable / null)'); continue
    print(f"  vtable: {e['vtable']}  rva={e.get('vtableRva')}")

    u = e['u32']
    print('  --- candidate MAP ID fields (100..2000, matches data_maps range) ---')
    for i, v in enumerate(u):
        if 100 <= v <= 2000:
            tag = '  <== MATCHES EXPECTED' if (expected and v == expected) else ''
            print(f'    +0x{i*4:03X} = {v}{tag}')

    print('  --- candidate DIMENSION pairs (two adjacent plausible ints) ---')
    for i in range(len(u) - 1):
        a, b = u[i], u[i + 1]
        if 32 <= a <= 8000 and 32 <= b <= 8000:
            print(f'    +0x{i*4:03X} = ({a}, {b})')

    print('  --- heap pointers (cell array candidates) ---')
    for off, q in e['ptrs'][:14]:
        print(f'    +{off} -> {q}')

session.detach()
print('\nNote: compare the map-id candidates against data_maps.json for the map you are standing in.')
