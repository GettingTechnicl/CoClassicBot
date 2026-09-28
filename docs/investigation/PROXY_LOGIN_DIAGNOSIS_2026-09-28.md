# Proxy-mode login failure — diagnosis (2026-09-28), no fix applied

Bar: with the launcher's proxy mode on and pointed at the local 127.0.0.1:1080 pass-through, game login must succeed.
Scope: proxy-mode login only (no system-wide routing / capture). Diagnose first, fix only after a go-ahead.

## UPDATE (same day, live attempt): proxy-mode login WORKS on v1078 — the bar is met, no code fix needed

After the user restored the quarantined exe (verified v1078: stamp 0x6AB0822B / 0x2A26000, so the fence accepts it; `version.json` 1079 was
just the new login-screen server entry, not a new client), the account's proxy was enabled (`tools\toggle_proxy.ps1`), the pass-through
started, and one login was run from `launcher.exe` with a connection watcher. Evidence (relay log, pass-through log, watcher trace,
bot logs) — two launches, both reached in-world:

* **Account leg (through both hops):** `ImConquer -> 127.0.0.1:9959` (relay) `-> 127.0.0.1:1080` (pass-through) `-> 148.113.160.82:9959`
  (login server). Relay log: `Accepted client connection` -> `SOCKS5 tunnel established` -> `client->target 202 bytes` ->
  `target->client 6 bytes` -> `Connection closed`. This is the normal short account exchange.
* **Game leg:** immediately after the account leg the client dials the game server **directly** (`172.16.2.13 -> 148.113.198.18:5816`),
  bypassing the relay — the known behavior (address delivered dynamically, not in `servers.json`).
