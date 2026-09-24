<#
.SYNOPSIS
  Turn an account's saved SOCKS5 proxy setting on or off in launcher.exe's account store
  (accounts.dat, next to launcher.exe), without going through the "Add Account" dialog again.

.DESCRIPTION
  accounts.dat is JSON. Only `label`, `useProxy` and `proxyHostPort` are stored in plain text --
  username/password/proxyUser/proxyPassword are DPAPI-encrypted blobs (tied to this Windows
  account) that this script never touches, only passes through unchanged. A timestamped .bak
  copy is written before every save.

  REMINDER (see docs / the launcher's own comment in injector/main.cpp): the relay only ever
  proxies the LOGIN/account-server connection. servers.json is the only address it can rewrite;
  the real game server's address is handed to the client dynamically after login and is never
  proxied. Turning this on masks your login handshake, not your gameplay traffic.

.PARAMETER Label
  The account's label exactly as shown in launcher.exe (case-insensitive).

.PARAMETER On / Off
  Which way to set it. Exactly one is required.

.PARAMETER HostPort
  "host:port" for the proxy, e.g. "127.0.0.1:1080". Required the FIRST time you turn an account's
  proxy on if it doesn't already have one saved; optional afterward (omit to keep the saved one,
  or pass it again to change it).

.PARAMETER User / Password
  Optional proxy credentials, only if the proxy requires auth. Stored DPAPI-encrypted like the
  account password.

.PARAMETER AccountsPath
  Defaults to build\bin\Release\accounts.dat next to this script's repo root.

.EXAMPLE
  tools\toggle_proxy.ps1 -Label Main -On -HostPort 127.0.0.1:1080
  tools\toggle_proxy.ps1 -Label Main -Off
#>
param(
    [Parameter(Mandatory=$true)][string]$Label,
    [switch]$On,
    [switch]$Off,
    [string]$HostPort,
    [string]$User,
    [string]$Password,
    [string]$AccountsPath = (Join-Path $PSScriptRoot "..\build\bin\Release\accounts.dat")
)

if ($On -and $Off) { throw "Pass -On or -Off, not both." }
if (-not $On -and -not $Off) { throw "Pass -On or -Off." }

# DPAPI encrypt, matching credentials.cpp's DpapiEncrypt+Base64Encode exactly (CryptProtectData,
# current-user scope, base64-wrapped) -- only used for -User/-Password, which are new plaintext
# input from this script's caller, never for round-tripping an existing blob.
Add-Type -AssemblyName System.Security
function Protect-ForStore([string]$plaintext) {
    if ([string]::IsNullOrEmpty($plaintext)) { return "" }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($plaintext)
    $enc = [System.Security.Cryptography.ProtectedData]::Protect($bytes, $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    return [Convert]::ToBase64String($enc)
}

if (-not (Test-Path $AccountsPath)) {
    throw "accounts.dat not found at $AccountsPath -- add the account once via launcher.exe's ""Add Account"" first (proxy choice at that step doesn't matter, this script overrides it)."
}

$json = Get-Content $AccountsPath -Raw | ConvertFrom-Json
if (-not $json) { $json = @() }
$entry = $json | Where-Object { $_.label -ieq $Label } | Select-Object -First 1
if (-not $entry) {
    $names = ($json | ForEach-Object { $_.label }) -join ", "
    throw "No saved account labelled '$Label' in $AccountsPath. Saved labels: $(if ($names) { $names } else { '(none)' })"
}

if ($On) {
    $effectiveHostPort = if ($HostPort) { $HostPort } elseif ($entry.proxyHostPort) { $entry.proxyHostPort } else { $null }
    if (-not $effectiveHostPort) {
        throw "'$Label' has no saved proxy address yet -- pass -HostPort host:port the first time."
    }
    $entry.useProxy = $true
    $entry.proxyHostPort = $effectiveHostPort
    if ($PSBoundParameters.ContainsKey('User'))     { $entry.proxyUser     = Protect-ForStore $User }
    if ($PSBoundParameters.ContainsKey('Password')) { $entry.proxyPassword = Protect-ForStore $Password }
    Write-Host "[+] '$Label': proxy ON via $effectiveHostPort"
} else {
    $entry.useProxy = $false
    Write-Host "[+] '$Label': proxy OFF (connects directly)"
}

$backup = "$AccountsPath.$(Get-Date -Format yyyyMMdd_HHmmss).bak"
Copy-Item $AccountsPath $backup
# -AsArray, fed through the PIPELINE (not -InputObject): piping unrolls $json element by
# element so ConvertTo-Json sees N objects and -AsArray wraps them as one JSON array either
# way -- for N=1 that's the fix (ConvertTo-Json otherwise drops the [ ] wrapper for a single
# object, confirmed by testing this script against a 1-account file). -InputObject instead
# hands the WHOLE array as a single input, which -AsArray then wraps AGAIN -- double-nested
# [[ ... ]] for N>1, also confirmed by testing against a 2-account file. The C++ loader
# (credentials.cpp) requires root.is_array() with account objects as its direct elements;
# either bug would look like "all accounts vanished" the next time launcher.exe starts.
($json | ConvertTo-Json -Depth 6 -AsArray) | Set-Content $AccountsPath -Encoding utf8
Write-Host "[+] saved (backup: $backup)"
Write-Host "[i] reminder: this only proxies the login connection, not gameplay traffic (see script header / docs/TEAM_NOTES.md)."
