#requires -Version 7
<#
.SYNOPSIS
  Always-on, read-only session monitor: records everything observable from OUTSIDE the game about
  the connection and machine, and freezes an "incident bundle" the moment a session drops.

.DESCRIPTION
  Purpose: we keep having disconnections and can only analyze them after the fact. This runs
  alongside the game (no admin needed, changes nothing in the bot or client) and continuously logs:
    * process lifecycle of ImConquer.exe / launcher.exe
    * every TCP connection of the game process, classified (game :5816 / login-relay 127.0.0.1:9959 /
      other external) with each state transition, timestamped (Established -> CloseWait -> gone ...)
    * network health probes (ICMP) to the default gateway, the game-server IP and 1.1.1.1 -- separates
      "my network hiccuped" from "the server dropped me"
    * NIC status / IPv4 changes (Wi-Fi drops, DHCP renewals, adapter resets)
    * game process working set / threads / responding, and how stale the bot's own log is (stall detector)
  Trigger: the game connection leaves Established, or the game process exits. After a short grace period it
  writes  <OutDir>\incidents\<time>_<reason>_pid<PID>\  containing: incident.txt (summary + who-closed-first
  inference), timeline.txt (the last few minutes of every sample), net_events.txt (Windows System/Application
  events, Defender detections, crash dumps in the window), netstat.txt / ipconfig.txt, bot_log_tail.txt,
  relay_packets_tail.txt, and (optional, elevated) a packet capture snapshot.

  "Who closed first" without a packet capture is an INFERENCE from TCP state as seen from this side:
    - our side showed CLOSE_WAIT  -> the server's FIN arrived first (server closed).
    - our side showed FIN_WAIT_*, or TIME_WAIT appeared for that local port -> we sent the first FIN.
    - Established -> gone with neither -> RST/abort, or a closing state shorter than the poll interval.
  The last case is why tools\pktmon_ring.ps1 exists: a packet capture is the definitive answer.

.PARAMETER DurationSec   Stop after N seconds (0 = run until Ctrl+C).
.PARAMETER TestIncident  After ~15 s, force one incident so you can inspect a bundle, then exit.
.PARAMETER PktmonSnapshot  On incident, also snapshot the pktmon ring (requires elevation + a running ring).
.PARAMETER Containment  ESCAPE ALARM. Treat ANY non-loopback TCP connection (including a SYN that a firewall is blocking) by a game-family
                 process (ImConquer, crashpad_handler, ImLauncher, ImBootstrapper) as an escape from the relay: prints a loud
                 "ESCAPE" line and raises a 'containment-escape' incident (deduped per process, 30 s). The game-server leg
                 (5816) counts as an escape too, so use this once the relay carries it (Phase 1), or with the firewall
                 containment (tools\containment.ps1) to see every attempt. TCP only -- UDP is covered by containment.ps1 -Action Report.
#>
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\monitor'),
    [string]$BotLogDir = (Join-Path $PSScriptRoot '..\build\bin\Release'),
    [string]$GameDir = 'F:\Games\Classic Conquer 2.0',
    [int]$PollMs = 300,
    [int]$GraceSec = 8,
    [int]$RingSec = 180,
    [int]$GamePort = 5816,
    [int]$DurationSec = 0,
    [switch]$TestIncident,
    [switch]$PktmonSnapshot,
    [switch]$Containment
)

