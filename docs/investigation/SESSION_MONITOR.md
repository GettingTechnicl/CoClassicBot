# Session monitor + packet ring (2026-09-28)

Why: disconnects have only ever been analyzed after the fact, and never from the server's side. Until the game protocol is
decrypted, these two tools record everything observable from OUTSIDE the game so the next disconnect arrives with its context.
Both are read-only: they change nothing in the bot, the client, or Windows settings.

## `tools/session_monitor.ps1` (no admin) — always-on
Run: `pwsh -File tools\session_monitor.ps1` (hidden background: `Start-Process pwsh -ArgumentList '-NoProfile','-File','tools\session_monitor.ps1' -WindowStyle Hidden`).
Stop: `Stop-Process -Name pwsh` for that PID (or Ctrl+C in its window). Output goes to `monitor\` (git-ignored).

Records, timestamped to the ms: ImConquer/launcher start+exit; every game TCP connection classified `game` (:5816) /
`login-relay` (127.0.0.1:9959) / `other`, with each state transition; ICMP probes to the default gateway, the game-server IP and
1.1.1.1 (separates "my network blipped" from "server dropped me"); NIC status/IPv4 changes; game working set / threads /
responding; how stale the bot's own log is. A 60 s summary line gives loss and min/avg/max RTT per target.

Triggers (each waits an 8 s grace period, then writes `monitor\incidents\<time>_<reason>_pid<PID>\`): game connection leaves
Established or disappears, game process exits, or the bot log goes silent >45 s while the process is alive (stall).
A bundle holds: `incident.txt` (summary + who-closed-first inference), `timeline.txt` (last ~3 min of every sample),
`net_events.txt` (Windows System/Application events, Defender detections, crash dumps in the window), `netstat.txt`,
`ipconfig.txt`, `bot_log_tail.txt`, `relay_packets_tail.txt`. `-TestIncident` forces one so you can see a bundle.

**Who closed first (inference from this side's TCP state):** CLOSE_WAIT seen => the server sent its FIN first;
FIN_WAIT/TIME_WAIT => we closed first; Established -> gone with neither => RST/abort *or* a closing state shorter than the
300 ms poll (cannot tell). The packet ring below removes that ambiguity.
Not captured: `launcher.log` (exclusively locked while the launcher runs — read it after it exits; its `[exit]` line has the
exit code and cause).

## `tools/pktmon_ring.ps1` (ELEVATED) - rolling packet capture
`Start` keeps a 256 MB circular pktmon capture of tcp 5816 + 9959 running; `Snapshot` freezes it to `snapshot_<time>.etl` and
`.pcapng`, writes `tcp_summary_<time>.txt`, and restarts the ring. `session_monitor.ps1 -PktmonSnapshot` does this automatically
on an incident. Verified 2026-09-28 (user ran Start + Snapshot elevated; monitor restarted with `-PktmonSnapshot` in the same
admin window): the `.pcapng` decodes cleanly - 138 frames, of which the decodable Ethernet/IPv4/TCP ones are the game flow
`148.113.198.18:5816 <-> 172.16.2.13:<port>` (`PA` data + `A` acks; server sends small 10-12 byte packets every ~1.5-5 s).
The other ~90 frames are the same packets seen at the Wi-Fi layer, encrypted, with junk ethertypes - ignored.

**Read the `.pcapng`, not the `.txt`.** `pktmon etl2txt` output is UTF-16 and full of those encrypted Wi-Fi copies; the first
version of the snapshot step grepped it for FIN/RST and could never have found any. `tools/pcap_tcp_summary.py` decodes the
pcapng instead (stdlib only): per flow it reports who sent the first FIN/RST (SERVER vs CLIENT), how long the flow was silent
before it, retransmits (same seq/len repeated >5 ms later; near-simultaneous repeats are the same packet at two capture points and
are dropped), and the last packets. Run by hand: `python tools\pcap_tcp_summary.py monitor\pktmon\snapshot_<time>.pcapng`.
Timestamps in the summary are UTC (local = UTC-5 in CDT).

## Correction to an earlier version of this doc
An earlier draft claimed sessions were "ending abruptly ~7-11 minutes in, with and without the proxy". That was wrong: the
06:54:58 process (pid 110492) ended because the user closed it to fix the account password, not because of a disconnect, so it
is not a data point. The only launcher-detected server-side disconnects on record are the two direct-connection sessions
(uptime 442 s on 2026-09-28 00:19 and 665 s on 2026-09-23) - two cases, no pattern, and nothing yet says anything about the proxy.
Lesson kept for whoever reads this next: a process ending is not a disconnect until the launcher log, the monitor's incident
bundle, or the user says so.

Note: the `walks/s` figures in `[actionrate]` lines count every real outbound packet from the game client (manual play
included), not just the bot's own actions.

## RESUME HERE - status as of 2026-09-28 and what to do next time

**State left behind (verify before relying on any of it):** the monitor and pktmon ring were started 07:24 in an ELEVATED
PowerShell window (`session_monitor.ps1 -PktmonSnapshot`, ring capture running). They do not survive a closed window or a
reboot. `archer`'s saved proxy is still ON (`127.0.0.1:1080`) and the pass-through may or may not still be running.
Nothing has been observed yet: no disconnect has happened since the tools were in place, so the incident path
(trigger -> grace -> bundle -> automatic pktmon snapshot -> `tcp_summary_*.txt`) is unproven end to end on a REAL event. It was
tested with a forced incident (`-TestIncident`) and the snapshot/decoder were tested separately, but not chained on a real drop.

**Next session - start everything (repo root = `C:\Users\TerryGluff\Documents\Claude\CO99\CoClassicBot`):**
1. Admin PowerShell 7 (Start menu -> PowerShell 7 -> right-click -> Run as administrator), then:
   `cd C:\Users\TerryGluff\Documents\Claude\CO99\CoClassicBot`
2. `pwsh -File .\tools\pktmon_ring.ps1 -Action Start`
3. `pwsh -File .\tools\session_monitor.ps1 -PktmonSnapshot`   (leave this window open; it runs in the foreground)
4. Proxy (only if wanted): `python C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\local_socks5_passthrough.py` in another
   window, and `.\tools\toggle_proxy.ps1 -Label archer -On -HostPort 127.0.0.1:1080` (or `-Off` for a direct connection).
   Note the proxy only carries the LOGIN leg, so it is not a variable for game-session disconnects.
5. Log in via `launcher.exe` and play/idle normally.

**When a disconnect happens (or after a long clean run):** read, in order, `monitor\incidents\<newest>\incident.txt` (who closed
first per TCP state), `monitor\pktmon\tcp_summary_<time>.txt` (first FIN/RST: SERVER or CLIENT, silence before it, retransmits),
`timeline.txt` (pings/NIC/process around it), then the launcher log's `[exit]` line (readable after the launcher exits).
Interpretation: first close from the SERVER with clean pings and no retransmits = the server ended the session (not our network);
first close from the CLIENT = something on this side; RST or long silence + retransmits = network path. Pings failing at the same
moment = local network. Compare against the two launcher-detected disconnects on record (442 s, 665 s uptime, both direct).

**Stop it all when done:** `Stop-Process -Name pwsh` is too broad - use Ctrl+C in the monitor window, then
`pwsh -File .\tools\pktmon_ring.ps1 -Action Stop`. Ring is a fixed 256 MB circular file; monitor logs stay small.

**Other threads still open (not part of this tooling):** first supervised WALK on v1078 (first real execution of
`CRole::SetCommand`, gate `SET_COMMAND_TESTED` is on) -> one ranged shot -> banded-follow live test -> only then autohunt;
Defender may re-quarantine `ImConquer.exe` (folder exclusion is the user's call); per-monster dodge range is parked
(no data source found, upstream bot doesn't model it either).
