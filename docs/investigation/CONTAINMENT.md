# Containment — "all game traffic goes through the relay, nothing escapes" (2026-09-28)

Goal (user): the launcher's relay sees and controls ALL of the game's traffic (login server, game server, anything else), and
nothing can bypass it, so everything is monitorable. IP masking is irrelevant. Approved design, three layers:

| Layer | What | Status |
|---|---|---|
| 1. Fail-closed firewall | Windows Firewall per-program outbound BLOCK for every game-folder exe except loopback. Independent of any hook, so it also catches early connects and anything we forget to hook. | **Phase 0 — built (`tools\containment.ps1`)** |
| 2. Redirect | In-process DLL hooks of the ws2_32 connect family send each connection to a relay gateway; relay becomes a SOCKS5 gateway with a direct-connect egress mode. | Phase 1 — **design proposed, awaiting go-ahead** |
| 3. Escape alarm | Monitor flags any non-loopback connection by a game process. | **Phase 0 — built (`session_monitor.ps1 -Containment`)** |

## Egress carve-out (how the relay still gets out)
Windows Firewall rules are scoped by program, and a Block rule beats an Allow rule, so the carve-out is done by *not blocking*
the egress processes rather than by allow-over-block. Blocked set = every `.exe` under `F:\Games\Classic Conquer 2.0`
(ImConquer.exe, crashpad_handler.exe, ImLauncher.exe, ImBootstrapper.exe, unins000.exe) plus `-Extra` paths. Deliberately NOT blocked:
`launcher.exe` (hosts the relay; the future direct-connect egress) and the python running `local_socks5_passthrough.py` (today's
egress: game → relay → 127.0.0.1:1080 → internet). If any firewall profile has default-outbound = Block, `Enable` adds explicit Allow
rules for those two programs (`-AllowEgress` overrides the list). Each rule blocks all remote addresses except 127.0.0.0/8 and ::1,
TCP+UDP, IPv4+IPv6, all profiles. Everything is in rule group `CoClassicBot-Containment`.

## Expected while it's on (NOT bugs)
* Login through the relay (`127.0.0.1:9959`) works, but the game-server leg (`148.113.198.18:5816`) is blocked → the client will not
  finish logging in until Phase 1 routes that leg.
* Blocked too: crash telemetry (`34.160.81.0:443`), the DLL's Discord webhook posts (they originate in the game process), the official updater.
* Enable BEFORE launching the game: rules apply to new connections; an already-established connection is not necessarily cut.
* DNS is done by the system resolver, not the game exe, so it is not blocked.

## Commands (elevated PowerShell 7, repo root `C:\Users\TerryGluff\Documents\Claude\CO99\CoClassicBot`)
```
pwsh -File .\tools\containment.ps1 -Action Status              # no admin needed
pwsh -File .\tools\containment.ps1 -Action LogOn               # firewall logs blocked packets (saves old settings)
pwsh -File .\tools\containment.ps1 -Action Enable              # containment ON   (add -WhatIf to dry-run)
pwsh -File .\tools\containment.ps1 -Action Report -Minutes 30  # what the game tried: process, pid, proto, dest ip:port, count
pwsh -File .\tools\containment.ps1 -Action Remove              # containment OFF (panic button)
pwsh -File .\tools\containment.ps1 -Action LogOff              # restore previous logging settings
pwsh -File .\tools\session_monitor.ps1 -Containment            # escape alarm (any admin level)
```
Report resolves pids via live processes and the monitor's `PROC start` lines; run the monitor during the session so exited pids resolve.
`Report` is the discovery tool: it enumerates every destination the game (and crashpad/ImLauncher) attempts, settling from the outside
which egress Phase 1 must route (login, game, telemetry, anything else, TCP or UDP, v4 or v6).

