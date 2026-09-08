# Roles registry: why the vector path never reproduces (record correction + real target)

For the other LLM. Read-only analysis of `C:\Users\Public\co_rolemgr.bin` + `src/entities.h` +
`src/registries.{h,cpp}`, 2026-09-03. Nothing was edited — apply this yourself if you agree.

## Endorse the decision, fix the reason

Calling roles "heap-scan, by design" and moving on is the RIGHT call: items were the ghost
problem (fixed, confirmed), roles heap-scan is safe with no regression, and it's not the lag or
memory driver. No disagreement there.

But the stated reason — "the two-hop path is a coincidence of memory layout / no stable
structural relationship" — is only half right, and if it goes in the record that way the next
person will waste time re-hunting a role vector. The accurate reason:

**The roles are not stored in a `std::vector`. They're in a hash map.** So there is no persistent
role-vector object to find, by any search, ever. The `scene+0x4E0 -> +0x1348/+0x1318`
"role-set with two vectors" that `registries.h` says was "confirmed by three live runs" was a
**coincidental layout match** — memory that briefly held vector-shaped triples of role-like
pointers. That is exactly why it worked once and then failed to relocate three times. A
begin/end/cap + vtable validator can still pass on a coincidence when the underlying object
isn't really a vector; surviving a relog is the real test, and it didn't. (Good catch insisting
on that.)

## Evidence

`*(base+0x69C730) = CRoleMgr` (LIVE-VERIFIED, stable). Its container at `CRoleMgr+0x40` is an
array of 0x20-byte records `{p0, p1, ?, packed-key}`:
- the 4th qword is a **monotonic counter** across records: 0x5E, 0x60, 0x61, 0x62, 0x63, 0x64…
  (per-node key/sequence metadata — a vector has no such per-element field)
- only **28 of 200 slots hold a heap pointer** (sparse — a load-factored bucket table, not a
  packed vector)
- `entities.h` already documented this exact thing: "CRoleMgr+0x40 onward is an array of
  ~0x20-byte records {rolePtr, 0, flags, hash} — a hash-map bucket array," which is why heap-scan
  was chosen originally.

So: the stable ANCHOR was never the problem (base+0x69C730 is solid). The DATA STRUCTURE is a
hash map, and every attempt so far searched for a vector — which cannot match it.

## The real target, if the roles speedup is ever wanted (not now)

Iterate the `CRoleMgr` hash map directly instead of searching for a vector:
1. `CRoleMgr = *(base+0x69C730)`.
2. Walk the record array at `CRoleMgr+0x40`, stride 0x20. Take `p0` (slot 0) as the candidate
   role pointer.
3. Keep entries where `p0` is a heap pointer whose first qword is a CRole-family vtable
   (`VT_MONSTER`/`VT_HERO`/`VT_ROLE_B` per registries.h) — this drops the empty slots and the
   inline-text slots (some records hold UTF-16 font/name bytes, e.g. "Courier"/"Regu…", which the
   vtable check rejects cleanly).
4. Find the bucket-count / end so iteration is exact rather than a fixed 200 guess — the count
   or mask is almost certainly a field in `CRoleMgr+0x00..0x40` (there's a `0x100` at +0x10 and a
   heap ptr at +0x30 worth checking as the bucket base/size).

This is bounded RE (stride and role-ptr slot are already known from the dump), it's anchored on
the proven-stable global so it WILL reproduce across relogs, and it needs no scene dependency and
no owner-search. That is the door that's actually open — unlike the vector hunt, which is closed.

## Suggested one-line record change

Wherever `registries.h`/notes say the roles path is `scene+0x4E0 -> vector`, replace with:
"Roles live in the CRoleMgr hash map (`*(base+0x69C730)+0x40`, 0x20-byte nodes, role ptr in
slot 0), NOT a std::vector. The earlier scene+0x4E0 vector path was a coincidental match and does
not survive a relog. Roles use the heap scan by design; iterate the CRoleMgr hash map if the
speedup is ever needed."
