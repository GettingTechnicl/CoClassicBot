#requires -Version 7
<#
.SYNOPSIS
  Phase 0 of "the relay sees and controls ALL game traffic": a Windows Firewall backstop that makes any game
  connection that does not go through loopback FAIL (visibly) instead of leaking. See docs/investigation/CONTAINMENT.md.

.DESCRIPTION
  Enable   Create an outbound BLOCK rule, per program, for every .exe under the game folder (ImConquer.exe, the
           official ImLauncher/bootstrapper, crashpad_handler, ...) plus -Extra paths. Each rule blocks all remote
           addresses EXCEPT 127.0.0.0/8 and ::1 (TCP+UDP, IPv4+IPv6). Rules live in the group
           'CoClassicBot-Containment' so one command removes them all.
  Remove   Delete every rule in that group. (This is the panic button.)
  Status   Show the rules, each profile's default outbound action, and the block-log settings. No admin needed.
  LogOn    Turn on Windows Firewall logging of BLOCKED packets (saves your previous settings first).
  LogOff   Restore the logging settings saved by LogOn.
  Report   Parse the firewall log: every blocked outbound attempt grouped by process id + destination ip:port +
           protocol, with process names resolved. This is the discovery tool: it lists exactly which connections
           the game tries (login server, game server, crash telemetry, anything else) that Phase 1 must route.

  THE EGRESS CARVE-OUT (how the relay is still allowed out): Windows Firewall rules are scoped by program, so only the
  game-folder executables are blocked. launcher.exe (which hosts the relay) and the pass-through's python.exe are NOT in
  the blocked set, so they keep their normal outbound access -- that is the sole exit. Nothing else is needed UNLESS a
  profile's default outbound action is Block; Enable detects that and, with -AllowEgress (defaults to launcher.exe +
  the python that runs the pass-through), adds explicit Allow rules for those programs.

  EXPECTED WHILE ENABLED (not bugs): login reaches the relay on loopback, but the game-server leg is blocked until
  Phase 1 routes it, so the game will not finish logging in; crash telemetry (e.g. 34.160.81.0:443), the DLL's Discord
  webhook posts (they come from the game process), and the official updater are blocked too. Run Remove to undo.
  Not covered by design: DNS (resolved by the system DNS service, not the game exe), other users' programs.

  Everything here except Status needs an ELEVATED PowerShell (Windows Firewall requires admin).
