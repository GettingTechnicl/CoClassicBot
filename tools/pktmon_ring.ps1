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
    Snapshot  stop the ring, save ring.etl as snapshot_<time>.etl, convert to .pcapng, write tcp_summary_<time>.txt
              (tools\pcap_tcp_summary.py: who sent the first FIN/RST, silence before it, retransmits, last
              packets), then RESTART the ring (gap ~1-2 s)
    Stop      stop the capture

  tools\session_monitor.ps1 -PktmonSnapshot calls Snapshot automatically when it detects an incident.

  STATUS: Start + Snapshot were run elevated by the user on 2026-09-28 and produced .etl/.pcapng; the .pcapng decodes
  cleanly (see pcap_tcp_summary.py). The Snapshot step that runs the summary is new since that run.
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
        # NOTE: `pktmon etl2txt` output is UTF-16 and mixes in encrypted Wi-Fi-layer copies of every packet, so it is
        # NOT used for analysis (an earlier version tried to grep it for FIN/RST and found nothing, wrongly). The .pcapng
        # is decoded by tools\pcap_tcp_summary.py instead: per flow, who sent the first FIN/RST, silence before it,
        # retransmits, last packets.
        $pcap = Join-Path $OutDir "snapshot_$stamp.pcapng"
        $summary = Join-Path $OutDir "tcp_summary_$stamp.txt"
        $py = Get-Command python -ErrorAction SilentlyContinue
        if ($py -and (Test-Path $pcap)) {
            & $py.Source (Join-Path $PSScriptRoot 'pcap_tcp_summary.py') $pcap 2>&1 | Set-Content $summary
            Write-Host "[pktmon_ring] TCP summary: $summary"
        } else { Write-Host '[pktmon_ring] python not found - run tools\pcap_tcp_summary.py on the .pcapng manually' }
        Write-Host "[pktmon_ring] snapshot saved: $snap (+ .pcapng, tcp_summary)"
        Start-Ring
    }
}
