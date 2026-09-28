#requires -Version 7
<#
.SYNOPSIS
  Always-on rolling packet capture of the game (5816) and login (9959) connections using Windows' built-in
  pktmon (no install), so the packets around ANY disconnect already exist when it happens.

.DESCRIPTION
  The definitive answer to "did the server close the connection, or did it drop, or did we?" is in the TCP
  flags on the wire (who sent the first FIN or RST, whether there were retransmits/stalls before it). Doing a
  capture by hand only after a disconnect is too late, so this keeps a fixed-size CIRCULAR capture running
  continuously (game traffic is light; 256 MB holds many hours) and can freeze a copy on demand.

  Payload is captured in full (--pkt-size 0) but is encrypted; headers/flags/timing are what matter here and
  the full packets are kept because the same captures feed the later cipher work
  (docs/investigation/CONNECTION_DECIPHER_PREP.md). Captures stay on this machine.

  Actions (all require an ELEVATED PowerShell; pktmon needs admin):
    Start     clear filters, filter tcp port 5816 + 9959, start the circular capture
    Status    pktmon status
    Snapshot  stop the ring, save ring.etl as snapshot_<time>.etl, convert to .pcapng + a readable .txt with
              TCP flags, extract last_packets.txt / close_flags.txt, then RESTART the ring (gap ~1-2 s)
    Stop      stop the capture

  tools\session_monitor.ps1 -PktmonSnapshot calls Snapshot automatically when it detects an incident.

  NOTE: written but only its non-elevated refusal path has been exercised so far (the authoring session had no
  elevated shell). Run `Start` then `Snapshot` once by hand and check the .txt before relying on it.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Start', 'Stop', 'Status', 'Snapshot')][string]$Action,
    [string]$OutDir = (Join-Path $PSScriptRoot '..\monitor\pktmon'),
    [int]$SizeMB = 256,
    [int[]]$Ports = @(5816, 9959)
)

$ErrorActionPreference = 'Stop'
$OutDir = [IO.Path]::GetFullPath($OutDir)
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[pktmon_ring] this needs an elevated PowerShell (Run as administrator) - pktmon requires admin. Nothing was changed.'
    exit 2
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$ring = Join-Path $OutDir 'ring.etl'

function Start-Ring {
    pktmon stop 2>&1 | Out-Null
    pktmon filter remove 2>&1 | Out-Null
    foreach ($p in $Ports) { pktmon filter add -p $p | Out-Null }
    pktmon start --capture --pkt-size 0 --log-mode circular --file-size $SizeMB -f $ring | Out-Null
    Write-Host "[pktmon_ring] ring capture running -> $ring (circular, $SizeMB MB, ports $($Ports -join ', '))"
}

switch ($Action) {
    'Start'  { Start-Ring }
    'Status' { pktmon status }
    'Stop'   { pktmon stop; Write-Host '[pktmon_ring] stopped' }
    'Snapshot' {
        $stamp = Get-Date -Format yyyyMMdd_HHmmss
        pktmon stop | Out-Null
        if (-not (Test-Path $ring)) { Write-Host '[pktmon_ring] no ring.etl to snapshot (was Start run?)'; exit 3 }
        $snap = Join-Path $OutDir "snapshot_$stamp.etl"
        Copy-Item $ring $snap
        try { pktmon etl2pcap $snap -o (Join-Path $OutDir "snapshot_$stamp.pcapng") | Out-Null } catch { Write-Host "[pktmon_ring] etl2pcap failed: $_" }
        $txt = Join-Path $OutDir "snapshot_$stamp.txt"
        try { pktmon etl2txt $snap -o $txt | Out-Null } catch { Write-Host "[pktmon_ring] etl2txt failed: $_" }
        if (Test-Path $txt) {
            Get-Content $txt -Tail 120 | Set-Content (Join-Path $OutDir "last_packets_$stamp.txt")
            # any FIN / RST in the capture, in order: the first one after the last data is who closed first.
            Select-String -Path $txt -Pattern 'Flags \[[^\]]*[FR][^\]]*\]' | ForEach-Object { $_.Line } |
                Set-Content (Join-Path $OutDir "close_flags_$stamp.txt")
        }
        Write-Host "[pktmon_ring] snapshot saved: $snap (+ .pcapng, .txt, last_packets, close_flags)"
        Start-Ring
    }
}