.PARAMETER WhatIf  Show what Enable/Remove would do, change nothing (works without admin).
#>
param(
    [Parameter(Mandatory)][ValidateSet('Enable', 'Remove', 'Status', 'LogOn', 'LogOff', 'Report')][string]$Action,
    [string]$GameDir = 'F:\Games\Classic Conquer 2.0',
    [string[]]$Extra = @(),
    [string[]]$AllowEgress = @(),
    [int]$Minutes = 60,
    [string]$MonitorDir = (Join-Path $PSScriptRoot '..\monitor'),
    [string]$LogPath = (Join-Path $env:SystemRoot 'System32\LogFiles\Firewall\pfirewall.log'),
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$Group = 'CoClassicBot-Containment'
$MonitorDir = [IO.Path]::GetFullPath($MonitorDir)
$StateFile = Join-Path $MonitorDir 'containment_state.json'
# Everything except loopback (127.0.0.0/8 and ::1). Windows Firewall already exempts loopback, but excluding it
# explicitly means the relay hop can never be caught by these rules even if that default ever changed.
$V4 = @('0.0.0.0-126.255.255.255', '128.0.0.0-255.255.255.255')
$V6 = @('::', '::2-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff')

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($Action -ne 'Status' -and -not $WhatIf -and -not $isAdmin) {
    Write-Host "[containment] '$Action' needs an ELEVATED PowerShell (Run as administrator). Nothing was changed."
    exit 2
}

function Get-BlockedPrograms {
    $exes = @()
    if (Test-Path $GameDir) { $exes += Get-ChildItem $GameDir -Recurse -Filter *.exe -File -ErrorAction SilentlyContinue | ForEach-Object FullName }
    else { Write-Host "[containment] WARNING: game dir not found: $GameDir" }
    $exes += $Extra
    $exes | Where-Object { $_ } | Sort-Object -Unique
}

function Get-EgressPrograms {
    if ($AllowEgress.Count) { return $AllowEgress }
    $l = Join-Path $PSScriptRoot '..\build\bin\Release\launcher.exe'
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    @($l, $py) | Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { [IO.Path]::GetFullPath($_) }
}

switch ($Action) {
    'Status' {
        Write-Host "== rules in group '$Group' =="
        $r = Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue
        if ($r) { $r | ForEach-Object { '{0,-8} {1,-6} {2}' -f $_.Enabled, $_.Action, $_.DisplayName } } else { '(none - containment is OFF)' }
        Write-Host "`n== default outbound action per profile (Block here means egress programs need explicit Allow rules) =="
        Get-NetFirewallProfile | Select-Object Name, Enabled, DefaultOutboundAction, LogBlocked, LogFileName | Format-Table -AutoSize | Out-String | Write-Host
        Write-Host "== egress programs that stay allowed (NOT blocked) =="
        Get-EgressPrograms | ForEach-Object { "  $_" }
    }

    'Enable' {
        $progs = Get-BlockedPrograms
        if (-not $progs) { Write-Host '[containment] no programs found to block'; exit 3 }
        Write-Host "[containment] blocking non-loopback outbound for $($progs.Count) program(s):"
        $progs | ForEach-Object { "  $_" }
        $blockDefault = Get-NetFirewallProfile | Where-Object { $_.DefaultOutboundAction -eq 'Block' }
        if (-not $WhatIf) { Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue | Remove-NetFirewallRule }
        foreach ($p in $progs) {
            $name = "$Group : $(Split-Path $p -Leaf) [$(([IO.Path]::GetDirectoryName($p)).GetHashCode().ToString('x'))]"
            $args1 = @{ DisplayName = $name; Group = $Group; Direction = 'Outbound'; Action = 'Block'; Program = $p
                        RemoteAddress = ($V4 + $V6); Protocol = 'Any'; Profile = 'Any'; Enabled = 'True'
                        Description = 'Fail-closed containment: game process may only talk to loopback (the relay). Remove: tools\containment.ps1 -Action Remove' }
            try { if ($WhatIf) { New-NetFirewallRule @args1 -WhatIf } else { New-NetFirewallRule @args1 | Out-Null } }
            catch {
                Write-Host "[containment] range syntax rejected for $(Split-Path $p -Leaf): $($_.Exception.Message)"
                Write-Host '[containment] retrying with RemoteAddress Any (loopback is exempt from Windows Firewall filtering by default)'
                $args1.RemoteAddress = 'Any'
                if ($WhatIf) { New-NetFirewallRule @args1 -WhatIf } else { New-NetFirewallRule @args1 | Out-Null }
            }
        }
        if ($blockDefault) {
            Write-Host "[containment] a profile's default outbound action is BLOCK ($($blockDefault.Name -join ', ')): adding explicit ALLOW rules for the egress programs"
            foreach ($e in (Get-EgressPrograms)) {
                $a = @{ DisplayName = "$Group : ALLOW egress $(Split-Path $e -Leaf)"; Group = $Group; Direction = 'Outbound'; Action = 'Allow'; Program = $e; Profile = 'Any' }
                if ($WhatIf) { New-NetFirewallRule @a -WhatIf } else { New-NetFirewallRule @a | Out-Null }
                "  allow: $e"
            }
        }
        Write-Host "`n[containment] ENABLED. Egress that stays open (not blocked): $((Get-EgressPrograms) -join '; ')"
        Write-Host '[containment] Expect game login to stall at the game-server handoff until Phase 1. Undo: pwsh -File .\tools\containment.ps1 -Action Remove'
        Write-Host '[containment] To see what the game tries: -Action LogOn, launch the game, then -Action Report'
    }

    'Remove' {
        $r = Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue
        if (-not $r) { Write-Host '[containment] nothing to remove (already off)'; break }
        if ($WhatIf) { $r | ForEach-Object { "would remove: $($_.DisplayName)" } } else { $r | Remove-NetFirewallRule; Write-Host "[containment] removed $(@($r).Count) rule(s). Containment is OFF." }
    }

    'LogOn' {
        New-Item -ItemType Directory -Force $MonitorDir | Out-Null
        if (-not (Test-Path $StateFile)) {
            Get-NetFirewallProfile | Select-Object Name, LogBlocked, LogAllowed, LogFileName, LogMaxSizeKilobytes | ConvertTo-Json | Set-Content $StateFile
            Write-Host "[containment] saved previous logging settings to $StateFile"
        }
        if ($WhatIf) { Write-Host "would: Set-NetFirewallProfile -All -LogBlocked True -LogAllowed False -LogFileName $LogPath -LogMaxSizeKilobytes 16384" }
        else { Set-NetFirewallProfile -All -LogBlocked True -LogAllowed False -LogFileName $LogPath -LogMaxSizeKilobytes 16384; Write-Host "[containment] blocked-packet logging ON -> $LogPath (16 MB cap)" }
    }

    'LogOff' {
        if (-not (Test-Path $StateFile)) { Write-Host '[containment] no saved settings - nothing to restore'; break }
        $old = Get-Content $StateFile -Raw | ConvertFrom-Json
        foreach ($p in $old) {
            Set-NetFirewallProfile -Name $p.Name -LogBlocked $p.LogBlocked -LogAllowed $p.LogAllowed -LogFileName $p.LogFileName -LogMaxSizeKilobytes $p.LogMaxSizeKilobytes
        }
        Remove-Item $StateFile
        Write-Host '[containment] logging settings restored'
    }

    'Report' {
        if (-not (Test-Path $LogPath)) { Write-Host "[containment] no firewall log at $LogPath - run -Action LogOn first"; exit 3 }
        $cut = (Get-Date).AddMinutes(-$Minutes)
        # pid -> name, from live processes and from the session monitor's PROC start lines
        $names = @{}
        Get-Process -ErrorAction SilentlyContinue | ForEach-Object { $names[[int]$_.Id] = $_.ProcessName }
        Get-ChildItem $MonitorDir -Filter 'session_*.log' -ErrorAction SilentlyContinue | ForEach-Object {
            Select-String -Path $_.FullName -Pattern 'PROC start\s+(\S+) pid=(\d+)' | ForEach-Object { $names[[int]$_.Matches[0].Groups[2].Value] = $_.Matches[0].Groups[1].Value + ' (exited)' }
        }
        $fields = $null; $rows = @{}
        # the log is held open by the firewall service: read it with a shared-read stream
        $fs = [IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite'); $sr = [IO.StreamReader]::new($fs)
        while (-not $sr.EndOfStream) {
            $line = $sr.ReadLine()
            if ($line -like '#Fields:*') { $fields = ($line.Substring(9).Trim() -split ' '); continue }
            if (-not $fields -or $line.StartsWith('#') -or -not $line.Trim()) { continue }
            $v = $line -split ' '
            if ($v.Count -lt $fields.Count) { continue }
            $o = @{}; for ($i = 0; $i -lt $fields.Count; $i++) { $o[$fields[$i]] = $v[$i] }
            if ($o['action'] -ne 'DROP' -or $o['path'] -ne 'SEND') { continue }
            if ([datetime]"$($o['date']) $($o['time'])" -lt $cut) { continue }
            $procid = if ($o.ContainsKey('pid')) { [int]$o['pid'] } else { -1 }
            $key = "$procid|$($o['protocol'])|$($o['dst-ip'])|$($o['dst-port'])"
            if (-not $rows[$key]) { $rows[$key] = [pscustomobject]@{ Pid = $procid; Process = if ($names[$procid]) { $names[$procid] } else { "pid $procid" }; Proto = $o['protocol']; Dest = "$($o['dst-ip']):$($o['dst-port'])"; Count = 0; First = "$($o['time'])"; Last = "$($o['time'])" } }
            $rows[$key].Count++; $rows[$key].Last = "$($o['time'])"
        }
        $sr.Close()
        $out = $rows.Values | Sort-Object Count -Descending
        Write-Host "== blocked outbound attempts, last $Minutes min (each row = one destination the game tried to reach) =="
        $out | Format-Table Process, Pid, Proto, Dest, Count, First, Last -AutoSize | Out-String | Write-Host
        New-Item -ItemType Directory -Force $MonitorDir | Out-Null
        $rf = Join-Path $MonitorDir ("containment_blocks_{0}.txt" -f (Get-Date -Format yyyyMMdd_HHmmss))
        $out | Format-Table Process, Pid, Proto, Dest, Count, First, Last -AutoSize | Out-String -Width 200 | Set-Content $rf
        Write-Host "saved: $rf"
    }
}
