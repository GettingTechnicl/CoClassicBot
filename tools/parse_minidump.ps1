param(
    [Parameter(Mandatory=$true)][string]$Path
)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;

public class MiniDumpParser {
    public static string ReadMiniDumpString(FileStream fs, uint rva) {
        long saved = fs.Position;
        fs.Seek(rva, SeekOrigin.Begin);
        byte[] lenBuf = new byte[4];
        fs.Read(lenBuf, 0, 4);
        uint lenBytes = BitConverter.ToUInt32(lenBuf, 0);
        if (lenBytes > 2000) lenBytes = 2000; // sanity cap against misparsed rva
        byte[] strBuf = new byte[lenBytes];
        fs.Read(strBuf, 0, (int)lenBytes);
        fs.Seek(saved, SeekOrigin.Begin);
        return Encoding.Unicode.GetString(strBuf);
    }

    public static void Parse(string path) {
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            byte[] hbuf = new byte[32];
            fs.Read(hbuf, 0, 32);
            uint signature = BitConverter.ToUInt32(hbuf, 0);
            uint numStreams = BitConverter.ToUInt32(hbuf, 8);
            uint streamDirRva = BitConverter.ToUInt32(hbuf, 12);
            uint timeDateStamp = BitConverter.ToUInt32(hbuf, 20);

            if (signature != 0x504D444D) {
                Console.WriteLine("NOT A VALID MINIDUMP");
                return;
            }

            DateTime dt = new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc).AddSeconds(timeDateStamp).ToLocalTime();
            Console.WriteLine("Dump local time: " + dt.ToString());
            Console.WriteLine("Streams: " + numStreams);
            Console.WriteLine("");

            fs.Seek(streamDirRva, SeekOrigin.Begin);
            byte[] entry = new byte[12];

            uint exStreamRva = 0, exStreamSize = 0;
            uint modStreamRva = 0, modStreamSize = 0;
            uint sysInfoRva = 0;
            uint threadListRva = 0, threadListSize = 0;

            for (uint i = 0; i < numStreams; i++) {
                fs.Read(entry, 0, 12);
                uint streamType = BitConverter.ToUInt32(entry, 0);
                uint dataSize = BitConverter.ToUInt32(entry, 4);
                uint rva = BitConverter.ToUInt32(entry, 8);
                if (streamType == 6) { exStreamRva = rva; exStreamSize = dataSize; }
                else if (streamType == 4) { modStreamRva = rva; modStreamSize = dataSize; }
                else if (streamType == 7) { sysInfoRva = rva; }
                else if (streamType == 3) { threadListRva = rva; threadListSize = dataSize; }
            }

            // ── Module list ──
            List<ulong> modBase = new List<ulong>();
            List<uint> modSize = new List<uint>();
            List<string> modName = new List<string>();
            if (modStreamRva != 0) {
                fs.Seek(modStreamRva, SeekOrigin.Begin);
                byte[] cbuf = new byte[4];
                fs.Read(cbuf, 0, 4);
                uint moduleCount = BitConverter.ToUInt32(cbuf, 0);
                Console.WriteLine("Module count: " + moduleCount);
                byte[] modBuf = new byte[108];
                for (uint m = 0; m < moduleCount; m++) {
                    fs.Read(modBuf, 0, 108);
                    ulong baseOfImage = BitConverter.ToUInt64(modBuf, 0);
                    uint sizeOfImage = BitConverter.ToUInt32(modBuf, 8);
                    uint nameRva = BitConverter.ToUInt32(modBuf, 20);
                    modBase.Add(baseOfImage);
                    modSize.Add(sizeOfImage);
                    modName.Add(ReadMiniDumpString(fs, nameRva));
                }
            }

            // ── Exception ──
            Console.WriteLine("");
            Console.WriteLine("=== EXCEPTION ===");
            if (exStreamRva != 0) {
                byte[] exBuf = new byte[exStreamSize];
                fs.Seek(exStreamRva, SeekOrigin.Begin);
                fs.Read(exBuf, 0, exBuf.Length);
                uint threadId = BitConverter.ToUInt32(exBuf, 0);
                // ExceptionRecord starts at offset 8 (after ThreadId+alignment)
                uint exCode = BitConverter.ToUInt32(exBuf, 8);
                uint exFlags = BitConverter.ToUInt32(exBuf, 12);
                ulong exAddr = BitConverter.ToUInt64(exBuf, 24);
                uint numParams = BitConverter.ToUInt32(exBuf, 32);

                Console.WriteLine("ThreadId: " + threadId);
                Console.WriteLine("ExceptionCode: 0x" + exCode.ToString("X8"));
                Console.WriteLine("ExceptionFlags: 0x" + exFlags.ToString("X8"));
                Console.WriteLine("ExceptionAddress: 0x" + exAddr.ToString("X16"));
                Console.WriteLine("NumberParameters: " + numParams);

                if (exCode == 0xC0000005 && numParams >= 2) {
                    // ExceptionInformation[] starts at offset 40
                    ulong accessType = BitConverter.ToUInt64(exBuf, 40);
                    ulong accessAddr = BitConverter.ToUInt64(exBuf, 48);
                    string accessStr = accessType == 0 ? "READ" : (accessType == 1 ? "WRITE" : (accessType == 8 ? "DEP/EXECUTE" : accessType.ToString()));
                    Console.WriteLine("Access violation: " + accessStr + " at 0x" + accessAddr.ToString("X16"));
                }

                // find faulting module
                bool found = false;
                for (int m = 0; m < modBase.Count; m++) {
                    ulong b = modBase[m];
                    ulong e = b + modSize[m];
                    if (exAddr >= b && exAddr < e) {
                        Console.WriteLine("FAULTING MODULE: " + modName[m] + "  base=0x" + b.ToString("X16") + " offset=0x" + (exAddr - b).ToString("X"));
                        found = true;
                    }
                }
                if (!found) Console.WriteLine("FAULTING MODULE: <not found in module list - address may be JIT/heap/stack>");
            } else {
                Console.WriteLine("No exception stream present.");
            }

            Console.WriteLine("");
            Console.WriteLine("=== coclassic / ImConquer modules ===");
            for (int m = 0; m < modName.Count; m++) {
                if (modName[m].IndexOf("coclassic", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    modName[m].IndexOf("ImConquer", StringComparison.OrdinalIgnoreCase) >= 0) {
                    Console.WriteLine(modName[m] + "  base=0x" + modBase[m].ToString("X16") + " size=0x" + modSize[m].ToString("X"));
                }
            }
        }
    }
}
"@

[MiniDumpParser]::Parse($Path)
