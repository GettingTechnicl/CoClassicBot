# Deciphering the connection + more from the launcher — prep notes (2026-09-17)

Written during a 1-hour research/planning pass (no code changes made) while the user was away
from the keyboard, in response to: "trying to get additional values from [the launcher], being
able to decipher the connection, utilizing quite a few different tactics." Nothing here has been
implemented or live-tested — it's assembled from (a) re-reading this project's own prior findings
(mostly `coclassicbot-live-offsets` memory, sessions 4b/4c/8), (b) a fresh look at an existing,
already-on-disk capture (`C:\Users\Public\coclassic_netfinder.json`) nobody had re-examined with
this question in mind, and (c) external research per the data-sourcing ladder (game files → CO
emulator sources → black-box → live instrumentation last) — this thread lives at the "emulator
sources" rung, and a genuinely useful one turned up. **Propose-and-confirm still applies — this is
a menu of next steps, not a queue of changes to make unattended.**

---

## 1. The single most useful external resource found: the CO Development Wiki + Comet

**https://conquer-online.github.io/wiki/** — a community-maintained, reverse-engineering-sourced
wiki covering Conquer Online's wire protocol, message-type IDs, and cryptography, across patches
4267 through 5187+. Most individual pages are stubs (`security/rc5.html`, `security/dh.html` are
literally just a heading, no body) — **but the message-type index
(`network/messages/index.html`) is fully populated** and gave an immediate, concrete win:

- **`kMsgActionPacketType = 0x03F2` in this bot's own `packets.h` = 1010 decimal = the wiki's
  documented `MsgAction` ("General action for a player or entity").** Exact match. This is strong
  evidence that despite the Themida packing / 64-bit / protobuf-lite / C3-engine rebuild, this
  client's **message-type ID space and 4-byte `[u16 size][u16 type]` header are unchanged from
  stock TQ Digital protocol** — not a from-scratch redesign.
- The wiki's full message table (copied into this doc's own research, not reproduced here in full)
  includes several IDs directly relevant to future work:
  - **1115 `MsgDisconnect`**, **2521 `MsgKickOut`** — if inbound traffic can ever be decoded, one
    of these landing right before a `GracefulClose` would be a direct, first-party answer to the
    disconnect investigation's still-open "~26 unexplained independent GracefulClose" bucket —
    something the current TCP-level-only instrumentation (`net_recv_hook.cpp`) structurally cannot
    see.
  - **1012 `MsgTick`** ("Round-trip tick validation") and **1037 `MsgPing`** — see §3 below, these
    are the leading suspects for the one genuinely-unexplained encrypted-looking traffic class
    found this session.
  - **1052 `MsgConnect`** / **1055 `MsgConnectEx`** / **1059 `MsgEncryptCode`** — the login-time
    handshake messages; `MsgEncryptCode`'s only field is a `UInt32 Seed` used to seed RC5 for the
    **login password field specifically** (not general game traffic — see below).

**The actual algorithms** aren't in the wiki (empty pages), but ARE fully implemented, tested, and
documented in **Comet** (`gitlab.com/spirited/comet`, patch branches 4274–5187 — the GitHub
`conquer-online/comet` is just a pointer README, the real code is on GitLab). Cloned the `5187`
branch to this session's scratchpad (not part of the repo) and read the crypto source directly:

- **`RC5.cs`** — RC5, 12 rounds, 16-byte word/key size, used ONLY to encrypt the password field
  during login (`MsgAccount`), seeded either by a static default key or a numeric seed carried in
  `MsgEncryptCode`. Not a general packet cipher.
- **`DiffieHellman.cs`** — non-standard DH (raw modpow, no hash) used to negotiate a shared secret
  for **Blowfish**, not RC5, despite RC5 being the more commonly-cited name. Default prime
  root/generator are hardcoded constants (interoperability defaults); a real server can use its
  own.
- **`BlowfishCipher.cs`** — the actual **session cipher for ongoing game-server traffic**: 576-bit
  key Blowfish in **CFB64** (byte-at-a-time keystream, not block-padded — this matters, see §3),
  standard P-box/S-box init constants (the Pi-digit table), keyed from the DH shared secret (or a
  hardcoded default `"DR654dt34trg4UI6"` before/absent a real exchange).
