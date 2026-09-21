param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [Parameter(Mandatory=$true)][string]$DllPath
)

# Minimal CreateRemoteThread+LoadLibraryA injector -- same technique
# injector/main.cpp's own Inject() uses to load coclassic.dll, just
# callable standalone for one-off diagnostic DLLs (e.g. netfinder.dll)
# without needing a dedicated build target.

$DllPath = (Resolve-Path $DllPath).Path
if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath" }

Add-Type -Name Win32Inject -Namespace Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr GetModuleHandle(string lpModuleName);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr GetProcAddress(IntPtr hModule, string procName);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out IntPtr lpThreadId);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr hObject);
'@

$PROCESS_ALL_ACCESS = 0x1F0FFF
$MEM_COMMIT_RESERVE = 0x3000
$PAGE_READWRITE = 0x04

$hProc = [Native.Win32Inject]::OpenProcess($PROCESS_ALL_ACCESS, $false, $ProcessId)
if ($hProc -eq [IntPtr]::Zero) { throw "OpenProcess failed (are you elevated?) - error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

$bytes = [System.Text.Encoding]::ASCII.GetBytes($DllPath + "`0")
$remoteBuf = [Native.Win32Inject]::VirtualAllocEx($hProc, [IntPtr]::Zero, [uint32]$bytes.Length, $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
if ($remoteBuf -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed" }

$written = [IntPtr]::Zero
if (-not [Native.Win32Inject]::WriteProcessMemory($hProc, $remoteBuf, $bytes, [uint32]$bytes.Length, [ref]$written)) {
    throw "WriteProcessMemory failed"
}

$k32 = [Native.Win32Inject]::GetModuleHandle("kernel32.dll")
$loadLibAddr = [Native.Win32Inject]::GetProcAddress($k32, "LoadLibraryA")
if ($loadLibAddr -eq [IntPtr]::Zero) { throw "GetProcAddress(LoadLibraryA) failed" }

$threadId = [IntPtr]::Zero
$hThread = [Native.Win32Inject]::CreateRemoteThread($hProc, [IntPtr]::Zero, 0, $loadLibAddr, $remoteBuf, 0, [ref]$threadId)
if ($hThread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }

[Native.Win32Inject]::WaitForSingleObject($hThread, 5000) | Out-Null
$exitCode = 0
[Native.Win32Inject]::GetExitCodeThread($hThread, [ref]$exitCode) | Out-Null
[Native.Win32Inject]::CloseHandle($hThread) | Out-Null
[Native.Win32Inject]::CloseHandle($hProc) | Out-Null

if ($exitCode -eq 0) {
    Write-Host "[!] LoadLibraryA returned NULL - injection failed."
    exit 1
} else {
    Write-Host "[+] Injected $DllPath into PID $ProcessId (module base 0x$($exitCode.ToString('X')))"
}
