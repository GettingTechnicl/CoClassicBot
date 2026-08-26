param(
    [Parameter(Mandatory=$true)][string]$Path
)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;

public class StreamDumper {
    public static void Dump(string path) {
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            byte[] hbuf = new byte[32];
            fs.Read(hbuf, 0, 32);
            uint signature = BitConverter.ToUInt32(hbuf, 0);
            uint version = BitConverter.ToUInt32(hbuf, 4);
            uint numStreams = BitConverter.ToUInt32(hbuf, 8);
            uint streamDirRva = BitConverter.ToUInt32(hbuf, 12);

            Console.WriteLine("Signature: 0x" + signature.ToString("X8"));
            Console.WriteLine("Version raw: 0x" + version.ToString("X8"));
            Console.WriteLine("NumberOfStreams: " + numStreams);
            Console.WriteLine("StreamDirectoryRva: 0x" + streamDirRva.ToString("X"));
            Console.WriteLine("");

            fs.Seek(streamDirRva, SeekOrigin.Begin);
            byte[] entry = new byte[12];
            for (uint i = 0; i < numStreams; i++) {
                fs.Read(entry, 0, 12);
                uint streamType = BitConverter.ToUInt32(entry, 0);
                uint dataSize = BitConverter.ToUInt32(entry, 4);
                uint rva = BitConverter.ToUInt32(entry, 8);
                Console.WriteLine("Stream " + i + ": Type=" + streamType + " DataSize=" + dataSize + " Rva=0x" + rva.ToString("X"));
            }
        }
    }
}
"@

[StreamDumper]::Dump($Path)
