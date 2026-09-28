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

## `tools/pktmon_ring.ps1` (ELEVATED) — rolling packet capture
`Start` keeps a 256 MB circular pktmon capture of tcp 5816 + 9959 running; `Snapshot` freezes it to
`snapshot_<time>.etl/.pcapng/.txt` plus `last_packets` and `close_flags` (every FIN/RST in order) and restarts the ring;
`session_monitor.ps1 -PktmonSnapshot` does this automatically on an incident. **Untested elevated** (authoring session had no
admin shell): run `Start` then `Snapshot` once by hand and check the .txt before relying on it.

## Correction to an earlier version of this doc
An earlier draft claimed sessions were "ending abruptly ~7-11 minutes in, with and without the proxy". That was wrong: the
06:54:58 process (pid 110492) ended because the user closed it to fix the account password, not because of a disconnect, so it
is not a data point. The only launcher-detected server-side disconnects on record are the two direct-connection sessions
(uptime 442 s on 2026-09-28 00:19 and 665 s on 2026-09-23) - two cases, no pattern, and nothing yet says anything about the proxy.
Lesson kept for whoever reads this next: a process ending is not a disconnect until the launcher log, the monitor's incident
bundle, or the user says so.

Note: the `walks/s` figures in `[actionrate]` lines count every real outbound packet from the game client (manual play
included), not just the bot's own actions.
