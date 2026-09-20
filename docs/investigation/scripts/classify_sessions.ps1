param([string]$Dir = "C:\Users\Terry.w11-test\Documents\Claude\CO99\CoClassicBot\build\bin\Release",
       [string]$Out = "sessions.csv")
$now = Get-Date
$rows = New-Object System.Collections.Generic.List[object]
$tsRe = [regex]'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]'
foreach ($f in Get-ChildItem $Dir -Filter 'coclassic_*.log' | Where-Object { $_.Name -match '^coclassic_\d+\.log$' }) {
  $lines = $null
  try { $lines = [IO.File]::ReadAllLines($f.FullName) } catch { continue }
  # split into sessions
  $starts = @()
  for ($i=0; $i -lt $lines.Length; $i++) { if ($lines[$i].Contains('Logger initialized')) { $starts += $i } }
  if ($starts.Count -eq 0) { $starts = @(0) }
  for ($s=0; $s -lt $starts.Count; $s++) {
    $a = $starts[$s]; $b = if ($s+1 -lt $starts.Count) { $starts[$s+1]-1 } else { $lines.Length-1 }
    $char=''; $stuck=$false; $gmh=$false; $fr=$false; $lastKind=''; $lastErr=''; $firstTs=$null; $lastTs=$null; $hero=$false
    $sockSet=@{}; $warnCount=0; $errCount=0
    for ($i=$a; $i -le $b; $i++) {
      $ln = $lines[$i]
      $m = $tsRe.Match($ln)
      if ($m.Success) {
        $t = [int]$m.Groups[1].Value*3600 + [int]$m.Groups[2].Value*60 + [int]$m.Groups[3].Value + [int]$m.Groups[4].Value/1000.0
        if ($null -eq $firstTs) { $firstTs=$t }; $lastTs=$t
      }
      if ($ln.Contains('[init] Hero:')) { $hero=$true; if ($ln -match 'Hero: (\S+)') { $char=$Matches[1] } }
      elseif ($ln.Contains('Hero UID never assigned')) { $stuck=$true }
      elseif ($ln.Contains('CHero::GetMaxHp')) { $gmh=$true }
      elseif ($ln.Contains('[flightrec][inbound]') -and $ln.Contains('kind=')) {
        $fr=$true
        if ($ln -match 'kind=(\w+)') { $k=$Matches[1] }
        if ($ln -match 'wsaError=(\d+)') { $e=$Matches[1] } else { $e='' }
        if ($ln -match 'sock=(0x[0-9A-Fa-f]+)') { $sockSet[$Matches[1]]=1 }
        if (-not ($k -eq 'Error' -and $e -eq '10035')) { $lastKind=$k; $lastErr=$e }
      }
      elseif ($ln.Contains('[flightrec] dumping')) { $fr=$true }
    }
    if ($null -eq $firstTs) { continue }
    $dur = $lastTs - $firstTs; if ($dur -lt 0) { $dur += 86400 }
    # date reconstruction: last session ends at mtime
    $endDt = $f.LastWriteTime
    if ($s -eq $starts.Count-1) {
      $endTod = [TimeSpan]::FromSeconds($lastTs)
      $cand = $endDt.Date + $endTod
      if ($cand -gt $endDt.AddHours(1)) { $cand = $cand.AddDays(-1) }
      $endDt = $cand
    } else { $endDt = $null }
    $startDt = if ($endDt) { $endDt.AddSeconds(-$dur) } else { $null }
    $running = ($s -eq $starts.Count-1) -and (($now - $f.LastWriteTime).TotalMinutes -lt 3)
    $cat =
      if ($running) { 'Running' }
      elseif ($stuck) { 'StuckLogin' }
      elseif (-not $hero) { 'NoLogin' }
      elseif ($fr -and $lastKind -eq 'GracefulClose') { 'GracefulClose' }
      elseif ($fr -and $lastKind -eq 'Error') { 'SockError' + $lastErr }
      elseif ($fr) { 'FlightrecOther:' + $lastKind }
      elseif ($gmh) { 'GetMaxHpNoDump' }
      else { 'SilentDeath' }
    $rows.Add([pscustomobject]@{
      File=$f.Name; Sess=$s; Char=$char; Start=$startDt; End=$endDt; DurMin=[math]::Round($dur/60,1);
      Cat=$cat; Multi=($starts.Count -gt 1); Socks=($sockSet.Keys -join ';') })
  }
}
$rows | Export-Csv -NoTypeInformation (Join-Path (Split-Path $PSCommandPath) $Out)
"sessions: $($rows.Count)"