$ErrorActionPreference = 'SilentlyContinue'
$OutDir = [IO.Path]::GetFullPath($OutDir)
$BotLogDir = [IO.Path]::GetFullPath($BotLogDir)
New-Item -ItemType Directory -Force $OutDir, (Join-Path $OutDir 'incidents') | Out-Null
$sessionLog = Join-Path $OutDir ("session_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))

$ring = [System.Collections.Generic.List[object]]::new()
function Ts { (Get-Date).ToString('HH:mm:ss.fff') }
function Trunc([string]$s, [int]$n) { $s = ($s -replace '\s+', ' '); if ($s.Length -gt $n) { $s.Substring(0, $n) } else { $s } }
function Emit([string]$msg, [switch]$RingOnly) {
    $line = '{0} {1}' -f (Ts), $msg
    $script:ring.Add([pscustomobject]@{ T = Get-Date; L = $line })
    if (-not $RingOnly) { Add-Content -Path $script:sessionLog -Value $line; Write-Host $line }
}
function PruneRing {
    $cut = (Get-Date).AddSeconds(-($RingSec + $GraceSec + 5))
    while ($script:ring.Count -gt 0 -and $script:ring[0].T -lt $cut) { $script:ring.RemoveAt(0) }
}

# ---- state --------------------------------------------------------------------------------------------
$procs = @{}          # pid -> @{Name; Start}
$conns = @{}          # key -> @{Pid; LPort; Remote; RIp; RPort; Kind; State; Hist; Gone}
$nicSig = @{}         # name -> "status|ipv4"
$pending = $null      # incident waiting out its grace period
$lastTrig = @{}       # reason|pid -> time (dedupe)
$stale = @{}          # pid -> $true while its bot log is stale (one incident per episode)
$gameServerIp = $null
$gateway = $null; $nextGw = Get-Date
$nextNic = Get-Date; $nextRes = Get-Date; $nextSummary = (Get-Date).AddSeconds(60)
$pingDefs = @{}       # name -> @{Ip; Ping; Task; Due; Ok; Sent; Lost; Sum; Min; Max; Streak}
$startedAt = Get-Date

function Set-PingTarget([string]$name, [string]$ip) {
    if (-not $ip) { return }
    $d = $script:pingDefs[$name]
    if ($d -and $d.Ip -eq $ip) { return }
    $script:pingDefs[$name] = @{ Ip = $ip; Ping = [System.Net.NetworkInformation.Ping]::new(); Task = $null; Due = Get-Date
        Ok = $true; Sent = 0; Lost = 0; Sum = 0.0; Min = [double]::MaxValue; Max = 0.0 }
}
Set-PingTarget 'cloudflare' '1.1.1.1'

function Kind-Of($c) {
    if ($c.RemotePort -eq $GamePort) { return 'game' }
    if ($c.RemoteAddress -eq '127.0.0.1' -and $c.RemotePort -eq 9959) { return 'login-relay' }
    if ($c.RemoteAddress -notin '127.0.0.1', '::1', '0.0.0.0', '::') { return 'other' }
    return 'ignore'
}

function Trigger([string]$reason, $pid_, [string]$extra) {
    $k = "$reason|$pid_"
    if ($script:pending) { return }
    if ($script:lastTrig[$k] -and ((Get-Date) - $script:lastTrig[$k]).TotalSeconds -lt 30) { return }
    $script:lastTrig[$k] = Get-Date
    $script:pending = @{ Reason = $reason; Pid = $pid_; T = Get-Date; Extra = $extra }
    Emit "INCIDENT triggered: $reason pid=$pid_ $extra  (bundle in ${GraceSec}s)"
}

function Write-Bundle($p) {
    $stamp = $p.T.ToString('yyyyMMdd_HHmmss')
    $dir = Join-Path $OutDir ("incidents\{0}_{1}_pid{2}" -f $stamp, ($p.Reason -replace '[^\w-]', '_'), $p.Pid)
    New-Item -ItemType Directory -Force $dir | Out-Null
    $now = Get-Date

    # timeline = the ring
    ($script:ring | ForEach-Object { $_.L }) | Set-Content (Join-Path $dir 'timeline.txt')

    # who-closed-first inference from this side's TCP state history
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("INCIDENT   : $($p.Reason)  $($p.Extra)")
    $lines.Add("Triggered  : $($p.T.ToString('yyyy-MM-dd HH:mm:ss.fff'))   Bundle written: $($now.ToString('HH:mm:ss'))")
    $pi = $script:procs[[int]$p.Pid]
    if ($pi) { $lines.Add("Process    : $($pi.Name) pid $($p.Pid) started $($pi.Start.ToString('HH:mm:ss'))  (uptime at trigger $([int]($p.T - $pi.Start).TotalSeconds)s)") }
    $lines.Add('')
    $lines.Add('Game connection(s) as seen from this side:')
    foreach ($c in $script:conns.Values | Where-Object { $_.Kind -eq 'game' -and $_.Pid -eq $p.Pid }) {
        $lines.Add("  local :$($c.LPort) -> $($c.Remote)   final state: $($c.State)$(if ($c.Gone) { ' (gone)' })")
        foreach ($h in $c.Hist) { $lines.Add("      $($h.T.ToString('HH:mm:ss.fff'))  $($h.S)") }
        $tw = Get-NetTCPConnection -LocalPort $c.LPort -State TimeWait
        $states = $c.Hist | ForEach-Object { $_.S }
        $verdict = if ($states -contains 'CloseWait') { 'CLOSE_WAIT seen on our side => the server sent its FIN first (server closed the connection).' }
                   elseif ($states -match 'FinWait|Closing') { 'FIN_WAIT seen on our side => WE sent the first FIN (client-initiated close).' }
                   elseif ($tw) { 'TIME_WAIT present for this local port => this side was the active closer (we sent the first FIN).' }
                   elseif ($c.Gone) { 'Went Established -> gone with no closing state and no TIME_WAIT => RST/abort, or a closing state shorter than the ' + $PollMs + 'ms poll. Needs the packet capture to tell.' }
                   else { 'Still present.' }
        $lines.Add("  inference: $verdict")
    }
    $lines.Add('')
    $botLog = Join-Path $BotLogDir ("coclassic_{0}.log" -f $p.Pid)
    if (Test-Path $botLog) {
        $bi = Get-Item $botLog
        $lines.Add("Bot log    : $($bi.Name)  last written $($bi.LastWriteTime.ToString('HH:mm:ss'))  ($([int]($p.T - $bi.LastWriteTime).TotalSeconds)s before trigger)")
        Get-Content $botLog -Tail 400 | Set-Content (Join-Path $dir 'bot_log_tail.txt')
    } else { $lines.Add('Bot log    : none found for this pid') }
    $rp = Join-Path $BotLogDir 'relay_packets.log'
    if (Test-Path $rp) { Get-Content $rp -Tail 200 | Set-Content (Join-Path $dir 'relay_packets_tail.txt') }
    $lines.Add('Launcher log: launcher.log is held exclusively by the running launcher; read it after the launcher exits (its [exit] line gives exit code + cause).')
    $lines | Set-Content (Join-Path $dir 'incident.txt')

    # Windows events around the incident
    $ev = [System.Collections.Generic.List[string]]::new()
    $w0 = $p.T.AddMinutes(-5); $w1 = Get-Date
    $ev.Add("== System log (network/power/time providers, and any Error/Critical), $($w0.ToString('HH:mm:ss'))..$($w1.ToString('HH:mm:ss')) ==")
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $w0; EndTime = $w1 } |
        Where-Object { $_.ProviderName -match 'Tcpip|NDIS|Netwtw|e1[a-z]+express|Rt[A-Za-z0-9]+|Kernel-Power|Wlan|Dhcp|Time-Service|Kernel-General|DNS|WLAN|NetworkProfile' -or $_.LevelDisplayName -in 'Error', 'Critical' } |
        ForEach-Object { $ev.Add(('{0} {1,-28} id={2,-5} {3}: {4}' -f $_.TimeCreated.ToString('HH:mm:ss'), $_.ProviderName, $_.Id, $_.LevelDisplayName, (Trunc $_.Message 180))) }
    $ev.Add("== Application log mentioning the game/bot ==")
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $w0; EndTime = $w1 } |
        Where-Object { $_.Message -match 'ImConquer|coclassic|launcher\.exe' } |
        ForEach-Object { $ev.Add(('{0} {1} id={2}: {3}' -f $_.TimeCreated.ToString('HH:mm:ss'), $_.ProviderName, $_.Id, (Trunc $_.Message 220))) }
    $ev.Add("== Defender detections in the last 15 min ==")
    Get-MpThreatDetection | Where-Object { $_.InitialDetectionTime -gt (Get-Date).AddMinutes(-15) } |
        ForEach-Object { $ev.Add(('{0} threat {1} {2} action-ok={3}' -f $_.InitialDetectionTime.ToString('HH:mm:ss'), $_.ThreatID, ($_.Resources -join ';'), $_.ActionSuccess)) }
    $ev.Add("== Crash dumps modified since $($w0.ToString('HH:mm:ss')) (game dir depth<=4, bot dir) ==")
    foreach ($d in $GameDir, $BotLogDir) {
        Get-ChildItem $d -Recurse -Depth 4 -Include *.dmp, *.mdmp, *.sentry-event -File | Where-Object { $_.LastWriteTime -gt $w0 } |
            ForEach-Object { $ev.Add("$($_.LastWriteTime.ToString('HH:mm:ss'))  $($_.FullName)  ($($_.Length) bytes)") }
    }
    $ev | Set-Content (Join-Path $dir 'net_events.txt')

    (netstat -ano) | Set-Content (Join-Path $dir 'netstat.txt')
    (ipconfig /all) | Set-Content (Join-Path $dir 'ipconfig.txt')

    if ($PktmonSnapshot) {
        $ringScript = Join-Path $PSScriptRoot 'pktmon_ring.ps1'
        if (Test-Path $ringScript) { & $ringScript -Action Snapshot -OutDir $dir *>&1 | Set-Content (Join-Path $dir 'pktmon_snapshot.txt') }
    }
    Emit "BUNDLE written: $dir"
}