- **`Comet.Game/Packets/MsgHandshake.cs`** — the actual wire handshake: the very first bytes the
  **game server** sends on connect (before login, unencrypted almost by definition — it's what
  bootstraps encryption) carry the DH prime root, generator, and server's public key, plus the
  initial Blowfish IVs; the client answers with its own DH public key in the same message shape.
  No certificate/signature on any of this — **by design, a passive relay cannot derive the shared
  secret from watching both public keys cross the wire (that's the whole point of DH), but an
  *active* relay that terminates two separate DH handshakes (one to the real client, one to the
  real server) can** — same class of technique as any classic unauthenticated-DH MITM.

**Caveat, stated plainly:** Comet targets *stock* TQ Digital clients. "Classic Conquer" (this
project's actual target, `conqueronline.net`) is independently built — forum discussion
(`cooldown.dev/topic/245`) describes it as running TQ's newer **C3 engine + ImGui**, genuinely
64-bit (no known official 64-bit TQ client ever existed), reverse-engineered from official
binaries rather than derived from Comet-era source. The message-type match above is real evidence
of continuity, but the crypto layer needs its own live confirmation — it is not automatically the
same.

---

## 2. This session's own free finding: `C:\Users\Public\coclassic_netfinder.json` already answers part of this

This file is a leftover capture from a **previous session's** `netfinder.cpp` run (the read-only
Winsock-hook tool already in this repo, `CMakeLists.txt` target `netfinder`) — nobody had gone
back and looked at it through this lens. It captures every `send()`/`WSASend()` call in the
process, with the first 16 raw bytes and (when the return address lands inside the game module) a
stack backtrace.

**Two clearly distinct traffic classes are mixed together in this one capture:**

1. **`"frames":[]` entries, `hdr` starting `22,3,3,...` or `23,3,3,...`** (i.e. `0x16 0x03 0x03` /
   `0x17 0x03 0x03`) — **this is a genuine TLS 1.2 record header** (ContentType
   Handshake/ApplicationData, version 0303, then a 2-byte length that matches the packet size
   exactly, e.g. `len:199` ↔ header length field `0x00C2`=194 + 5-byte record header). The empty
   `frames` (no game-module return address on the stack) confirms this call did **not** originate
   from the game's own send pipeline — it's a **separate TLS connection** somewhere else in the
   process. `netfinder.cpp`'s own header comment already flagged exactly this class of false
   positive once before (an HTTP GET to `/cache/vcache.txt`, an update/version-check, not game
   traffic) — this is the same phenomenon, almost certainly a similarly mundane thing (telemetry,
   an update/CDN check, the fork's Discord-RPC integration, etc.), **not the game-server
   connection**, and should be filtered out of any future analysis by exactly this signature
   (`frames` empty + `hdr[0] in {0x14,0x15,0x16,0x17}` + `hdr[1..2] == 0x03,0x0X`).
2. **`"frames":[...]` entries with real game-module addresses** (the confirmed real send chain,
   `0x1DD888←0x1909D9←0xBFB48←0xE9B5C←0x384729` — matches `coclassicbot-live-offsets` memory's
   own "331/332 real sends" finding), **small (8–38 byte) packets, header bytes that do NOT look
   like a readable `[u16 size][u16 type]`** — e.g. `len:13`, `hdr:[137,72,186,...]`; if this were
   the bot's own plaintext framing, byte0/1 would read as a small integer ≈13, not 137/72. This
   traffic genuinely looks like real ciphertext (or some other transform), and it's real game
   traffic, not noise.

**This directly conflicts with `coclassicbot-live-offsets` session 8's live-tested finding** that
a hand-built, genuinely plaintext raw packet — sent through the same confirmed real pipeline
(`packets.cpp`'s `SendPacket()` → `0x1DD450` → `0x1DD860` → real `send()`) — was accepted by the
official server with zero encryption applied anywhere in our own code, and that `0x1DD450` itself
was disassembled and shown to copy bytes into its queue "essentially verbatim... no visible
encryption step" (memory line 444).

## 3. Working hypothesis to reconcile the two: a message-type-specific transform, not a whole-connection cipher

Both findings are independently solid (one is a live server-accepted test, the other is a genuine
wire capture through the confirmed pipeline), so the honest reconciliation is: **`0x1DD450`
itself never encrypts anything — whatever transform produces the high-entropy bytes is applied by
the *native caller*, before the bytes ever reach `SendPacket()`'s equivalent, and only for
*some* message types.** Our own hand-built calls (a raw `MsgMapItem` pickup) skip that step
entirely and still worked — meaning either that specific message type doesn't require it, or the
transform isn't actually a confidentiality cipher at all in the security sense.

**Leading suspect: `MsgTick` (1012, "round-trip tick validation") or `MsgPing` (1037).** These
12–13 byte captures are small, frequent (30+ in a short window), and fixed-size-class — exactly
the shape of a heartbeat, and "round-trip tick validation" is a plausible thing for a game to
deliberately obfuscate/checksum specifically so a bot can't trivially forge "I'm still here, and
running unmodified" responses — independent of whether the rest of the protocol is encrypted. If
true, this is a **much smaller, more tractable RE target than "decrypt the whole connection"**,
and — interestingly — ties back into the disconnect investigation: if the server's kick decision
for a bad/stale client leans on this validation, understanding its transform could bear directly
on the "~26 unexplained independent GracefulClose" bucket in `TROJAN_DISCONNECT_REPORT.md`.

This is a hypothesis, not a finding — the only way to actually confirm it is to correlate a fresh
capture's message SIZES against which native action was happening at that moment (the existing
`[actionrate]` telemetry in `packets.cpp` already timestamps real activity; a synchronized fresh
`netfinder` run during a period of near-total idle — character just standing, nothing but
heartbeats — would isolate the 12-13 byte class cleanly and let its cadence be compared against
`MsgTick`'s likely interval).

---

## 4. Proposed next steps (ordered cheapest/lowest-risk first — none of this has been done)

1. **Free, zero-risk, do first:** grep any *fresh* `netfinder` or `net_recv_hook` capture for the
   TLS signature (`hdr[0] ∈ {0x14..0x17}, hdr[1..2] == 0x03,0x0X`) and exclude those entries before
   looking at anything else — saves re-discovering the same false lead.
2. **Free, zero-risk:** re-run `netfinder` (already built, `build/bin/Release/netfinder.dll`,
   injectable the same way `coclassic.dll` is) during a deliberately idle stretch (character
   standing still, nothing else happening) to isolate the 12–13 byte class and get its exact
   inter-arrival cadence — cross-check against whatever interval a tick/ping validation would
   plausibly use.
3. **Cheap, in-process, same risk class as work already done:** check whether the *inbound* side
   (`net_recv_hook.cpp`'s existing `RecordInboundEvent` peek bytes, or the action_recorder's
   inbound ring) ever shows a readable `[u16 size][u16 type]` header on data frames. If inbound
   is plaintext-framed the same way outbound turned out to be (net of whatever transform gate #2
   narrows down), "decipher the connection" mostly collapses into "write a decoder using the CO
   wiki's message-type table" — pure C++ + a lookup table, no crypto and no new live-instrumentation
   risk, directly extending `DecodeVarintFields()`'s existing pattern.
4. **If a real transform is confirmed on a specific message type:** the Comet Blowfish/DH C# is
   now sitting in this session's scratchpad (`comet5187/src/Comet.Network/Security/`) as a
   complete, tested reference — mechanical to port to C++ if it's ever actually needed. Before
   reaching for it: check whether the connection object's `+0x20` transport-singleton field
   (already located, `coclassicbot-live-offsets` session ~7, described there as "high-entropy,
   crypto-key-shaped, ~0x48 bytes") is actually a Blowfish key schedule sitting in memory
   already-expanded (P[18]+S[1024] = 4168 bytes is the real shape to search for, larger than the
   0x48 bytes previously glanced at — worth a fresh, larger dump around that same object) — reading
   an already-derived key straight out of memory sidesteps the DH discrete-log problem entirely
   and needs no network-level MITM.
5. **Only if 1–4 dead-end:** the network-level active-MITM approach (extend the launcher's
   existing `Socks5Relay` from a passive byte pipe into a protocol-aware terminator that runs its
   own DH exchange with each side) is the standard technique for an unauthenticated-DH protocol,
   but it's the most invasive option here (modifies live traffic shape, highest detection-relevance
   of everything in this list) — last resort, not a starting point, consistent with
   `THEMIDA_NOTES.md`'s own "minimize live interaction, maximize offline/black-box" rule.

---

## 5. Separate, smaller thread: "additional values from the launcher"

The launcher (`injector/main.cpp`) already has more relevant plumbing than it's using:

- **`Socks5Relay` + `RelayLogger`** (proxy mode) already hex-dumps every byte crossing the wire in
  both directions to `relay_packets.log` — this is the exact capture mechanism §4/§5 above would
  want, already built, just not normally running (only active in proxy mode). No new code needed
  to get a fresh full-session capture — just launch one account through the existing proxy path
  with logging on.
- **Easy, low-risk enhancement (proposal only):** teach `RelayLogger` to label packets by the CO
  wiki's message-type table (`hdr[2..3]` as u16 → name) instead of raw hex, and to flag the TLS
  false-positive signature from §2 automatically — turns every future proxy-mode session into a
  self-annotating capture instead of needing manual hex-reading each time.
- Credentials/session data (`credentials.h`) already covers what a launcher reasonably should hold
  locally (DPAPI-encrypted). Nothing obviously missing there for "additional values" — the
  interesting untapped values are connection-layer (RTT, real resolved IP, TLS-vs-not per
  connection), not credential-layer.

---

## Session 2026-09-18 — live diagnostics, why decryption matters, and what's ruled out

**Goal restated (why this thread is worth the effort, not a parallel rabbit hole):** the action
layer (movement, pickup, ranged/melee/magic) is already live-verified via native calls/memory —
decrypting the connection is NOT needed to make the bot work today. It's pursued for two specific
payoffs: (1) **running the bot without a live game client injected at all** — talk to the server
directly over a socket, headless — and (2) **packet-level robustness against disconnects** (see
`TROJAN_DISCONNECT_REPORT.md`'s still-open "~26 unexplained independent GracefulClose" bucket —
reading `MsgDisconnect`/`MsgKickOut` server-side reason codes directly would be a first-party
answer instead of inferring from TCP-level signals alone).

### What got shipped
- `src/msg_types.h` — the CO wiki's full message-type table, wired into the overlay's Packets tab
  and `RelayLogger`'s hex dumps (label packets by name, e.g. `type=0x3F2 (MsgAction)`). Built into
  `coclassic.dll`/`launcher.exe`.
- `src/netfinder.cpp` — preview cap raised 16→128 bytes (was truncating every real packet).

### The message-staging-buffer finding (corrects a prior session's read)
Live memory read of the confirmed connection-object chain (`game.h`'s `ResolveConnectionObject()`:
`CNetClient_ConnectionSingleton()` RVA `0xB7320` → `+0x20`) shows the **first N bytes match,
byte-for-byte, the most recently sent packet's wire bytes** — confirmed against netfinder's
independent Winsock-level capture from the same session, exact match on multiple samples. This is
a **reused message-staging buffer holding final wire bytes**, overwritten from the start on each
send — NOT a persistent key. It retroactively explains an earlier session's "~0x48 bytes of
high-entropy, crypto-key-shaped data" note (`coclassicbot-live-offsets`, session 8) as the same
kind of artifact (leftover bytes from a longer prior message), not real key material. That earlier
note should be read as **debunked by direct evidence**, not just superseded.

### Decrypt attempts — rigorously tested, all negative (with a caught-and-fixed harness bug)
Built a Python harness (`scratchpad/try_decrypt3.py`) porting cipher logic **directly from Comet's
C# source** (not a generic library, to avoid a wrong-mode false negative): both `TQCipher` (counter
XOR + nibble-rotate) and `BlowfishCipher` (byte-wise CFB, IV-chained — confirmed the right mode:
Comet's own `Decrypt()`/`Encrypt()` process one byte at a time with no block padding, matching our
37-byte, non-8-aligned captures exactly). **Both got positive controls** (encrypt a known test
packet — header + payload + `TQClient`/`TQServer` seal — through the harness, confirm decrypt
round-trips before trusting any negative). The TQCipher control **failed on the first attempt**:
applying `XOR(v) = rot4(v^0xAB)^K` twice is NOT self-inverse in general (proven algebraically —
requires `K`'s two nibbles to match, not generally true), even though Comet's C# calls the same
`XOR()` method for both `Encrypt()`/`Decrypt()`. Fixed by deriving the real inverse
(`rot4(c^K)^0xAB`, reversed operation order) — control then passed cleanly. **Lesson banked: a
harness with zero hits ever is unfalsifiable; always positive-control before trusting a negative.**

With both harnesses validated, tested against 150+ fresh real captured samples (client→server,
same session as a memory snapshot):
- TQCipher, default/never-rekeyed state, full 0–65535 counter sweep: **zero hits** (score
  threshold required BOTH size-field match AND known type ID — a joint condition with near-zero
  random chance rate, unlike an earlier looser threshold that produced 6361 "hits" which was just
  statistical noise, ~matching the predicted false-positive rate exactly).
- Blowfish, connection-object static-tail-as-key (3 IV guesses) + Comet's hardcoded
  `DefaultSeed="DR654dt34trg4UI6"`: **zero hits**, including with an added seal-string check
  (`TQClient`/`TQServer`, 8 bytes, fixed position, wiki-documented — near-zero false-positive rate
  on its own).

### The structural finding that reframes the whole hunt
Grepped Comet's actual server source (not just the crypto class files) for how `TQCipher` and
`BlowfishCipher` get wired to real connections: **`TQCipher` is only ever instantiated by the
ACCOUNT server** (`Comet.Account/States/Client.cs`) — i.e. the login socket, which nothing we've
captured tonight is even connected to. **The GAME server uses the DH+Blowfish handshake
exclusively** (`Comet.Game/Server.cs`'s `Exchanged()`): the Blowfish key is literally
`DiffieHellman.PrivateKey` (the raw DH shared secret bytes), with IVs from the handshake's own
random buffers. This means tonight's TQCipher negatives aren't just "wrong key" — they're
**wrong cipher for this socket entirely**, which is a cleaner, structural explanation, not just an
empirical shrug. The tail-as-Blowfish-key negative stands on its own separately: it's still
evidence the ~53-byte static tail isn't the raw DH shared secret (mode/harness now confirmed
correct), just not yet explained by a wrong-cipher story.

### Key-schedule anchor hunt — DONE, negative (2026-09-18 continued)
Ran the "fresh, larger dump around the connection object" sub-step this section originally called
for. Built a validated oracle (`scratchpad/scan_for_schedule.py`: every 4-byte-aligned offset in a
dumped region, loaded as a candidate P[18]+S[1024] schedule, several nearby IV guesses, tested
against real captured client→server samples, accept only on a `TQClient`/`TQServer` seal hit — same
positive-controlled harness as the decrypt attempts above) and pointed it at:
- The connection object itself, 16KB anchor dump: **zero pointer-looking QWORDs anywhere in the
  full 16KB** (not just the first 0x400 bytes originally scanned) — this object really is just the
  small message-staging buffer plus unused/zeroed heap padding, confirmed, not "one hop from a
  bigger struct."
- `netClient` (`coclassicbot-live-offsets` memory's `0x15E718`), tried both plausible
  interpretations since the address's own frame of reference was ambiguous: **literal absolute
  `0x15E718`** reads as a small table of `base+RVA`-shaped pointers (vtable/function-pointer table,
  not data — every word lands in the `0x140xxxxxx` image range, the signature of code/vtable
  pointers, not high-entropy heap data); **`base+0x15E718`** disassembles as literal x86-64
  instructions (`mov [rcx],rax`, `call rel32`) — straightforwardly code, not a data object.
- **Result: 0 hits across ~250,000 (offset, IV, sample) combinations, 48KB scanned.** Both
  known/documented anchors are now genuinely exhausted, not just guessed at.

**Antipattern flag for next time:** this is the same shape of dead end `coclassicbot-registries`
already recorded once for the roles-container hunt (two anchor theories, both implemented and
live-refuted, resolved by falling back to the heap-scan instead of a third anchor guess). Don't
reach for a fourth named-object guess here either — the next legitimate move if the schedule search
resumes is a **bounded, capped `VirtualQueryEx` scan over `MEM_COMMIT`+`PAGE_READWRITE` private
regions only** (skip image/code ranges entirely — filterable for free, see above), every candidate
gated by the same seal-oracle, with a hard node/wall-clock cap and exception-safety around the scan
body (`coclassicbot-registries` also records an unbounded injected search once exhausting its arena
and crashing the game with an uncaught `bad_alloc` — read-only intent doesn't make an unbounded loop
safe). **Not started — deliberately deprioritized, see below.**

### Bigger open question: is a session-wide cipher even the right target?
Re-reading this doc's own §2–§3 alongside `coclassicbot-live-offsets` session 8: a hand-built,
genuinely **plaintext** packet was sent through the fully-confirmed real pipeline (`0x1DD450` →
`0x1DD860` → real `send()`) and **accepted by the official server with zero encryption applied**,
and `0x1DD450` itself was disassembled as copying bytes "essentially verbatim, no visible
encryption step." The only ciphertext-*looking* traffic found so far is a small, frequent
12–13-byte class, hypothesized (§3) as a transform specific to `MsgTick`(1012)/`MsgPing`(1037)
heartbeats — not a whole-connection cipher. If that hypothesis is right, a 4168-byte session-wide
Blowfish schedule may not even be the mechanism protecting most traffic, and the key-schedule hunt
could be solving for the wrong thing. **Two cheaper, still-unexecuted steps from this doc's own §4
should come before resuming the schedule hunt:**
1. **Sample hygiene check** — confirm every sample fed to the decrypt oracle tonight is a genuine
   game-module send (non-empty `frames`, filters out the TLS-record false-positive class from §2)
   and isn't itself part of the small heartbeat-ish class contaminating a "general traffic" test.
2. **Check inbound for plaintext framing** (§4 step 3) — **DONE, decisive negative.** Built
   `src/recvfinder.cpp` (new CMake target, same Detour-on-Winsock-exports technique as
   `netfinder.cpp`, mirrored for `recv`/`WSARecv` — ships as its own DLL specifically so it can be
   injected into an already-running process without a relaunch). Captured 42 real inbound reads
   across sizes 1, 12, 59, 65, 69, 79, 81, 112, 121, 242 bytes, all through the confirmed
   game-module call chain. **Zero readable headers** — every sample's would-be size field is off by
   thousands from the actual byte count, and the would-be type field never once lands in the known
   `src/msg_types.h` ID set, across every size class checked. Inbound is genuinely ciphertext-shaped,
   not a narrow exception — this rules out the "decrypt collapses into a free decoder" hope.

### Where this leaves the investigation
Both cheap paths (key-schedule anchor hunt, inbound-plaintext check) are now genuinely exhausted
with rigor, not just tried once. The broad inbound encryption (many sizes/types, not just a
heartbeat) also revises the working theory.

**Correction to the "lenient server validation" explanation above — it doesn't actually hold up.**
If the connection is really encrypted, the server decrypts before dispatching to any handler; a
plaintext packet decrypts to garbage, fails at the size/type layer, and gets dropped upstream of any
handler-level leniency. That explanation can't rescue an encrypted-connection model. The coherent
version is: **the native encrypt step sits downstream of our injection point** — we inject at
`SendPacket()`/`0x1DD450` (confirmed to copy bytes verbatim, no visible encryption), so if a cipher
lives between there and the real `send()` call, our hand-built "plaintext" packet gets encrypted on
the way out exactly like everything else, and "no encryption in `0x1DD450`" only means the transform
is a few calls further down — it says nothing about whether the connection overall is encrypted.

### Known-plaintext wire-capture experiment — settles Model A vs Model B empirically
Used the existing "Debug: Native Pickup Test" overlay button (`CHero.cpp`'s `DebugTestNativePickup`)
as a controlled, fully-deterministic packet source: it takes a typed-in Item ID + X/Y, hardcodes
`idType=1000000` and `plus=0`, so the resulting `MsgMapItem` plaintext is fully reconstructable from
just those three inputs — no ambiguity, no guessing. Added one log line to
`SendPickupItemPacket()` dumping the exact bytes handed to `SendPacket()` (pure logging addition,
doesn't touch send behavior). Confirmed the decode is exactly right by hand — every field (item id,
idType=1000000, x, y, plus=0, mode=3) matched the known inputs byte-for-byte.

Ran the experiment **twice**, the second time with netfinder's capture file actively confirmed
growing (proving the hook was live) *before* the test fired, closing off any injection-ordering
ambiguity:
- Both times: `SendPacket()`'s own trace log confirms the call happened (`msgType=0x44D size=23`,
  `ok=true`).
- Both times: **the known 23-byte plaintext never appears on the wire** — not as an isolated
  `len=23` capture, not as a substring inside any larger capture.
- Across the entire session (312 captured sends, 100% via plain `send()` — never `WSASend`, so no
  multi-buffer capture-gap explanation either), **length 23 does not appear even once.**

**Conclusion: Model B (the connection is genuinely encrypted) is empirically supported, not Model
A.** Our own hand-built packets are not actually reaching the wire in the clear; they're being
transformed downstream of `SendPacket()`, the same as native traffic. Per the reasoning above, a
known plaintext↔ciphertext pair like this does NOT let us recover the Blowfish key (infeasible by
design) — but it does two other useful things: it brackets the encrypt function's location to the
span between `0x1DD450` and the real `send()` call, and it gives a rock-solid **known-answer
oracle** (this exact plaintext, reproducible on demand via the debug button) for validating any
future candidate key/schedule instantly — strictly stronger than the seal-string heuristic used
tonight, which carries false-negative risk if this client's seal handling differs from Comet's.

**Open wrinkle, not yet explained:** a simple length-preserving stream cipher (Blowfish-CFB) would
still produce a 23-byte *ciphertext* — we'd expect to see `len=23` in the capture, just with
different bytes. We never do, across 312 real sends. That means something more than a straight
byte-for-byte cipher is happening before the wire write — likely merging with adjacent queued
messages, padding, or reframing, not just encryption. Worth understanding before the schedule hunt
resumes, since it affects how a decrypt attempt would even need to be framed.

### Follow-up: connection-object struct layout — verified fields vs. a misread one (retire flushfinder)
Built three iterations of a new tool (`src/flushfinder.cpp`, `flushfinder`/`flushfinder2`/`flushfinder3`
CMake targets) to Detour-hook `0x1DD860` (the confirmed-NOT-hook-resistant per-tick poller — safely
distinct from the protected `0xE9960`/`0xBF920` coroutine, which our own packets never even traverse
since `SendPacket()` enqueues via `0x1DD450` directly) and read the pending-send buffer before the
internal flush. **Retired — wrong tool for this question.** `send()`'s own argument, already captured
by `netfinder`, IS the final post-merge/post-transform buffer; there was never a need to read an
intermediate buffer at a guessed offset to know the output. Two real things came out of the attempt
before it was correctly abandoned:
- **`connPtr+0x2008` is NOT a valid buffer pointer.** Read as `0xCBC`/`0xCC0` consistently across two
  completely different sessions/PIDs — far too small for a real 64-bit heap address. Almost certainly
  a capacity or config constant, not a live pointer. The prior `coclassicbot-live-offsets` session-8
  note describing `+0x2008` as "pending send buffer POINTER" should be treated as **unconfirmed**,
  not re-asserted, until someone actually dereferences it successfully.
- **`connPtr+0x2000` ("count")** read as plausible small integers (0 at idle, 33/18/34 during real
  traffic) — consistent with being a real length field — but two live test runs (one busy map, one
  empty map) both failed to catch a nonzero-count moment that matched our own deterministic test
  packets' sizes, so treat this field too as **plausible, not confirmed** — it was never cross-verified
  against a known packet the way the message-staging-buffer finding (`connPtr+0x00`) was.
- The one thing this session's exploration DID reconfirm cleanly: **message content lives inline at
  `connPtr+0x00`**, byte-for-byte matching netfinder's wire capture (original finding, this section's
  intro) — that remains the one verified fact about this object's layout. Everything past the first
  ~37-96 bytes (the exact boundary appears to vary by how much traffic has flowed) is unconfirmed.

### Empty-map confirmatory test — decisive, precisely scoped
Repeated the known-plaintext experiment a third time on a freshly-relaunched client on an empty map
(minimal background traffic — the cleanest environment available). `netfinder` captured a **48-byte**
wire entry with the real-pipeline backtrace confirmed (`0x1DD888←0x1909D9←0xBFB48←0xE9B5C←0x384729`).
22 (this run's pickup plaintext) + 26 (this run's jump packet, sent 310ms earlier) = 48 — size-matched,
though not provable as THE specific merged pair since netfinder captures carry no per-entry timestamp.
Checked the one fully-deterministic, byte-exact known plaintext available (this run's 22-byte pickup,
decoded and hand-verified field-by-field against the debug inputs) against the full 48-byte capture:
**not present, as a prefix, suffix, or substring, anywhere.**

**Precisely scoped conclusion (not a blanket claim):** in this specific empty-map, backtrace-confirmed,
48-byte capture, the known 22-byte pickup plaintext does not appear. Combined with the two earlier
independent test runs (23-byte plaintext, verified-live-hook, zero matches across 312 total captured
sends that session) — **three separate deterministic-plaintext tests, across two different client
sessions, all show the same result: our own hand-built packet content never appears on the wire
unmodified.** This is the strongest evidence gathered tonight that a real transform is happening
(Model B), not merely "connection is encrypted" as a general claim — the mechanism, whether it merges
before or after transforming, and whether it's applied to all traffic or a subset, remains unidentified.

### Black-box characterization — DONE, decisive: stateful stream cipher, not a toy XOR mask
Added a `skipJump` mode to `DebugTestNativePickup()` (`CHero.h`/`.cpp`, default `false`, preserves
prior behavior) so the debug button fires an isolated pickup send with no jump merged in — needed for
a byte-aligned known-plaintext↔ciphertext pair. Also added a **"→ Pickup Test" button** on every
ground-item row in the overlay's Map tab entities table (`overlay.cpp`), auto-filling Item ID/X/Y so
this no longer requires hand-transcribing from a screenshot.

**Experiment 1 (identical plaintext, sent 4 times):** dropped one item, clicked the isolated-pickup
debug button 4 times without changing anything (`16004D040DBA256E7D10C0843D189D01200E28003003`,
byte-identical all 4 times, confirmed from the log). `netfinder` captured 4 corresponding `len=22`
entries in matching sequence. **Result: all 4 ciphertexts are pairwise distinct, differing at every
single byte position, every time.** This is the clean, controlled version of the earlier "plaintext
never appears" finding — with identical input confirmed, not just similar input.

**Conclusion (precisely scoped): this rules out a stateless transform** (fixed XOR mask or
per-message-reset IV/key) **outright.** The cipher state is continuously advancing across the
connection — consistent with Comet's own `BlowfishCipher` architecture (CFB, `EncryptCount`/state
persisting as instance fields across the object's whole lifetime, never reset per call). The
plaintext⊕ciphertext values computed for all 4 sends show no visible structure (high entropy, no
repeats) — consistent with a real cipher, not something crackable by inspection alone.

**This settles the "characterize before committing to RE" question this section was opened to answer:**
it is NOT a toy mask breakable straight from the oracle. Locating the actual native encrypt function
(to read the live key/IV state) is now the *justified* next step, not a premature one — the
black-box path has been properly exhausted first, per the process this session established.

(Earlier same-day sample — 3 pickups with only the item-ID field differing — showed the same full
avalanche across all 22 bytes; consistent with this conclusion, but on its own couldn't distinguish
"real avalanche" from "different position in a continuously-advancing stream," which is exactly why
the identical-plaintext-4x experiment above was needed to settle it cleanly.)

### Remaining next steps
1. **Locate the actual native encrypt/decrypt function.** Justified and scoped: input truth =
   `0x1DD450`'s own argument (confirmed hookable, not protected) = plaintext before any
   merge/transform; output truth = `send()`'s argument, already captured by `netfinder`. The
   transform lives somewhere in between — RE effort should target that span specifically, not guess
   at connection-object offsets. Still the biggest, riskiest option per `THEMIDA_NOTES.md`'s posture,
   but no longer speculative about whether it's needed.
2. The bounded `VirtualQueryEx` RW-private-only scan this doc already specified (capped,
   exception-safe, gated by the known-answer oracle — the exact pickup plaintext, reproducible on
   demand via the debug button) — no anchor shortcut, sized accordingly.
3. Capture the connection's opening bytes (inject before/at login) to see whether this client sends
   anything resembling `MsgHandshake`'s cleartext DH parameters — needs the fiddly injection-timing
   handling this doc originally flagged.

**Process lesson banked:** when a hook observes nothing where something must be happening, the next
move is to re-check the two endpoints already in hand (here: `netfinder`'s existing capture had the
answer two iterations before it was actually read that way) before building another variant of the
same intermediate-probing tool.

---

## Strategic pivot: headless is the actual goal, not "read the live client's key"

Explicit human decision (2026-09-18, late session): the connection-decipher thread is pursued as a
**long game in service of running the bot headless** (no game client at all) — not a quick unlock.
For that goal specifically, extracting the live Themida-packed client's key is unnecessary: a
headless client would run its **own** DH handshake and derive its **own** Blowfish state, exactly
like the real client does. **The actual headless-critical question is narrower: does this server's
handshake match Comet's DH+Blowfish shape closely enough to reproduce?** That question is answerable
without ever touching the protected encrypt function — via a **relay capture from the very first byte
of a fresh connection**, which every prior attempt this session (netfinder injected mid-session) was
structurally unable to get.

### The relay: two real bugs found and fixed, not just "brought back up"
The fork's `Socks5Relay`/`RelayLogger` (`injector/main.cpp`) already existed for this — it was NOT a
new build — but had two live bugs that had to be fixed before it could actually capture anything
useful:
1. **Only one direction was ever logged.** `HandleClient()`'s backward-direction `PumpTraffic` call
   passed `nullptr` for the logger (`"target->client"` — the direction carrying any server-sent
   handshake response) — only outbound was ever hex-dumped. Fixed: both directions now log.
2. **`RelayLogger` was never actually instantiated anywhere.** `relay.Start(...)` was always called
   with a null logger in the real UI-driven proxy-mode path (`params.activeLogger` was declared but
   never assigned) — `relay_packets.log` never got written despite the class being fully implemented
   and documented. Fixed: a `RelayLogger` instance is now created, started next to `launcher.exe`, and
   wired into both `relay.Start()` and `params.activeLogger` (so it stops cleanly on teardown).

**The relay always tunnels through an upstream SOCKS5 proxy — there's no direct-connect mode in
`Socks5Relay`.** The human had previously stood up a trivial local pass-through SOCKS5 server on this
PC purely to satisfy that requirement (no real anonymization, just plain forwarding) — not documented
anywhere before now. Recreated at `scratchpad/local_socks5_passthrough.py` (no-auth, CONNECT-only,
plain relay, listens on `127.0.0.1:1080`). **To reproduce:** run that script, then in `launcher.exe`
set the account's proxy address to `127.0.0.1:1080` and enable "use proxy", then launch normally —
`ServerConfigPatch` rewrites every `servers.json` entry to point at the relay's own local listen
port, which tunnels through `127.0.0.1:1080` to the real target.

### First capture result: account/login server only
The capture landed the connection to `login.conqueronline.net:9959` — the **account/login server**,
not the game server (see below for why). Its opening exchange (207 bytes out, 33 in, 19 out, then
closed) is **high-entropy from byte 0** — no readable `[u16 size][u16 type]` header. **Corrected
interpretation** (an earlier pass through this doc wrongly called this "diverges from Comet, maybe
SRP6" — that was a category error): the account server's documented cipher (`Comet.Account/States/
Client.cs`, confirmed earlier tonight) is **TQCipher, a stream cipher** — high-entropy from byte 0 is
exactly what a stream cipher looks like, and is *consistent* with the Comet account model, not a
divergence from it. There was no need to reach for SRP6.

**Tested it properly — the actual right-connection TQCipher experiment, not the earlier wrong-socket
one.** The earlier "TQCipher: zero hits" result in this doc tested TQCipher against *game-server*
traffic — the wrong socket entirely (TQCipher is account-only per Comet's source). This is the first
time TQCipher has been tested against its documented home. Ran the same validated harness (positive-
controlled earlier tonight) with a full 0–65535 counter sweep, default/un-rekeyed `KInit` state,
against the real captured 207-byte client→account-server message: **zero hits** (joint size+type+seal
validation, same rigor as every other test tonight). **Precisely scoped conclusion:** TQCipher's
default state is ruled out for the account server specifically, same as it was for the game server —
this does NOT mean TQCipher is the wrong algorithm, only that the default/un-rekeyed seed path isn't
what's active on this connection. Remaining open possibilities: a different seed constant than
Comet's hardcoded one, rekeying already in effect before this first message via some means not yet
identified, or a genuinely different cipher despite Comet's account server using TQCipher. **This
does not yet answer the actual headless-relevant question**, which is about the GAME server's
handshake, not the account server's.

### Why the game server's handshake wasn't captured, and the cheap path to try first
Per the CO wiki's own documented flow: client authenticates against the account server, receives the
real game server's address via `MsgConnectEx`, then disconnects and reconnects there **separately**.
That address is **dynamic** — delivered inside the account server's own response, never present in
`servers.json` — so `ServerConfigPatch`'s static rewrite can't touch it. The client connects to the
real game server directly, correctly bypassing the relay (which faithfully passed the `MsgConnectEx`
response through unmodified).

**Don't jump straight to protocol-aware MITM rewriting — try the cheap version first.** If the game
server's address is *stable* across logins (plausible — one realm, one game-server box), get it once
(either by decrypting the captured `MsgConnectEx` response once TQCipher/rekeying is sorted out, or
trivially via `netstat`/a socket-info log during any normal live session) and add it to `servers.json`
as a **static entry**, the same way the account server is listed — `ServerConfigPatch` already
rewrites every entry in the file, so this needs zero new code, just a config addition. That routes
the game leg through the relay with no MITM logic at all. **Only build the in-flight `MsgConnectEx`
rewrite if the address turns out to actually change between logins** — confirm instability first,
don't build the hard version speculatively. Neither path started yet — scoped for a future session.

### Kill-switch bug found and disarmed (separate from the capture, but a real usability issue)
`Socks5Relay::HandleClient()` fires `TriggerFailClosed()` unconditionally whenever any tunnel that
was established later closes (`if (m_establishedTunnel.load()) TriggerFailClosed(...)`) — with no way
to distinguish a genuine mid-game proxy failure from the account server's own **normal**
disconnect-after-handoff. Since (per above) the relay currently only ever proxies the account/login
leg — which always closes quickly by design — **this kill-switch was misfiring on every successful
login**, not just real failures (live-confirmed: the human's account manager showed "failed" and
closed the window despite the game having logged in correctly). **Disarmed for now**
(`params.options.m_killSwitch = false`, `injector/main.cpp`) —

**⚠️ TODO, do not let this silently become permanent:** re-arm the kill-switch specifically for the
game leg once it routes through the relay (see the `servers.json`-static-entry approach below). The
fail-closed behavior is the whole point of proxy mode — losing it is a real regression, not a
harmless cleanup. It stays disarmed only until the relay can tell a real
failure apart from a normal handoff.

### Game-server handshake capture — DONE, via passive sniffing, not the relay
`servers.json` cannot redirect the game leg at all (recorded above) — but redirecting was never
actually necessary. The DH handshake, if present, is **plaintext by construction** (it bootstraps
encryption — same as Comet's own model), so it doesn't need MITM or decryption to observe, only a
passive capture. Used Windows' built-in `pktmon` (no install, no system change beyond a capture
session — correctly identified as lower-risk than both the network-level IP-redirect and the
in-flight `MsgConnectEx`-rewrite options considered earlier, neither of which was needed):

```
pktmon filter add -p 5816
pktmon start --capture --pkt-size 0 -f game.etl
  (log in, reach the game world)
pktmon stop
pktmon format game.etl -o game_hex.txt --hex   # etl2txt, --hex is required for payload bytes
```

Parsed the output in Python (IP/TCP header lengths computed from IHL/data-offset, not assumed;
deduplicated by (src,sport,dst,dport,seq,length) since `pktmon` logs the same physical packet at
multiple capture points/edges) to reconstruct the true first payload message each direction by TCP
sequence number, not just capture order.

**Result: the very first bytes in BOTH directions are high-entropy, and stay that way.** First
client→server payload: 27 bytes, no structure. First server→client: 16 bytes, immediately followed
(contiguous sequence number, no gap) by a **923-byte message** — a striking size match for a
DH-handshake-shaped message (a single 65-byte prime root alone hex-encodes to 130 ASCII characters,
matching Comet's `MsgHandshake` field sizes) — but scanning the **entire** 923 bytes for the longest
run of pure hex-ASCII-digit characters found only **2 consecutive characters**, and the longest run
of any printable ASCII found only **6**. A real `MsgHandshake`-shaped message would show a
~130-character contiguous hex-text run for `PrimeRoot` alone (Comet's `Encode()` writes
`PrimeRoot`/`Generator`/`ServerKey` as literal hex-string text, not raw binary) — nothing close to
that appears anywhere. Sequence-number continuity across all parsed packets (each packet's payload
ends exactly where the next one's sequence number begins) confirms the parsing itself is sound, not
misaligned.

### First pass wrongly assumed the handshake is cleartext — corrected
An earlier pass through this doc concluded "diverges from Comet" from the absence of readable
hex-ASCII text alone. **That assumption was wrong and got caught:** CO's DH handshake is NOT sent in
the clear — per Comet's own source (`Client.cs:33`, verified directly, not inferred): every new
connection constructs `new BlowfishCipher(BlowfishCipher.Default)` — the hardcoded default seed
(`"DR654dt34trg4UI6"`), zero IVs — and THAT cipher state, not plaintext, encrypts the `MsgHandshake`
message itself, before any DH-derived rekeying happens (`Exchanged()`'s `GenerateKeys()` call, which
only fires after receiving the client's response, comes later). High-entropy-from-byte-0 is exactly
what a correctly-Comet-shaped handshake looks like on the wire — it is not by itself evidence of
divergence. The 923-byte size match to `MsgHandshake`'s expected shape is corroboration, not
something to wave off.

**Ran the actual decisive test** (same validated Blowfish-CFB harness from the black-box
characterization): decrypted the captured 923-byte server→client handshake, the 16-byte message
immediately preceding it (same TCP stream, contiguous sequence numbers, tried concatenated as one
continuous CFB stream too), AND the first 27-byte client→server message — all with Comet's exact
default key + zero IV, sourced directly from source rather than guessed. **All still high-entropy,
zero readable hex-ASCII structure (longest run: 3 characters) in any of them.**

### Where this actually leaves the go/no-go (properly scoped, not overclaimed either direction)
This is a real, correctly-executed negative on the specific hypothesis "this server's handshake is
Comet's `MsgHandshake`, encrypted with Comet's exact default Blowfish key" — tested on both directions
plus a combined-stream attempt, not skipped or mis-aimed this time. It is **not** yet a fully proven
"genuinely custom from scratch" verdict: untested remaining possibilities include (a) a different
default seed than the one hardcoded in Comet's `5187` branch specifically (other patch branches
weren't checked), and (b) this server's very first message might not be structured as `MsgHandshake`
at all — a difference in *when*/*whether* a DH handshake happens, not just how it's keyed. **Don't
let either "confirmed matches Comet" or "confirmed diverges" get inherited as settled by a future
session** — what's actually established is: the obvious, correctly-sourced default-key hypothesis is
ruled out; the underlying bootstrap mechanism is still unidentified.

### Multi-connection diff — decisive, real progress: the cipher state IS static/deterministic
Per-session-key vs static-key is directly testable without any cipher assumptions: capture the same
opening exchange across several independent fresh logins and diff. Captured 4 separate game-server
connections with `pktmon` (same method as above, one continuous capture spanning 3 full
close-and-relaunch cycles, distinguished by each connection's unique client-side ephemeral port).

**Server→client opening (16 bytes): 14 of 16 bytes are byte-for-byte IDENTICAL across all 4
independent connections** — only 2 bytes vary (positions 11-12). **Client→server opening (26-27
bytes): 8-9 bytes constant, a small 5-byte varying window (positions 9-13), then mostly constant
again** for 3 of the 4 connections (the 4th — one connection out of four — diverges more broadly from
byte 0 onward and is 1 byte shorter overall; not yet explained, possibly a different code path e.g.
a reconnect vs. fresh login, not investigated further tonight).

**This is decisive and unambiguous: the cipher starts from a deterministic, static state (fixed
key + IV), not a per-session-derived key.** A per-session key would produce full avalanche across
every byte on every connection — exactly what the black-box characterization earlier tonight PROVED
for the already-established, mid-session game-traffic cipher (four identical plaintexts → four fully
different ciphertexts, zero bytes in common). Here we see the opposite signature: overwhelming
majority constant, small windows varying (almost certainly encrypting short, genuinely-varying
plaintext fields — a nonce, tick, or short counter — inside an otherwise fixed message). **This
rules out per-session/token-derived keying for this opening message specifically**, and confirms the
static-key approach (Comet's default-seed model in kind, even though the exact default-key decrypt
attempt failed) is the right angle to keep working, not a dead end.

**One more structural clue from this same data:** real CFB mode feeds ciphertext back into its own
keystream, so a plaintext difference at one position should cascade and make every later byte differ
too. Here, bytes *after* the small varying window return to matching across the 3 consistent
connections — which does not fit CFB's own defining behavior. Tested CFB vs. a non-chaining OFB-style
variant (same keystream generation, but the register updates from keystream output rather than
ciphertext feedback) across start-offsets 0-8, still with Comet's default key: **no hit** (best result
was a 4-character hex-ASCII run, consistent with noise, not structure). So the *mechanism* (static
key) is now confirmed; the *exact* key and possibly the *exact* mode/framing are still unidentified —
the default Comet seed string itself is now the more likely wrong detail, not the deterministic-vs-
per-session question (which is settled).

### Pending next steps for this thread, in priority order
1. **The exact static key is still unknown.** Comet's literal default seed string was ruled out
   against real captured data (properly this time — both directions, multiple modes/offsets). Next
   candidates: other Comet patch branches' default seeds; reading the key straight out of client
   memory (now much better justified — we know it's a *fixed, findable* value, not something that
   changes per session, which makes a memory search far more tractable than searching for a
   continuously-advancing per-connection state would have been).
2. The small varying windows in both directions (5 bytes c2s, 2 bytes s2c) are themselves worth
   decoding once the key is found — likely short session-identifying fields (nonce/tick/counter), not
   large enough to be a DH public key field on their own.
3. The one outlier connection (58226 — diverges more broadly, 1 byte shorter) wasn't investigated —
   worth checking whether it correlates with a reconnect-after-disconnect rather than a fresh login,
   which would itself be diagnostic.

### Next steps for this thread
1. **Headless crypto foundation needs the actual bootstrap mechanism identified**, not a Comet port.
   That pushes back toward the RE options this doc already scoped (locate the native encrypt/decrypt
   function; the bounded `VirtualQueryEx` scan) — now motivated by a confirmed real target, not a
   hopeful shortcut.
2. The four identical-plaintext↔ciphertext pairs from the black-box characterization above remain the
   confirmation test once any candidate algorithm/key is found: a correct implementation must
   reproduce those exact captured ciphertexts. That's how the algorithm ID gets proven, not assumed,
   regardless of which path finds it.
3. Headless itself is a long, multi-part build — the cipher is the entry ticket, not the bulk of the
   work. Don't scope the rest of it prematurely from this doc.

---

## References

- CO Development Wiki: https://conquer-online.github.io/wiki/ (message index + identifiers pages
  are populated and reliable; most `security/*` pages are empty stubs — don't expect content
  there)
- Comet reference implementation: `gitlab.com/spirited/comet`, branch `5187` (cloned to this
  session's scratchpad, not part of this repo — re-clone if needed later)
- `coclassicbot-live-offsets` memory, sessions 4b/4c (coroutine dead-end, `0xE9960`/`0xBF920`
  hook-resistance) and 8 (`0x1DD450`/`0x1DD860` real pipeline, live-confirmed plaintext pickup)
- `docs/investigation/THEMIDA_NOTES.md` — the data-sourcing ladder and live-interaction risk
  posture this plan follows
- `docs/investigation/TROJAN_DISCONNECT_REPORT.md` / `00_HANDOFF_STATE.md` — the disconnect
  investigation this connects back to via `MsgDisconnect`/`MsgKickOut`
- Session 2026-09-18 scratchpad scripts (not part of this repo, re-derive if needed):
  `read_conn_object_multi.ps1` (live memory read of the connection-object chain),
  `try_decrypt3.py` (TQCipher/Blowfish harness with positive controls + seal-aware scoring —
  the validated oracle to reuse against any future key-schedule candidate)
