param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$OffsetHex
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public class Sym {
    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);

    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName, string ModuleName, ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);

    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymCleanup(IntPtr hProcess);

    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern uint SymSetOptions(uint SymOptions);

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct SYMBOL_INFOW {
        public uint SizeOfStruct;
        public uint TypeIndex;
        public ulong Reserved0;
        public ulong Reserved1;
        public uint Index;
        public uint Size;
        public ulong ModBase;
        public uint Flags;
        public ulong Value;
        public ulong Address;
        public uint Register;
        public uint Scope;
        public uint Tag;
        public uint NameLen;
        public uint MaxNameLen;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=1024)]
        public string Name;
    }

    [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SymFromAddrW(IntPtr hProcess, ulong Address, out ulong Displacement, ref SYMBOL_INFOW Symbol);

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct IMAGEHLP_LINEW64 {
        public uint SizeOfStruct;
        public IntPtr Key;
        public uint LineNumber;
        public IntPtr FileNamePtr;
        public ulong Address;
    }

    [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SymGetLineFromAddrW64(IntPtr hProcess, ulong dwAddr, out uint pdwDisplacement, ref IMAGEHLP_LINEW64 Line);
}
"@

$hProcess = [System.Diagnostics.Process]::GetCurrentProcess().Handle

$ok = [Sym]::SymInitialize($hProcess, [System.IO.Path]::GetDirectoryName($DllPath), $false)
if (-not $ok) { Write-Host "SymInitialize failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }

$base = [Sym]::SymLoadModuleEx($hProcess, [IntPtr]::Zero, $DllPath, $null, 0, 0, [IntPtr]::Zero, 0)
if ($base -eq 0) { Write-Host "SymLoadModuleEx failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }
Write-Host ("Loaded {0} at base 0x{1:X}" -f $DllPath, $base)

$offset = [Convert]::ToUInt64($OffsetHex, 16)
$addr = $base + $offset
Write-Host ("Resolving address 0x{0:X} (base+0x{1:X})" -f $addr, $offset)

$sym = New-Object Sym+SYMBOL_INFOW
$sym.SizeOfStruct = 88   # sizeof(SYMBOL_INFOW) without the Name buffer (fixed part up to and incl. MaxNameLen)
$sym.MaxNameLen = 1024
[UInt64]$disp = 0
$ok2 = [Sym]::SymFromAddrW($hProcess, $addr, [ref]$disp, [ref]$sym)
if ($ok2) {
    Write-Host ("SYMBOL: {0} + 0x{1:X}" -f $sym.Name, $disp)
} else {
    Write-Host ("SymFromAddrW failed: {0}" -f [System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
}

try {
    $line = New-Object Sym+IMAGEHLP_LINEW64
    $line.SizeOfStruct = 40  # sizeof(IMAGEHLP_LINEW64) on x64: DWORD+pad+PVOID+DWORD+pad+PWSTR+DWORD64
    [UInt32]$lineDisp = 0
    $ok3 = [Sym]::SymGetLineFromAddrW64($hProcess, $addr, [ref]$lineDisp, [ref]$line)
    if ($ok3) {
        $fileName = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($line.FileNamePtr)
        Write-Host ("LINE: {0}:{1} (+0x{2:X})" -f $fileName, $line.LineNumber, $lineDisp)
    } else {
        Write-Host ("SymGetLineFromAddrW64 failed: {0}" -f [System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
} catch {
    Write-Host ("LINE lookup exception: {0}" -f $_.Exception.Message)
}

[Sym]::SymCleanup($hProcess) | Out-Null
