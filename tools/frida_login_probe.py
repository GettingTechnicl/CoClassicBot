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
# Recommended usage — one shot, walks the pointer chain automatically:
#   python frida_login_probe.py <pid> chase <marker> [maxDepth=5]
# Type a distinctive marker string (NOT your real username) into the
# username field, then run this. It finds every occurrence of the marker
# (ASCII + UTF-16), then for each heap hit repeatedly asks "what points at
# this address" and follows that pointer upward, stopping early if it
# reaches something inside the module image (a static/global anchor — what
# we actually need for a stable production implementation, since heap
# addresses are different every run) or hits a dead end.
#
# Manual step-by-step alternative (slower, more visibility into ties):
#   python frida_login_probe.py <pid> scan <marker>
#   python frida_login_probe.py <pid> scanptr <addr-hex>

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

// 8-byte little-endian hex pattern for a pointer value, for fast native
// Memory.scanSync-based "who stores this address" lookups instead of a
// slow manual per-8-bytes readPointer() loop.
function ptrToLEPattern(p) {
    let hex = p.toString();
    if (hex.startsWith('0x')) hex = hex.slice(2);
    hex = hex.padStart(16, '0');
    const bytes = [];
    for (let i = hex.length - 2; i >= 0; i -= 2)
        bytes.push(hex.substr(i, 2));
    return bytes.join(' ');
}

function isAligned8(p) {
    return p.and(ptr(7)).isNull();
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

function scanMarker(marker) {
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
                    address: m.address,
                    encoding: p.name,
                    regionBase: r.base,
                    regionSize: r.size,
                    regionProtection: r.protection,
                    inModuleImage: inImage(m.address),
                });
            }
        }
    }
    return results;
}

function findPointersTo(target) {
    const pattern = ptrToLEPattern(target);
    const results = [];
    for (const r of enumRw()) {
        let matches;
        try { matches = Memory.scanSync(r.base, r.size, pattern); }
        catch (e) { continue; }
        for (const m of matches) {
            if (!isAligned8(m.address)) continue;  // reduce unaligned coincidental-byte false positives
            results.push({ addr: m.address, inModuleImage: inImage(m.address) });
        }
    }
    return results;
}