## Verified so far (no admin available in the dev session)
* Script parses; non-admin refusal works; `Status` works; `Enable -WhatIf` enumerates the 5 game-folder exes and 2 egress programs.
* The escape alarm fired correctly against the real running game: `ESCAPE ImConquer pid=111040 -> 148.113.198.18:5816 (Established)` (today the game leg is direct, which is exactly what it should flag).
* NOT yet exercised: `Enable`/`LogOn`/`Report` for real (need the user's elevated shell). If `New-NetFirewallRule` rejects the IPv6 range syntax the script
  retries with RemoteAddress Any (loopback is exempt from Windows Firewall filtering anyway) and prints that it did.

## Phase 1 sketch (proposal only — do not build without go-ahead)
1. Relay → SOCKS5 gateway: per-connection destination from the CONNECT request, both-direction logging (keeps `relay_packets.log`), **direct-connect
   egress mode** (retires the Python pass-through: one hop, one process to allow), thin low-latency pumps (TCP_NODELAY, no per-packet logging on the hot path
   unless enabled), and per-connection latency measurement (client→relay→server vs direct baseline) because the game leg is persistent and real-time.
2. DLL hooks (Detours) for the whole ws2_32 connect set — `connect`, `WSAConnect`, `ConnectEx` (via WSAIoctl function pointer), `WSAConnectByNameW/A`, `WSAConnectByList` —
   rewriting non-loopback destinations to the gateway with the real destination handed over in the SOCKS5 CONNECT; fail closed if the relay is unreachable.
3. Early-connect gap: the DLL is injected ~6 s after launch; connects before that (telemetry) rely on the firewall. Option: suspended-launch injection.
4. Fallback if hooks prove unstable in the Themida client: WinDivert/WFP redirect driver (a signed kernel component — larger step, propose separately).

## Phase 0 discovery result (2026-09-28 08:56-08:58, two launches, user ran Enable+LogOn+Report)
Blocked outbound attempts by the game (all from `ImConquer.exe`; nothing from crashpad_handler/ImLauncher/ImBootstrapper):

| Destination | Proto | Seen | Meaning |
|---|---|---|---|
| `34.160.81.0:443` | TCP | once per launch, ~9 s BEFORE the game-server attempt | telemetry/crash-reporting-style HTTPS (Google Cloud IP); fires early, likely before the DLL is injected |
| `148.113.198.18:5816` | TCP | after the login leg, 1-2 SYNs per launch (both launches) | game-server leg; the address arrives dynamically in MsgConnectEx |

No UDP, no IPv6, no other host was attempted. Login leg (`127.0.0.1:9959`, loopback) is not in the log by design. Both game processes then exited
(game leg blocked, as expected). Caveat: the game never got in-world, so anything it would only do AFTER login (in-game web calls, a second server, etc.)
is not covered by this observation; re-run Report after Phase 1 routes the game leg. This does NOT say which ws2_32 function issues the connects.

## Phase 1 Step 0 result: WAN latency baseline (2026-09-28, tools/latency_baseline.py, 30 samples/target)
| target | median | p95 | p99 | max |
|---|---|---|---|---|
| login-server 148.113.160.82:9959 | 36.42ms | 63.45ms | 77.51ms | 82.06ms |
| game-server 148.113.198.18:5816 | 34.67ms | 52.28ms | 55.19ms | 56.08ms |

0 failures on either target. This is raw TCP-handshake RTT with no relay involved -- the number the relay's own added
overhead (Step 2 gate: median <=1ms, p99<=3ms, measured once the gateway exists) sits on top of. At these WAN numbers,
a 1-3ms relay hop is well under 10% of even the median, i.e. in the noise.

## Phase 1 Step 1: connectfinder.dll (observe-only connect hook) — built, not yet run live
`src/connectfinder.cpp` (new standalone diagnostic DLL, same pattern as `netfinder`/`recvfinder`): Detour-hooks every ws2_32 export
that can originate an outbound connect — `connect`, `WSAConnect`, `WSAConnectByNameA`, `WSAConnectByNameW`, `WSAConnectByList` — plus
`ConnectEx`, caught indirectly via a `WSAIoctl` hook watching for `SIO_GET_EXTENSION_FUNCTION_POINTER` + `WSAID_CONNECTEX`, then
Detoured once. Every hook calls straight through with unmodified args/results — zero behavior change, nothing is redirected.
Logs one JSON line per connect attempt to `C:\Users\Public\coclassic_connectfinder.json`: api name, socket, destination `ip:port`
(or hostname/service for the ByName variants), calling thread id, and every return-address frame that lands inside the game
module (RVA), so the call site is visible too. Built clean (`build\bin\Release\connectfinder.dll`), not yet injected/run live.

Test plan (needs the user driving the live game — not run yet): inject into `ImConquer.exe` via `tools\inject_dll.ps1` AFTER
launching via `launcher.exe` with the proxy on and firewall containment OFF (containment would block the game leg before this
hook gets to see the redirect target), log in, get in-world, do a few actions, then read `coclassic_connectfinder.json` for which
function(s) fired for the login leg and the game leg, plus whatever else connects post-login. Run `session_monitor.ps1
-Containment` alongside it (no conflict — it just watches, and it now also catches anything post-login the earlier
firewall-only Report couldn't see, per that section's caveat above).

## Pass-through as an on-demand Scheduled Task (2026-09-28)
Per user request, the local SOCKS5 pass-through now runs as Windows Scheduled Task `CoClassicBot-Passthrough`
(`tools\passthrough_task.ps1`: Install/Uninstall/Start/Stop/Status/Test) instead of a manually-run console window.
No trigger — on-demand only, starts only via `-Action Start`/`Test` or manually from Task Scheduler. Runs as the
current user, standard rights; Install/Start/Test/Status all worked with **no admin needed**. The task's actual
process is `pythonw.exe` running `tools\passthrough_wrapper.py` directly (no `cmd.exe` layer), so `Stop` reliably
kills the real listening process rather than orphaning it. pythonw has no console, so the wrapper redirects
stdout/stderr to `monitor\passthrough.log` (git-ignored). `containment.ps1`'s egress carve-out was updated to
allow `pythonw.exe` (same folder as `python.exe`) alongside it, since that's now the actual egress process.
`tools\socks5_selftest.py` (new, recreates the now-gone `tunnel_probe.py` check as a permanent tool) does a real
SOCKS5 CONNECT through the pass-through to the login server and verifies the SOCKS5 success reply, not just that
the port is open. Verified live: Install → Test → PASS (166ms CONNECT to `login.conqueronline.net:9959`) → Status
confirmed `Running` + listening + log shows the exact expected trace. Left running for use with the launcher.

## Phase 1 Step 1 interim status (2026-09-28, live injection via System Informer)
`connectfinder.dll` injected into a running `ImConquer.exe` (pid 27732) via System Informer. Hook install diag confirms
clean attach: all 6 targets resolved, every `err_*` = 0 (connect, WSAConnect, WSAConnectByNameA/W, WSAConnectByList,
WSAIoctl-for-ConnectEx). At the moment of injection the process was ALREADY connected in-world -- established
connections to `148.113.198.18:5816` (game leg) and `34.160.81.0:443` (telemetry, unblocked since containment is
off for this step) both predate the hook, so they were never observed (a hook only sees calls made after it
attaches -- expected, not a bug). No connect events logged yet. Next: a fresh connect made AFTER injection (relaunch
via launcher.exe + inject connectfinder as early as possible, or any other action that opens a new outbound
connection) is needed to actually capture which function the client calls.

## Phase 1 Step 1 RESULT (2026-09-28, relaunch + reinject): the client uses plain `connect()` only
Two connect events captured post-injection, both via the bare `connect` export — WSAConnect, WSAConnectByNameA/W,
WSAConnectByList and ConnectEx were all hooked (see interim status above) but NEVER called:

| api | dest | game-module call frames (RVA, shallow→deep) |
|---|---|---|
| connect | 127.0.0.1:9959 (login leg, via relay) | 0x1C6F85 → 0x199E06 → 0x10CC7D1 → 0xE8CB5 → 0xC1656 → 0xEB678 → 0x39C1C9 |
| connect | 148.113.198.18:5816 (game leg, direct) | 0x1C6F85 → 0x199E06 → 0x1F236A → 0x199CF8 → 0xC1368 → 0xEB678 → 0x39C1C9 |

Both share the same shallow two frames (0x1C6F85, 0x199E06 — almost certainly a thin connect-wrapper and its caller,
one level above raw `connect()`), diverge at the login-specific vs game-specific call site (0x10CC7D1 vs 0x1F236A),
then reconverge at 0xEB678 → 0x39C1C9 — a shared outer dispatcher both paths route through. Not yet correlated
against netfinder's send-path RVAs; worth doing before Phase 1 Step 2 if a stable network-layer landmark is wanted.

**Conclusion for Phase 1 Step 2:** the real redirect hook needs to cover `connect()` as the confirmed, evidence-backed
primary target. Recommend keeping the other 5 hooked too (already built, zero extra cost, and a future client update
could switch APIs) but `connect()` is what actually matters today for both the login and game legs.

## Phase 1 Step 2: direct-connect gateway + redirect hook — BUILT, compiled clean, NOT yet live-tested
Implements the design agreed after Step 1's result: the game leg now gets carried by the relay too.

**Gateway (`injector/main.cpp`, new `ConnectGateway` class, right after `Socks5Relay`):** one per account's worker
thread, alongside the existing `Socks5Relay relay;`. Listens on an ephemeral loopback control port. Protocol:
DLL sends `CONNECT host:port\n`, gateway replies `PORT nnnn\n`, DLL then connects to `127.0.0.1:nnnn` itself — a
per-destination loopback listener, created on first request and reused for repeat connects to the same destination
(each accepted connection still gets its own fresh direct dial to the real target). Egress is DIRECT (no upstream
SOCKS5) — this is what retires the Python pass-through's reason for existing on the game leg; the pass-through
itself is untouched and still carries the login leg / would carry a real upstream proxy. `GatewayPump` fixes the
FIN/RST propagation gap noted in the original Phase 1 proposal: a real socket error (reset/aborted) now produces a
real RESET on the other side (`SO_LINGER 0`) instead of always turning every close into an orderly FIN — matters
because `pcap_tcp_summary.py`'s disconnect forensics reads FIN-vs-RST off the wire.

**Wiring:** `ConnectGateway* gateway` added to `SupervisionParams`; started right after `relay.Start()` succeeds,
stopped at all three teardown sites (build-fence-unsupported, CreateProcess-failed, normal end-of-session) alongside
`relay.Stop()`. `COCLASSIC_GW=127.0.0.1:<controlPort>` is set on the launcher's own process environment (inherited by
`CreateProcessA` since `lpEnvironment=nullptr`) immediately before EVERY `CreateProcessA` call — including relaunches
— guarded by a new `g_envVarMutex`, since multiple accounts can be launching concurrently and the environment block is
shared process-wide. A non-gateway launch explicitly clears it too, so a prior gateway-mode account's stale value can
never leak into an unrelated later launch.

