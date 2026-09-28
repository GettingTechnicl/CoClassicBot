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
