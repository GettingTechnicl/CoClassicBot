#requires -Version 7
<#
.SYNOPSIS
  Windows Scheduled Task wrapper around the local SOCKS5 pass-through (scratchpad\local_socks5_passthrough.py),
  so it can be started on demand instead of babysitting a console window.

.DESCRIPTION
  Install   Registers task 'CoClassicBot-Passthrough' with NO trigger (on-demand only -- exactly what was
            asked for: it never runs on its own, only when you run -Action Start/Test or start it from
            Task Scheduler / `Start-ScheduledTask`). Action = pythonw.exe running tools\passthrough_wrapper.py,
            which redirects the pass-through's prints to monitor\passthrough.log (git-ignored) since pythonw
            has no console. Runs as the current user, standard rights -- no admin needed to install or run.
  Start     Starts it (idempotent: fine to call while already running).
  Stop      Stops it. Because the task's own process IS pythonw.exe running the pass-through directly (no
            cmd.exe wrapper in between), this actually kills the listening process, not just a shell around it.
  Status    Task state + last run result + whether something is actually listening on the port + log tail.
  Test      Starts it if needed, waits for the listen port, then runs tools\socks5_selftest.py to prove a
            REAL SOCKS5 CONNECT through it reaches the login server (not just "port is open"). Leaves it
            running afterward -- this is meant to be left on for the launcher's proxy mode.
  Uninstall Removes the task.

