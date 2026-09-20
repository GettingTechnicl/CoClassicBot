param([string]$Txt, [string[]]$Times)   # Times = 'HH:mm:ss' of each disconnect (log time); searches +-4 s for the server FIN
$re = [regex]'(\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): Flags \[([^\]]+)\](?:, seq (\d+)(?::(\d+))?)?(?:, ack (\d+))?, win (\d+).*length (\d+)'
function TS($t){ [TimeSpan]::Parse($t.Substring(0,15)).TotalSeconds }
# pass 1: collect every FIN/RST
$sr=[IO.StreamReader]::new($Txt); $t=$null; $dir=''; $fins=New-Object System.Collections.Generic.List[object]
while(($l=$sr.ReadLine()) -ne $null){
  if($l.Length -gt 12 -and $l[2] -eq ':' -and $l.Contains('PktGroupId')){ $t=$l.Substring(0,18); $dir= if($l.Contains('Direction Rx')){'Rx'}else{'Tx'} }
  elseif($l[0] -eq "`t"){ $m=$re.Match($l); if($m.Success -and $m.Groups[5].Value -match '[FR]'){ $fins.Add([pscustomobject]@{T=$t;Dir=$dir;SP=[int]$m.Groups[2].Value;DP=[int]$m.Groups[4].Value;Fl=$m.Groups[5].Value}) } } }
$sr.Close()
foreach($want in $Times){
  $w=[TimeSpan]::Parse($want).TotalSeconds
  $cand = $fins | Where-Object { [math]::Abs((TS $_.T)-$w) -le 4 -and $_.SP -eq 5816 -or ([math]::Abs((TS $_.T)-$w) -le 4 -and $_.DP -eq 5816) } | Sort T
  "=== disconnect near $want ==="
  if(-not $cand){ "  no FIN/RST within +-4 s in this capture"; continue }
  $cand | ForEach-Object { "  {0} {1} flags=[{2}] {3}->{4}" -f $_.T,$_.Dir,$_.Fl,$_.SP,$_.DP }
  $first = $cand | Select -First 1
  $port = if($first.SP -eq 5816){$first.DP}else{$first.SP}
  "  FIRST close packet: $($first.Dir) [$($first.Fl)]  => " + $(if($first.Dir -eq 'Rx'){'SERVER initiated'}else{'CLIENT (our side) initiated'})
  # pass 2: flow packets in the 60 s before + 2 s after
  $lo=$w-60; $hi=$w+4; $sr=[IO.StreamReader]::new($Txt); $pk=New-Object System.Collections.Generic.List[object]
  while(($l=$sr.ReadLine()) -ne $null){
    if($l.Length -gt 12 -and $l[2] -eq ':' -and $l.Contains('PktGroupId')){ $t=$l.Substring(0,18); $dir= if($l.Contains('Direction Rx')){'Rx'}else{'Tx'} }
    elseif($l[0] -eq "`t" -and $t){ $ts=TS $t; if($ts -ge $lo -and $ts -le $hi){ $m=$re.Match($l); if($m.Success -and ($m.Groups[2].Value -eq "$port" -or $m.Groups[4].Value -eq "$port")){ $g=$m.Groups
       $pk.Add([pscustomobject]@{T=$t;TS=$ts;Dir=$dir;Fl=$g[5].Value;Seq=$g[6].Value;Win=[int]$g[9].Value;Len=[int]$g[10].Value}) } } } }
  $sr.Close()
  $fts = TS $first.T
  $pre = $pk | Where-Object { $_.TS -lt $fts }
  $rx = @($pre | Where-Object { $_.Dir -eq 'Rx' -and $_.Len -gt 0 })
  $maxGap=0;$gapAt='';for($i=1;$i -lt $rx.Count;$i++){ $g=$rx[$i].TS-$rx[$i-1].TS; if($g -gt $maxGap){$maxGap=$g;$gapAt=$rx[$i].T} }
  $retx = @($pre | Where-Object { $_.Len -gt 0 } | Group-Object Dir,Seq | Where-Object Count -gt 1).Count
  $zw = @($pre | Where-Object { $_.Win -lt 10 }).Count
  $lastData = ($rx | Select -Last 1)
  "  flow port $port | prior 60s: server data pkts=$($rx.Count), client data pkts=$(@($pre | Where-Object {$_.Dir -eq 'Tx' -and $_.Len -gt 0}).Count), retransmitted segs=$retx, zero-windows=$zw, max server-data gap={0:N2}s (at $gapAt)" -f $maxGap
  if($lastData){ "  last server data packet {0:N0} ms before the close" -f (($fts-$lastData.TS)*1000) }
  $txd = @($pre | Where-Object { $_.Dir -eq 'Tx' -and $_.Len -gt 0 })
  if($txd.Count){ $lt=$txd[-1]; "  last CLIENT data packet {0:N1} ms before the FIN (RTT~30-35 ms; <RTT => not a reaction to it). Tx tail (ms-before/size): {1}" -f (($fts-$lt.TS)*1000), ((($txd | Select -Last 6) | ForEach-Object { '{0:N0}/{1}B' -f (($fts-$_.TS)*1000),$_.Len }) -join '  ') }
}