**Redirect hook (`src/net_connect_hook.{h,cpp}`, new, added to `COCLASSIC_SOURCES` — both DLL targets):** hooks the
same 6 targets `connectfinder.dll` proved out in Step 1. `connect()`, `WSAConnect()`, `ConnectEx()` all take a plain
`sockaddr` destination and get REAL redirect logic (ask the gateway for a loopback port, rewrite the destination,
call through). `WSAConnectByNameA/W` and `WSAConnectByList` resolve a hostname internally with no sockaddr to
rewrite — consistent with "nothing escapes over everything works", these three FAIL CLOSED (refuse + log loudly)
if the gateway is armed and they're ever called, rather than passing an unrouted connection through silently. Every
redirect failure (gateway unreachable, control protocol timeout/error) also fails closed (`WSAECONNREFUSED`), never
falls through to the real address. Loopback destinations (the login leg, already patched to `127.0.0.1:9959`) are
always left untouched. Gated on the `COCLASSIC_GW` env var — unset (manual injection, or proxy off) means every hook
is pure pass-through, identical in effect to `connectfinder.dll`'s observe-only build.

**Timing (why this isn't the same trap Step 1 hit):** `InitNetConnectHook()` is called as the FIRST line of
`InitThread` in `dllmain.cpp` — before even the build-fence check (it's a generic ws2_32 hook with zero game-offset
dependence, so it stays armed even on a build the fence would otherwise refuse) and long before the hero-UID wait
that gates everything else. This mirrors `HwidSpoof::Init`'s existing precedent (also must run before login, also
done via this same async `InitThread` rather than synchronously in `DllMain`) — and by construction, `launcher.exe`'s
`Inject()` call only returns once `DllMain`/`DLL_PROCESS_ATTACH` has fully completed, and auto-login (which is what
actually triggers the account-server connect) only proceeds after `Inject()` returns — so for a normal
launcher-driven launch, this hook has already had time to install before the game makes its first connect. (This is
why Step 1's manual System-Informer injection, done well after the game was already in-world, needed a relaunch to
observe anything — that was a manual-injection artifact, not a property of the real launch path.)

Build: clean, zero errors/warnings, `coclassic_v1078.dll` + `launcher.exe`, both DLL targets get the hook (shared
`COCLASSIC_SOURCES`).

**NOT yet done:** no live test. Needs: launch via `launcher.exe` with proxy mode on, confirm login still works and
gets in-world, confirm `relay_packets.log` now shows both legs (not just login), confirm `containment.ps1 -Action
Report` / `session_monitor.ps1 -Containment` show zero escapes with Phase 0's firewall back on, and a latency
comparison against Step 0's baseline.