rpc.exports = {
    scan(marker) {
        return scanMarker(marker).map(m => ({
            addr: m.address.toString(),
            encoding: m.encoding,
            regionBase: m.regionBase.toString(),
            regionSize: m.regionSize,
            regionProtection: m.regionProtection,
            inModuleImage: m.inModuleImage,
        }));
    },

    scanptr(addrHex) {
        const target = ptr(addrHex);
        return findPointersTo(target).map(p => ({
            addr: p.addr.toString(),
            inModuleImage: p.inModuleImage,
        }));
    },

    scanrange(loHex, hiHex) {
        const lo = ptr(loHex);
        const hi = ptr(hiHex);
        const results = [];
        for (const r of enumReadable()) {
            const end = r.base.add(r.size);
            let cursor = r.base;
            while (cursor.compare(end.sub(8)) < 0) {
                try {
                    const v = cursor.readPointer();
                    if (v.compare(lo) >= 0 && v.compare(hi) < 0) {
                        results.push({
                            addr: cursor.toString(),
                            value: v.toString(),
                            inModuleImage: inImage(cursor),
                        });
                    }
                } catch (e) { break; }
                cursor = cursor.add(8);
            }
        }
        return results;
    },

    dump(addrHex, before, after) {
        const center = ptr(addrHex);
        const start = center.sub(before);
        const out = { center: center.toString(), start: start.toString(), u32: [], ptrs: [], bytes: null };
        try { out.bytes = Array.from(new Uint8Array(start.readByteArray(before + after))); }
        catch (e) { out.err = e.toString(); return out; }
        for (let off = 0; off < before + after; off += 4) {
            try { out.u32.push(start.add(off).readU32()); }
            catch (e) { out.u32.push(null); }
        }
        for (let off = 0; off < before + after - 8; off += 8) {
            try {
                const q = start.add(off).readPointer();
                if (!q.isNull())
                    out.ptrs.push([off - before, q.toString(), inImage(q)]);
            } catch (e) {}
        }
        return out;
    },

    // Page-guard watchpoint: arms MemoryAccessMonitor on the 4KB page(s)
    // containing the given addresses, signals "armed" back to the host so
    // it can prompt for a live keystroke, then resolves with the FIRST
    // access detected — including the faulting instruction's address (the
    // actual game code that performs the write). This does not patch any
    // game code; it temporarily restricts page permissions and catches the
    // resulting guard-page exception, the same non-invasive technique
    // described for page-guard-based instrumentation generally.
    watch(addrsHex, timeoutMs) {
        return new Promise((resolve) => {
            const pageMask = ptr('0xFFFFFFFFFFFFF000');
            const pages = [];
            const seen = new Set();
            for (const a of addrsHex) {
                const pg = ptr(a).and(pageMask);
                const key = pg.toString();
                if (!seen.has(key)) { seen.add(key); pages.push({ base: pg, size: 0x1000 }); }
            }

            let done = false;
            const timer = setTimeout(() => {
                if (done) return;
                done = true;
                try { MemoryAccessMonitor.disable(); } catch (e) {}
                resolve({ timedOut: true });
            }, timeoutMs);

            MemoryAccessMonitor.enable(pages, {
                onAccess(details) {
                    if (done) return;
                    done = true;
                    clearTimeout(timer);
                    try { MemoryAccessMonitor.disable(); } catch (e) {}
                    resolve({
                        timedOut: false,
                        operation: details.operation,
                        from: details.from.toString(),
                        fromRva: inImage(details.from) ? '0x' + details.from.sub(base).toString(16) : null,
                        address: details.address.toString(),
                    });
                }
            });

            send({ type: 'armed', pages: pages.map(p => p.base.toString()) });
        });
    },

    // One-shot combined investigation: scan for the marker, chase each
    // non-module heap hit's pointer chain, dump around the deepest pointer
    // found in each chain, and scanrange the surrounding heap block for any
    // static/heap reference into it. Bundling all of this into a single
    // attach avoids repeated attach/detach cycles against a protected
    // process, which seems to be what triggers termination.
    investigate(marker, maxDepth, dumpBefore, dumpAfter, rangePad) {
        const hits = scanMarker(marker).filter(h => !h.inModuleImage);
        const out = [];
        for (const hit of hits) {
            const nodes = [{ kind: 'buffer', addr: hit.address.toString(), encoding: hit.encoding }];
            let current = hit.address;
            let lastNonModule = hit.address;
            let reachedAnchor = false;
            for (let depth = 0; depth < maxDepth; depth++) {
                const ptrs = findPointersTo(current);
                if (ptrs.length === 0) {
                    nodes.push({ kind: 'dead-end' });
                    break;
                }
                const moduleHit = ptrs.find(p => p.inModuleImage);
                const chosen = moduleHit || ptrs[0];
                nodes.push({
                    kind: 'pointer',
                    addr: chosen.addr.toString(),
                    candidateCount: ptrs.length,
                    allCandidates: ptrs.slice(0, 8).map(p => p.addr.toString() + (p.inModuleImage ? ' [IMAGE]' : '')),
                    inModuleImage: chosen.inModuleImage,
                });
                if (chosen.inModuleImage) {
                    nodes.push({ kind: 'anchor-found', rva: '0x' + chosen.addr.sub(base).toString(16) });
                    reachedAnchor = true;
                    break;
                }
                current = chosen.addr;
                lastNonModule = chosen.addr;
            }

            let dump = null;
            try {
                const start = lastNonModule.sub(dumpBefore);
                const bytes = Array.from(new Uint8Array(start.readByteArray(dumpBefore + dumpAfter)));
                const u32 = [];
                for (let off = 0; off < dumpBefore + dumpAfter; off += 4) {
                    try { u32.push(start.add(off).readU32()); } catch (e) { u32.push(null); }
                }
                const dptrs = [];
                for (let off = 0; off < dumpBefore + dumpAfter - 8; off += 8) {
                    try {
                        const q = start.add(off).readPointer();
                        if (!q.isNull()) dptrs.push([off - dumpBefore, q.toString(), inImage(q)]);
                    } catch (e) {}
                }
                dump = { center: lastNonModule.toString(), u32, ptrs: dptrs, bytes };
            } catch (e) { dump = { err: e.toString() }; }

            let rangeHits = [];
            if (!reachedAnchor) {
                const lo = lastNonModule.sub(rangePad);
                const hi = lastNonModule.add(rangePad);
                for (const r of enumReadable()) {
                    const end = r.base.add(r.size);
                    let cursor = r.base;
                    while (cursor.compare(end.sub(8)) < 0) {
                        try {
                            const v = cursor.readPointer();
                            if (v.compare(lo) >= 0 && v.compare(hi) < 0) {
                                rangeHits.push({ addr: cursor.toString(), value: v.toString(), inModuleImage: inImage(cursor) });
                            }
                        } catch (e) { break; }
                        cursor = cursor.add(8);
                    }
                }
            }

            out.push({
                startEncoding: hit.encoding,
                startAddr: hit.address.toString(),
                reachedAnchor,
                nodes,
                dump,
                rangeHits,
            });
        }
        return out;
    },

    chase(marker, maxDepth) {
        const hits = scanMarker(marker).filter(h => !h.inModuleImage);
        const chains = [];
        for (const hit of hits) {
            const nodes = [{ kind: 'buffer', addr: hit.address.toString(), encoding: hit.encoding }];
            let current = hit.address;
            let reachedAnchor = false;
            for (let depth = 0; depth < maxDepth; depth++) {
                const ptrs = findPointersTo(current);
                if (ptrs.length === 0) {
                    nodes.push({ kind: 'dead-end' });
                    break;
                }
                const moduleHit = ptrs.find(p => p.inModuleImage);
                const chosen = moduleHit || ptrs[0];
                nodes.push({
                    kind: 'pointer',
                    addr: chosen.addr.toString(),
                    candidateCount: ptrs.length,
                    allCandidates: ptrs.slice(0, 8).map(p => p.addr.toString() + (p.inModuleImage ? ' [IMAGE]' : '')),
                    inModuleImage: chosen.inModuleImage,
                });
                if (chosen.inModuleImage) {
                    nodes.push({ kind: 'anchor-found', rva: '0x' + chosen.addr.sub(base).toString(16) });
                    reachedAnchor = true;
                    break;
                }
                current = chosen.addr;
            }
            chains.push({ startEncoding: hit.encoding, startAddr: hit.address.toString(), reachedAnchor, nodes });
        }
        return chains;
    }
};
''')
def on_message(message, data):
    if message.get('type') == 'send':
        payload = message.get('payload', {})
        if payload.get('type') == 'armed':
            pages = ', '.join(payload.get('pages', []))
            print(f'\n>>> Watchpoint ARMED on page(s): {pages}')
            print('>>> Type ONE character into the field NOW. <<<\n')

script.on('message', on_message)
script.load()

if mode == 'scan':
    marker = sys.argv[3]
    res = script.exports_sync.scan(marker)
    print(f'Found {len(res)} match(es) for "{marker}":\n')
    for m in res:
        tag = 'MODULE IMAGE (static/global — likely not per-instance)' if m['inModuleImage'] else 'heap/other'
        print(f"  {m['addr']}  [{m['encoding']}]  region={m['regionBase']} size=0x{m['regionSize']:x} "
              f"prot={m['regionProtection']}  ({tag})")
    print('\nNext: pick a non-module-image address above and run scanptr on it, or just use "chase" instead.')

elif mode == 'scanptr':
    addr = sys.argv[3]
    res = script.exports_sync.scanptr(addr)
    print(f'Found {len(res)} pointer(s) to {addr}:\n')
    for m in res:
        tag = ' (in module image — could be a static field-pointer table)' if m['inModuleImage'] else ''
        print(f"  {m['addr']}  {tag}")

elif mode == 'chase':
    marker = sys.argv[3]
    maxDepth = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    chains = script.exports_sync.chase(marker, maxDepth)
    print(f'Chased {len(chains)} non-module buffer hit(s) for "{marker}" up to depth {maxDepth}:\n')
    for i, c in enumerate(chains):
        status = 'REACHED STATIC ANCHOR' if c['reachedAnchor'] else 'no static anchor found within depth'
        print(f"=== chain {i+1}: buffer {c['startAddr']} [{c['startEncoding']}] — {status} ===")
        for n in c['nodes']:
            if n['kind'] == 'buffer':
                print(f"  buffer      {n['addr']}  [{n['encoding']}]")
            elif n['kind'] == 'pointer':
                extra = f"  ({n['candidateCount']} candidate(s) at this level)" if n['candidateCount'] > 1 else ''
                tag = '  [MODULE IMAGE]' if n['inModuleImage'] else ''
                print(f"  <- ptr at  {n['addr']}{tag}{extra}")
                if n['candidateCount'] > 1:
                    for alt in n['allCandidates']:
                        print(f"       also: {alt}")
            elif n['kind'] == 'anchor-found':
                print(f"  ANCHOR: static pointer at RVA {n['rva']} in the module image — stable across runs")
            elif n['kind'] == 'dead-end':
                print(f"  (dead end — nothing found pointing at the previous address)")
        print()

elif mode == 'scanrange':
    lo, hi = sys.argv[3], sys.argv[4]
    res = script.exports_sync.scanrange(lo, hi)
    print(f'Found {len(res)} pointer(s) into [{lo}, {hi}):\n')
    for m in res:
        tag = '  [MODULE IMAGE — static anchor candidate]' if m['inModuleImage'] else ''
        print(f"  {m['addr']} -> {m['value']}{tag}")

elif mode == 'dump':
    addr = sys.argv[3]
    before = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x40
    after = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x100
    d = script.exports_sync.dump(addr, before, after)
    if d.get('err'):
        print(f"Error: {d['err']}")
    else:
        print(f"Dump around {d['center']} (window starts at {d['start']}, -0x{before:x}/+0x{after:x}):\n")
        print('--- u32 values (offset relative to center) ---')
        for i, v in enumerate(d['u32']):
            off = i * 4 - before
            if v is not None:
                sign = '+' if off >= 0 else ''
                print(f"  {sign}0x{off:x} = {v} (0x{v:x})")
        print('\n--- pointer-looking values (offset relative to center) ---')
        for off, val, inImg in d['ptrs']:
            sign = '+' if off >= 0 else ''
            tag = '  [MODULE IMAGE]' if inImg else ''
            print(f"  {sign}0x{off:x} -> {val}{tag}")
        print('\n--- raw bytes as ASCII (offset relative to center) ---')
        b = d['bytes']
        ascii_str = ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)
        print(f"  {ascii_str}")

elif mode == 'watch':
    addrs = sys.argv[3].split(',')
    timeoutMs = int(sys.argv[4], 0) if len(sys.argv) > 4 else 30000
    print(f'Arming watchpoint, timeout {timeoutMs}ms...')
    result = script.exports_sync.watch(addrs, timeoutMs)
    if result.get('timedOut'):
        print(f'Timed out after {timeoutMs}ms with no write detected to the watched page(s).')
    else:
        print(f"Write detected:")
        print(f"  operation: {result['operation']}")
        print(f"  written address: {result['address']}")
        if result['fromRva']:
            print(f"  from instruction: {result['from']}  (module RVA {result['fromRva']})")
        else:
            print(f"  from instruction: {result['from']}  (NOT in the main module image)")

elif mode == 'investigate':
    marker = sys.argv[3]
    maxDepth = int(sys.argv[4], 0) if len(sys.argv) > 4 else 5
    dumpBefore = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x40
    dumpAfter = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0x200
    rangePad = int(sys.argv[7], 0) if len(sys.argv) > 7 else 0x8000
    chains = script.exports_sync.investigate(marker, maxDepth, dumpBefore, dumpAfter, rangePad)
    print(f'Investigated {len(chains)} non-module buffer hit(s) for "{marker}":\n')
    for i, c in enumerate(chains):
        status = 'REACHED STATIC ANCHOR' if c['reachedAnchor'] else 'no static anchor found within depth'
        print(f"=== chain {i+1}: buffer {c['startAddr']} [{c['startEncoding']}] — {status} ===")
        for n in c['nodes']:
            if n['kind'] == 'buffer':
                print(f"  buffer      {n['addr']}  [{n['encoding']}]")
            elif n['kind'] == 'pointer':
                extra = f"  ({n['candidateCount']} candidate(s) at this level)" if n['candidateCount'] > 1 else ''
                tag = '  [MODULE IMAGE]' if n['inModuleImage'] else ''
                print(f"  <- ptr at  {n['addr']}{tag}{extra}")
                if n['candidateCount'] > 1:
                    for alt in n['allCandidates']:
                        print(f"       also: {alt}")
            elif n['kind'] == 'anchor-found':
                print(f"  ANCHOR: static pointer at RVA {n['rva']} in the module image — stable across runs")
            elif n['kind'] == 'dead-end':
                print(f"  (dead end — nothing found pointing at the previous address)")

        d = c['dump']
        if d and not d.get('err'):
            print(f"\n  --- dump around {d['center']} (-0x{dumpBefore:x}/+0x{dumpAfter:x}) ---")
            print('  u32 (nonzero only):')
            for j, v in enumerate(d['u32']):
                off = j * 4 - dumpBefore
                if v:
                    sign = '+' if off >= 0 else ''
                    print(f"    {sign}0x{off:x} = {v} (0x{v:x})")
            print('  pointers:')
            for off, val, inImg in d['ptrs']:
                sign = '+' if off >= 0 else ''
                tag = '  [MODULE IMAGE]' if inImg else ''
                print(f"    {sign}0x{off:x} -> {val}{tag}")
            ascii_str = ''.join(chr(x) if 32 <= x < 127 else '.' for x in d['bytes'])
            print(f"  ascii: {ascii_str}")
        elif d and d.get('err'):
            print(f"  dump error: {d['err']}")

        if not c['reachedAnchor']:
            print(f"\n  --- scanrange around last non-module address (+/-0x{rangePad:x}) ---")
            if not c['rangeHits']:
                print('    (nothing found referencing this heap block)')
            for h in c['rangeHits']:
                tag = '  [MODULE IMAGE — static anchor candidate]' if h['inModuleImage'] else ''
                print(f"    {h['addr']} -> {h['value']}{tag}")
        print()

else:
    print(f'Unknown mode "{mode}" — use "scan <marker>", "scanptr <addr-hex>", '
          f'"scanrange <lo-hex> <hi-hex>", "dump <addr-hex> [before] [after]", "chase <marker> [maxDepth]", '
          f'"watch <addr1,addr2,...> [timeoutMs]", or '
          f'"investigate <marker> [maxDepth] [dumpBefore] [dumpAfter] [rangePad]"')

session.detach()
print('\nDetached.')
