import frida, sys, json

# Read-only probe for ImConquer.exe's login screen text-field buffers.
#
# SAFETY: run this ONLY while sitting on the login screen with something
# typed into a field, BEFORE clicking Login. The server's integrity check
# reacts to what happens around the login/authentication handshake — this
# scan is purely passive (no writes, no hooks) and is meant to be detached
# again before Login is ever clicked, so the actual authentication proceeds
# through the already-proven external automation with nothing attached.
#
# Phase 1 usage: type a distinctive marker string (NOT your real username)
# into the username field, then run:
#   python frida_login_probe.py <pid> scan <marker>
# This finds every occurrence of the marker in readable/writable memory, as
# both ASCII and UTF-16, and reports the containing region (module image vs
# heap) so we can tell a real per-instance text buffer from an unrelated
# coincidental match.
#
# Phase 2 usage: once a real buffer address looks promising, find what
# object points AT it (the field's owning struct) via:
#   python frida_login_probe.py <pid> scanptr <addr-hex>
# This searches all rw- memory for that exact address value stored as a
# pointer, i.e. "who references this buffer."

pid = int(sys.argv[1])
mode = sys.argv[2]

session = frida.get_local_device().attach(pid)
script = session.create_script(r'''
const base = Process.mainModule.base;
const size = Process.mainModule.size;

function inImage(p) {
    try { return !p.isNull() && p.compare(base) >= 0 && p.compare(base.add(size)) < 0; }
    catch (e) { return false; }
}

function toHexPattern(str, wide) {
    const bytes = [];
    for (let i = 0; i < str.length; i++) {
        const code = str.charCodeAt(i);
        bytes.push(code.toString(16).padStart(2, '0'));
        if (wide) bytes.push('00');
    }
    return bytes.join(' ');
}

function enumRw() {
    try { return Process.enumerateRanges({ protection: 'rw-', coalesce: true }); }
    catch (e) { return Process.enumerateRanges('rw-'); }
}

function enumReadable() {
    const out = enumRw();
    try {
        for (const r of Process.enumerateRanges({ protection: 'r--', coalesce: true }))
            out.push(r);
    } catch (e) {}
    return out;
}

rpc.exports = {
    scan(marker) {
        const patterns = [
            { name: 'ascii', pattern: toHexPattern(marker, false) },
            { name: 'utf16', pattern: toHexPattern(marker, true) },
        ];
        const results = [];
        for (const r of enumReadable()) {
            for (const p of patterns) {
                let matches;
                try { matches = Memory.scanSync(r.base, r.size, p.pattern); }
                catch (e) { continue; }
                for (const m of matches) {
                    results.push({
                        addr: m.address.toString(),
                        encoding: p.name,
                        regionBase: r.base.toString(),
                        regionSize: r.size,
                        regionProtection: r.protection,
                        inModuleImage: inImage(m.address),
                    });
                }
            }
        }
        return results;
    },

    scanptr(addrHex) {
        const target = ptr(addrHex);
        const targetHex = target.toString();
        const results = [];
        for (const r of enumRw()) {
            const end = r.base.add(r.size);
            let cursor = r.base;
            // pointer-aligned scan
            while (cursor.compare(end.sub(8)) < 0) {
                try {
                    const v = cursor.readPointer();
                    if (v.equals(target)) {
                        results.push({
                            addr: cursor.toString(),
                            regionBase: r.base.toString(),
                            regionProtection: r.protection,
                            inModuleImage: inImage(cursor),
                        });
                    }
                } catch (e) { break; }
                cursor = cursor.add(8);
            }
        }
        return results;
    }
};
''')
script.load()

if mode == 'scan':
    marker = sys.argv[3]
    res = script.exports_sync.scan(marker)
    print(f'Found {len(res)} match(es) for "{marker}":\n')
    for m in res:
        tag = 'MODULE IMAGE (static/global — likely not per-instance)' if m['inModuleImage'] else 'heap/other'
        print(f"  {m['addr']}  [{m['encoding']}]  region={m['regionBase']} size=0x{m['regionSize']:x} "
              f"prot={m['regionProtection']}  ({tag})")
    print('\nNext: pick a non-module-image address above and run scanptr on it to find the owning object.')

elif mode == 'scanptr':
    addr = sys.argv[3]
    res = script.exports_sync.scanptr(addr)
    print(f'Found {len(res)} pointer(s) to {addr}:\n')
    for m in res:
        tag = ' (in module image — could be a static field-pointer table)' if m['inModuleImage'] else ''
        print(f"  {m['addr']}  region={m['regionBase']} prot={m['regionProtection']}{tag}")

else:
    print(f'Unknown mode "{mode}" — use "scan <marker>" or "scanptr <addr-hex>"')

session.detach()
print('\nDetached.')