# ---- main loop ----------------------------------------------------------------------------------------
Emit "session monitor started (poll ${PollMs}ms, grace ${GraceSec}s, ring ${RingSec}s) log=$sessionLog"
$testDone = $false
while ($true) {
    $loopStart = Get-Date
    if ($DurationSec -gt 0 -and ($loopStart - $startedAt).TotalSeconds -ge $DurationSec) { break }

    # processes
    $ps = @(Get-Process ImConquer, launcher, crashpad_handler, ImLauncher, ImBootstrapper -ErrorAction SilentlyContinue)
    $seenIds = @{}
    foreach ($p in $ps) {
        $seenIds[$p.Id] = 1
        if (-not $procs.ContainsKey($p.Id)) { $procs[$p.Id] = @{ Name = $p.ProcessName; Start = $p.StartTime }; Emit "PROC start  $($p.ProcessName) pid=$($p.Id)" }
    }
    foreach ($id in @($procs.Keys)) {
        if (-not $seenIds.ContainsKey($id)) {
            $info = $procs[$id]; Emit "PROC exit   $($info.Name) pid=$id after $([int]((Get-Date) - $info.Start).TotalSeconds)s"
            if ($info.Name -eq 'ImConquer') { Trigger 'process-exit' $id ('uptime={0}s' -f [int]((Get-Date) - $info.Start).TotalSeconds) }
            $procs.Remove($id)
        }
    }

    # connections of the game process
    $ids = @($ps | Where-Object ProcessName -eq 'ImConquer' | ForEach-Object Id)
    $watchIds = if ($Containment) { @($ps | Where-Object ProcessName -ne 'launcher' | ForEach-Object Id) } else { $ids }
    $cur = @{}
    if ($watchIds.Count) {
        foreach ($c in @(Get-NetTCPConnection -OwningProcess $watchIds)) {
            if ($c.State -in 'Bound', 'Listen') { continue }
            $kind = Kind-Of $c
            if ($Containment -and $c.RemoteAddress -notin '127.0.0.1', '::1', '0.0.0.0', '::') { $kind = 'escape' }
            if ($kind -eq 'ignore') { continue }
            $key = '{0}|{1}|{2}:{3}' -f $c.OwningProcess, $c.LocalPort, $c.RemoteAddress, $c.RemotePort
            $cur[$key] = $c
            $st = [string]$c.State
            if (-not $conns.ContainsKey($key)) {
                $conns[$key] = @{ Pid = $c.OwningProcess; LPort = $c.LocalPort; Remote = "$($c.RemoteAddress):$($c.RemotePort)"; RIp = $c.RemoteAddress; Kind = $kind; State = $st
                    Hist = [System.Collections.Generic.List[object]]::new(); Gone = $false }
                $conns[$key].Hist.Add(@{ T = Get-Date; S = $st })
                Emit "CONN new    [$kind] pid=$($c.OwningProcess) :$($c.LocalPort) -> $($c.RemoteAddress):$($c.RemotePort) $st"
                if ($kind -eq 'escape') {
                    $pn = if ($procs.ContainsKey([int]$c.OwningProcess)) { $procs[[int]$c.OwningProcess].Name } else { '?' }
                    Emit "!!! ESCAPE  $pn pid=$($c.OwningProcess) opened a non-loopback connection -> $($c.RemoteAddress):$($c.RemotePort) ($st)  -- NOT going through the relay"
                    Trigger 'containment-escape' $c.OwningProcess "$pn -> $($c.RemoteAddress):$($c.RemotePort)"
                }
                if ($kind -eq 'game') { $gameServerIp = [string]$c.RemoteAddress; Set-PingTarget 'game-server' $gameServerIp }
            } elseif ($conns[$key].State -ne $st) {
                Emit "CONN state  [$kind] pid=$($c.OwningProcess) :$($c.LocalPort) $($conns[$key].State) -> $st"
                $conns[$key].State = $st; $conns[$key].Hist.Add(@{ T = Get-Date; S = $st })
                if ($kind -eq 'game' -and $st -ne 'Established') { Trigger "game-conn-$st" $c.OwningProcess ":$($c.LocalPort)" }
            }
        }
    }
    foreach ($key in @($conns.Keys)) {
        $cn = $conns[$key]
        if (-not $cur.ContainsKey($key) -and -not $cn.Gone) {
            $cn.Gone = $true; $cn.Hist.Add(@{ T = Get-Date; S = 'gone' })
            $tw = Get-NetTCPConnection -LocalPort $cn.LPort -State TimeWait
            Emit "CONN gone   [$($cn.Kind)] pid=$($cn.Pid) :$($cn.LPort) -> $($cn.Remote) (last $($cn.State))$(if ($tw) { ' TIME_WAIT now held on this port => we were the active closer' })"
            if ($cn.Kind -eq 'game') { Trigger 'game-conn-lost' $cn.Pid ":$($cn.LPort) last=$($cn.State)" }
        }
    }

    # pings (async, never blocks the poll loop)
    if ((Get-Date) -ge $nextGw) {
        $nextGw = (Get-Date).AddSeconds(60)
        $gw = (Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1).NextHop
        if ($gw -and $gw -ne '0.0.0.0') { Set-PingTarget 'gateway' $gw }
    }
    foreach ($name in @($pingDefs.Keys)) {
        $d = $pingDefs[$name]
        if ($d.Task -and $d.Task.IsCompleted) {
            $ok = $false; $rtt = 0; $status = 'Faulted'
            if (-not $d.Task.IsFaulted) { $r = $d.Task.Result; $status = [string]$r.Status; $ok = ($r.Status -eq 'Success'); $rtt = $r.RoundtripTime }
            $d.Sent++
            if ($ok) { $d.Sum += $rtt; if ($rtt -lt $d.Min) { $d.Min = $rtt }; if ($rtt -gt $d.Max) { $d.Max = $rtt }
                Emit "PING $name $($d.Ip) ${rtt}ms" -RingOnly
                if (-not $d.Ok) { Emit "PING $name $($d.Ip) RECOVERED (${rtt}ms)"; $d.Ok = $true }
            } else {
                $d.Lost++
                Emit "PING $name $($d.Ip) FAIL ($status)" -RingOnly
                if ($d.Ok) { Emit "PING $name $($d.Ip) FAIL ($status)"; $d.Ok = $false }
            }
            $d.Task = $null
        }
        if (-not $d.Task -and (Get-Date) -ge $d.Due) { $d.Task = $d.Ping.SendPingAsync($d.Ip, 1000); $d.Due = (Get-Date).AddSeconds(2) }
    }

    # NIC changes
    if ((Get-Date) -ge $nextNic) {
        $nextNic = (Get-Date).AddSeconds(2)
        foreach ($n in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            if ($n.NetworkInterfaceType -in 'Loopback', 'Tunnel') { continue }
            # Windows lists every filter-driver layer (WFP/QoS/Native WiFi...) as its own pseudo-adapter, plus
            # NotPresent placeholders: pure noise for connection forensics.
            if ($n.OperationalStatus -eq 'NotPresent' -or $n.Name -match '-(WFP|QoS|Native|Virtual)|Kernel Debugger|Bluetooth|Local Area Connection') { continue }
            $ip4 = ($n.GetIPProperties().UnicastAddresses | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.IPAddressToString }) -join ','
            $sig = "$($n.OperationalStatus)|$ip4"
            if ($nicSig[$n.Name] -ne $sig) { if ($nicSig.ContainsKey($n.Name)) { Emit "NIC change  $($n.Name): $($nicSig[$n.Name]) -> $sig" } else { Emit "NIC          $($n.Name): $sig" }; $nicSig[$n.Name] = $sig }
        }
    }

    # game process resources + bot-log staleness (stall detector), ring only
    if ((Get-Date) -ge $nextRes) {
        $nextRes = (Get-Date).AddSeconds(5)
        foreach ($p in $ps | Where-Object ProcessName -eq 'ImConquer') {
            $bl = Join-Path $BotLogDir ("coclassic_{0}.log" -f $p.Id)
            $age = if (Test-Path $bl) { [int]((Get-Date) - (Get-Item $bl).LastWriteTime).TotalSeconds } else { -1 }
            Emit ("RES pid=$($p.Id) ws=$([int]($p.WorkingSet64/1MB))MB threads=$($p.Threads.Count) responding=$($p.Responding) botlog-age=${age}s") -RingOnly
            # Stall detector: the bot logs at least every ~30s (memstats) while healthy, so a long silence with the
            # process still alive means the game/DLL hung or the session dropped underneath it (the 07:03 disconnect
            # looked exactly like this: log ends mid-operation, process lingers, launcher later relaunches).
            if ($age -gt 45) { if (-not $stale[$p.Id]) { $stale[$p.Id] = $true; Emit "STALL bot log silent ${age}s while pid=$($p.Id) is alive (responding=$($p.Responding))"; Trigger 'botlog-stale' $p.Id "age=${age}s" } }
            else { $stale.Remove($p.Id) }
        }
    }

    # 60 s summary
    if ((Get-Date) -ge $nextSummary) {
        $nextSummary = (Get-Date).AddSeconds(60)
        $sum = foreach ($name in $pingDefs.Keys) { $d = $pingDefs[$name]; if ($d.Sent) { '{0}: {1}/{2} lost, rtt min/avg/max {3}/{4:N0}/{5}ms' -f $name, $d.Lost, $d.Sent, $(if ($d.Min -eq [double]::MaxValue) { 0 } else { $d.Min }), $(if ($d.Sent - $d.Lost) { $d.Sum / ($d.Sent - $d.Lost) } else { 0 }), $d.Max } }
        $gc = @($conns.Values | Where-Object { $_.Kind -eq 'game' -and -not $_.Gone }).Count
        Emit ("SUMMARY game-conns=$gc  " + ($sum -join '  |  '))
        foreach ($d in $pingDefs.Values) { $d.Sent = 0; $d.Lost = 0; $d.Sum = 0.0; $d.Min = [double]::MaxValue; $d.Max = 0.0 }
    }

    # forced test incident
    if ($TestIncident -and -not $testDone -and ((Get-Date) - $startedAt).TotalSeconds -ge 15) {
        $testDone = $true; $tp = if ($ids.Count) { $ids[0] } else { 0 }
        $script:lastTrig.Clear(); Trigger 'manual-test' $tp 'forced by -TestIncident'
    }

    # bundle when the grace period is over
    if ($pending -and ((Get-Date) - $pending.T).TotalSeconds -ge $GraceSec) {
        Write-Bundle $pending; $pending = $null
        if ($TestIncident) { break }
    }
    PruneRing
    $sleep = $PollMs - [int]((Get-Date) - $loopStart).TotalMilliseconds
    if ($sleep -gt 0) { Start-Sleep -Milliseconds $sleep }
}
Emit 'session monitor stopped'
