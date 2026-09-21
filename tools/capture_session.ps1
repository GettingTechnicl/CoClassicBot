param(
    [Parameter(Mandatory=$true)][string]$Label,
    [switch]$Full,                    # also dump the private heap (use this on a build whose object roots are unknown)
    [string]$Note = "",               # what is on screen right now: hp/mp/level/silver/map/coords/panel open ...
    [int]$TimeoutSec = 240
)

# Ask an already-injected imgdump.dll to take a checkpoint and wait for it to finish, then append a line to
# notes.txt in the session dir so the dumps can be correlated offline with what the player saw.
# Read-only on the game: imgdump reads process memory only (no hooks, no calls into game code).

$base = "C:\Users\Public\coclassic_capture"
$done = Join-Path $base "last_done.txt"
$before = if (Test-Path $done) { (Get-Item $done).LastWriteTimeUtc } else { [datetime]::MinValue }

$verb = if ($Full) { "full" } else { "checkpoint" }
Set-Content -Path (Join-Path $base "trigger.txt") -Value "$verb $Label" -Encoding ascii
Write-Host "[..] requested '$verb $Label' - waiting for imgdump (the game may hitch for a few seconds)"

$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $done) -and ((Get-Item $done).LastWriteTimeUtc -gt $before)) {
        $dir = (Get-Content $done -TotalCount 1).Trim()
        $meta = Join-Path $dir "meta.json"
        if (Test-Path $meta) { Get-Content $meta | Select-String 'image_bytes|nonzero|heap_|hero_|map_id' | ForEach-Object { $_.Line.Trim() } }
        $line = "{0}  {1}  {2}  {3}" -f (Get-Date -Format s), (Split-Path $dir -Leaf), $verb, $Note
        Add-Content -Path (Join-Path (Split-Path $dir -Parent) "notes.txt") -Value $line
        Write-Host "[+] done: $dir"
        exit 0
    }
}
Write-Host "[!] timed out waiting for the checkpoint; check $base\imgdump.log"
exit 1