.PARAMETER ScriptPath  The pass-through script to run (default: the project's standing path).
.PARAMETER LogPath      Where the wrapper redirects stdout/stderr (default: monitor\passthrough.log).
.PARAMETER ListenPort   Port the pass-through binds (default 1080, matching toggle_proxy.ps1's default).
#>
param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Uninstall', 'Start', 'Stop', 'Status', 'Test')][string]$Action,
    [string]$TaskName = 'CoClassicBot-Passthrough',
    [string]$ScriptPath = 'C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\local_socks5_passthrough.py',
    [string]$LogPath = (Join-Path $PSScriptRoot '..\monitor\passthrough.log'),
    [int]$ListenPort = 1080,
    [string]$TestHost = 'login.conqueronline.net',
    [int]$TestPort = 9959,
    [int]$WaitSec = 8
)

$ErrorActionPreference = 'Stop'
$LogPath = [IO.Path]::GetFullPath($LogPath)
$WrapperPath = Join-Path $PSScriptRoot 'passthrough_wrapper.py'
$SelfTestPath = Join-Path $PSScriptRoot 'socks5_selftest.py'

function Resolve-Pythonw {
    # Match containment.ps1's egress detection (Get-Command python) so the process this
    # task launches is the SAME python install that firewall containment already allows
    # out, rather than picking a different one of the two Pythons on this machine.
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) {
        $candidate = Join-Path (Split-Path $py) 'pythonw.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    $pyw = (Get-Command pythonw -ErrorAction SilentlyContinue).Source
    if ($pyw) { return $pyw }
    throw "pythonw.exe not found (checked next to 'python' on PATH, then PATH itself)"
}

function Test-Listening {
    [bool](Get-NetTCPConnection -LocalPort $ListenPort -State Listen -ErrorAction SilentlyContinue)
}

switch ($Action) {
    'Install' {
        $pythonw = Resolve-Pythonw
        if (-not (Test-Path $ScriptPath)) { Write-Host "[passthrough] WARNING: script not found at $ScriptPath (task will fail to do anything useful until it exists)" }
        $existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if ($existing) { Write-Host "[passthrough] '$TaskName' already exists - re-registering (Install is idempotent)" }

        $taskAction = New-ScheduledTaskAction -Execute $pythonw -Argument ('"{0}" "{1}" "{2}"' -f $WrapperPath, $ScriptPath, $LogPath) `
            -WorkingDirectory (Split-Path $ScriptPath)
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable `
            -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
        $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

        Register-ScheduledTask -TaskName $TaskName -Action $taskAction -Settings $settings -Principal $principal `
            -Description "On-demand local SOCKS5 pass-through for CoClassicBot's launcher relay (127.0.0.1:$ListenPort). NO TRIGGER -- runs only when started manually (Start-ScheduledTask / tools\passthrough_task.ps1 -Action Start or Test). Managed by tools\passthrough_task.ps1." `
            -Force | Out-Null

        Write-Host "[passthrough] installed task '$TaskName' (on-demand only, no trigger)"
        Write-Host "  runs: $pythonw `"$WrapperPath`" `"$ScriptPath`" `"$LogPath`""
        Write-Host "  log:  $LogPath"
        Write-Host "Next: pwsh -File .\tools\passthrough_task.ps1 -Action Test"
    }

    'Uninstall' {
        if (-not (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue)) { Write-Host "[passthrough] '$TaskName' does not exist"; break }
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "[passthrough] removed task '$TaskName'"
    }

    'Start' {
        if (-not (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue)) { Write-Host "[passthrough] '$TaskName' is not installed - run -Action Install first"; exit 3 }
        if (Test-Listening) { Write-Host "[passthrough] already listening on 127.0.0.1:$ListenPort - nothing to do"; break }
        Start-ScheduledTask -TaskName $TaskName
        for ($i = 0; $i -lt $WaitSec * 5 -and -not (Test-Listening); $i++) { Start-Sleep -Milliseconds 200 }
        if (Test-Listening) { Write-Host "[passthrough] started - listening on 127.0.0.1:$ListenPort" }
        else { Write-Host "[passthrough] started the task but nothing is listening on $ListenPort after ${WaitSec}s - check the log: $LogPath"; exit 4 }
    }

    'Stop' {
        if (-not (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue)) { Write-Host "[passthrough] '$TaskName' is not installed"; break }
        Stop-ScheduledTask -TaskName $TaskName
        Write-Host "[passthrough] stop requested"
        for ($i = 0; $i -lt 25 -and (Test-Listening); $i++) { Start-Sleep -Milliseconds 200 }
        if (Test-Listening) { Write-Host "[passthrough] WARNING: still listening on $ListenPort after stop - check for a stray process" }
        else { Write-Host "[passthrough] confirmed stopped (port $ListenPort free)" }
    }

    'Status' {
        $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if (-not $t) { Write-Host "[passthrough] '$TaskName' is NOT installed. Run: pwsh -File .\tools\passthrough_task.ps1 -Action Install"; break }
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        Write-Host "task state:   $($t.State)"
        Write-Host "last run:     $($info.LastRunTime)   last result: 0x$($info.LastTaskResult.ToString('X'))"
        Write-Host "listening:    $(if (Test-Listening) { "YES - 127.0.0.1:$ListenPort" } else { 'no' })"
        if (Test-Path $LogPath) {
            Write-Host "`n--- last 10 lines of $LogPath ---"
            Get-Content $LogPath -Tail 10
        } else { Write-Host "`n(no log file yet at $LogPath)" }
    }

    'Test' {
        if (-not (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue)) { Write-Host "[passthrough] '$TaskName' is not installed - run -Action Install first"; exit 3 }
        if (-not (Test-Listening)) {
            Write-Host "[passthrough] not listening - starting..."
            Start-ScheduledTask -TaskName $TaskName
            for ($i = 0; $i -lt $WaitSec * 5 -and -not (Test-Listening); $i++) { Start-Sleep -Milliseconds 200 }
        }
        if (-not (Test-Listening)) {
            Write-Host "[passthrough] FAIL: nothing listening on 127.0.0.1:$ListenPort after ${WaitSec}s. Log tail:"
            if (Test-Path $LogPath) { Get-Content $LogPath -Tail 20 }
            exit 4
        }
        Write-Host "[passthrough] listening on 127.0.0.1:$ListenPort - running end-to-end SOCKS5 test to ${TestHost}:${TestPort}..."
        $py = (Get-Command python -ErrorAction SilentlyContinue).Source
        if (-not $py) { Write-Host "[passthrough] python not found on PATH - can't run socks5_selftest.py, but the port IS listening"; exit 0 }
        & $py $SelfTestPath --proxy-port $ListenPort --host $TestHost --port $TestPort
        $rc = $LASTEXITCODE
        if ($rc -eq 0) { Write-Host "[passthrough] leaving the task running - it's ready for the launcher's proxy mode" }
        exit $rc
    }
}
