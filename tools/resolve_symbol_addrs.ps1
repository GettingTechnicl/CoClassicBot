param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$Mask
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public class SymEnum {
    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);

    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName, string ModuleName, ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);

    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymCleanup(IntPtr hProcess);

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

    public delegate bool SymEnumSymbolsProc(ref SYMBOL_INFOW pSymInfo, uint SymbolSize, IntPtr UserContext);

    [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SymEnumSymbolsW(IntPtr hProcess, ulong BaseOfDll, string Mask, SymEnumSymbolsProc EnumSymbolsCallback, IntPtr UserContext);

    public static List<string> Results = new List<string>();

    public static bool Callback(ref SYMBOL_INFOW sym, uint size, IntPtr ctx) {
        Results.Add(string.Format("{0}\t0x{1:X}", sym.Name, sym.Address));
        return true;
    }
}
"@

$hProcess = [System.Diagnostics.Process]::GetCurrentProcess().Handle
$ok = [SymEnum]::SymInitialize($hProcess, [System.IO.Path]::GetDirectoryName($DllPath), $false)
if (-not $ok) { Write-Host "SymInitialize failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }

$base = [SymEnum]::SymLoadModuleEx($hProcess, [IntPtr]::Zero, $DllPath, $null, 0, 0, [IntPtr]::Zero, 0)
if ($base -eq 0) { Write-Host "SymLoadModuleEx failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }
Write-Host ("Loaded {0} at base 0x{1:X}" -f $DllPath, $base)

$methodInfo = [SymEnum].GetMethod("Callback")
$cb = [Delegate]::CreateDelegate([SymEnum+SymEnumSymbolsProc], $methodInfo)
$ok2 = [SymEnum]::SymEnumSymbolsW($hProcess, $base, $Mask, $cb, [IntPtr]::Zero)
if (-not $ok2) { Write-Host "SymEnumSymbolsW failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

foreach ($r in [SymEnum]::Results) {
    $parts = $r -split "`t"
    $addr = [Convert]::ToUInt64($parts[1].Substring(2), 16)
    $rva = $addr - $base
    Write-Host ("{0}  RVA=0x{1:X}" -f $parts[0], $rva)
}

[SymEnum]::SymCleanup($hProcess) | Out-Null