* **In-world:** bot log `Hero: S411 (ID=1174578)` in both processes (35916 at 06:53:22, 110492 at 06:55:32); game stayed up.
* **Bad password:** in launch 110492 two account-leg connections got the same 6-byte reply and closed with no game-leg connection
  (the rejection path, matching the user's mistyped password); the third connection succeeded and went on to :5816.
* **Kill-switch:** the text `KILL-SWITCH: Proxied game connection closed` still prints after each account-leg close but is inert
  (`m_killSwitch=false`) — the game was not terminated.
* Source IP is identical on both legs (pass-through egresses from this machine), so no IP-mismatch between login and game legs.
  With a REAL remote proxy that would differ (login leg from the proxy's IP, game leg from the user's own) — untested, not needed here.

So the earlier "fails" is not reproducible with the prerequisites met. What actually stood in the way today, from the evidence above:
Defender quarantine of the exe (launch dies, `CreateProcess 0xE1`), the pass-through not running (the launcher's pre-launch SOCKS5 test
fails and the game is not launched), and the account's saved proxy flag being off. Which one the user originally hit is unconfirmed.

## Earlier findings (before the live attempt): the failing attempt was NOT on disk, and the client couldn't launch

No v1078 proxy-on attempt exists in `launcher.log` (the proxy blocks in it are all pre-timestamp, i.e. v1074-era, and in the
last of them login *did* succeed through the relay: pre-launch SOCKS5 test OK -> relay listening -> `Login confirmed`).
The account `archer` in `accounts.dat` is saved with `useProxy=False`, and every timestamped session (Sep 23, Sep 28 00:19)
connected directly and logged in fine. `relay_packets.log` is 0 bytes. So the leg that fails cannot be localized from existing
evidence; it needs one instrumented attempt, and three things currently block that attempt:

1. **Defender quarantined `ImConquer.exe`.** `Get-MpThreatDetection`: `Trojan:Win32/Kepavll!rfn` (generic reputation heuristic),
   file `F:\Games\Classic Conquer 2.0\bin\64\ImConquer.exe`, removed 2026-09-28 00:26:44 and 00:26:49 (ActionSuccess=True). This is
   the `CreateProcess failed (0x000000E1)` = `ERROR_VIRUS_INFECTED` at the end of `launcher.log`; `bin\64\ImConquer.exe` is gone.
   It happened in a proxy-OFF session, so it is independent of the proxy work, but nothing can be tested until it is resolved.
   Changing Defender exclusions/restoring quarantine is a security-setting change and is the user's call, not automated.
2. **The client may be mid-update to a newer build.** `version.json` = `{"version":1079}` (touched 00:43 by the official updater);
   v1078 was PE stamp 0x6AB0822B. If the re-downloaded exe is a new build, `BuildFence::IsSupported` refuses to launch it —
   *before* any proxy step runs — with "Unsupported client build". A proxy-login test through `launcher.exe` therefore also depends
   on the fence decision for the new build.
3. **The pass-through is not running** (nothing on 127.0.0.1:1080 / 9959 / 5816) and the account's proxy flag is off.
   Enable with `tools\toggle_proxy.ps1 -Label archer -On -HostPort 127.0.0.1:1080`; start the pass-through with
   `python C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\local_socks5_passthrough.py`.

## What was ruled out offline (no game needed)

* **Account-leg double hop, mechanically:** ran the existing pass-through unmodified and drove it with a client replicating
  `PerformSocks5Handshake` byte-for-byte (greeting `05 01 00`, one-shot CONNECT with ATYP 3 domain). Handshake accepted, tunnel to
  `login.conqueronline.net:9959` (148.113.160.82) established in 0.24 s, replies identical to a direct connection.
  Probe: `scratchpad/tunnel_probe.py`.
* **Pass-through idle timeout (`create_connection(timeout=15)` stays on the upstream socket):** suspected, tested, not the cause
  for the login leg — the login server itself closes an idle connection at exactly 10.0 s, both direct and through the tunnel
  (server does not speak first; 0 bytes either way). It would only bite a leg idle >15 s (e.g. a game leg, if ever routed through it).
* **Relay code:** `HandleClient`/`PumpTraffic` are plain bidirectional pumps; the handshake parser follows RFC 1928; the kill-switch
  is disarmed (`m_killSwitch=false`) so a tunnel closing does not terminate the game (the log text "KILL-SWITCH: ..." is only a message).
* **Patcher vs the v1078-era `servers.json`:** schema unchanged (array of groups -> `servers[]` with `address`/`port`), five entries
  collapse to one unique target `login.conqueronline.net:9959`; `Restore()` leaves it in its original state (currently is).
* **Injected DLL:** hooks only `recv`/`WSARecv` (observational), HWID APIs, render/`SendMsgReal`; nothing hooks `connect()`/DNS, and it
  is loaded identically with proxy on or off.

## The one instrumented attempt (once the blockers are cleared), and how to read it

Order: pass-through up -> proxy flag on -> launch via launcher, log in once. Watch `launcher.log`, `relay_packets.log`, the
pass-through console, and a `netstat -ano` snapshot for the game process during the attempt.

| observation | means |
|---|---|
| `Proxy setup failed` / `SOCKS5 pre-launch test failed` | pass-through down or unreachable (Step 0) — not a relay bug |
| relay listening, but `relay_packets.log` never shows `Accepted client connection` | the v1078 client never dialed `127.0.0.1:9959` — it is not honoring the patched `servers.json` (or takes the login host from somewhere else). Fix would be on how the client is pointed at the relay |
| `Accepted` + `SOCKS5 tunnel established` + bytes both ways, then the login window stays | account leg is fine; failure is after it (MsgConnectEx handoff / direct game-server leg). Check what the client dials next (netstat) |
| `Accepted` then `SOCKS5 handshake failed` / `Failed to connect to SOCKS5 proxy` | hop 2 problem (pass-through died/misparsed) — offline probe says the handshake itself is fine, so look at lifetime |
| `terminating game process` | kill-switch is somehow armed again — check `params.options.m_killSwitch` |

If the account leg completes and the game-server leg is the failure, note that the game leg is delivered dynamically in
MsgConnectEx and is not in `servers.json`, so it goes direct (bypassing the relay) — the known limitation; routing it through the
proxy is explicitly **not** required for this bar.
