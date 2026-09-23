#pragma once
// =====================================================================
// build_fence.h — refuse to arm the bot against a client build it has not been verified on.
//
// WHY: every RVA in game.h and (to a lesser degree) every struct offset in CHero.h/CRole.h/... is
// specific to one ImConquer.exe build. The last client update moved every function RVA
// non-uniformly and shifted data fields, and calling a stale RVA jumps into garbage and crashes
// (or worse, corrupts) the client. This header identifies the build from the PE header —
// TimeDateStamp + SizeOfImage — WITHOUT touching any game code, and answers "is this a build we
// have verified?". It is used in two places:
//   * injector/main.cpp   reads the exe FILE on disk before launching/injecting anything;
//   * src/dllmain.cpp     reads the host process's own image header before initialising anything.
//
// HOW TO ADD A BUILD: only after the new build's RVAs/offsets have been re-derived AND live-verified
// (tools/sigscan.py proposes, the self-test / paired dumps confirm), append its stamp/size to
// kSupported below AND give it its own compiled DLL target in CMakeLists.txt (see field_offsets.h's
// header comment for why one binary can't serve two builds). Do NOT add a build just to make the
// fence go away.
//
// TWO SEPARATE CHECKS, deliberately not the same function:
//   IsSupported(stamp, size)      — "do we have a DLL for this build at all?" Checks the FULL list
//                                   below. Used by injector/main.cpp (compiled ONCE, un-varianted)
//                                   to decide whether to launch/inject at all, and which DLL to pick
//                                   (FindSupported(...)->dllName).
//   IsSelfBuild(stamp, size)      — "is this build the ONE I was compiled for?" Checks ONLY
//                                   kSelfBuild, which is selected by the SAME COCLASSIC_TARGET_V1078
//                                   macro that picks this DLL's struct offsets / native RVAs
//                                   (field_offsets.h, game.h). Used by dllmain.cpp's own runtime
//                                   self-check. This is what makes it physically impossible for a
//                                   v1078-targeted DLL to fire a v1074 offset/RVA or vice versa even
//                                   if injected into the wrong process: IsSupported alone would wrongly
//                                   say "yes" for EITHER build once both are listed, so the DLL must
//                                   check against its own single compiled-for entry, not the full list.
// =====================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace BuildFence {

struct Build {
    uint32_t peTimeDateStamp;
    uint32_t sizeOfImage;
    const char* label;
    const char* dllName;   // the DLL file the launcher should inject for this build
};

// v1074: ImConquer.exe SHA-256 C2B53437EF68D687A1EF0F70C74BCF2DF6027BF82B558E93330C839EB5E1C396
// (docs/investigation/CLIENT_V1074_BASELINE.md).
// v1078: struct fields value-confirmed against character S411
// (docs/investigation/V1078_OFFSET_FINDINGS.md); the action-critical native-call RVAs
// (CNETCLIENT_CONNECTION_SINGLETON/SEND_MSG_REAL/CROLE_SET_COMMAND_REAL) are only
// SIGNATURE-relocated, not yet live-tested — see game.h GameRva::SEND_MSG_TESTED / SET_COMMAND_TESTED,
// which independently gates SendPacket()/SetCommand() on this build until a real
// pickup/movement/attack test passes. Listing v1078 here arms READS only.
inline constexpr Build kSupported[] = {
    { 0x6A51CFB9u, 0x28C5000u, "v1074", "coclassic.dll" },
    { 0x6AB0822Bu, 0x2A26000u, "v1078", "coclassic_v1078.dll" },
};

// The ONE build this specific compiled DLL is for — see the header comment above. Selected by the
// same macro as field_offsets.h/game.h, so it is always in lockstep with the offsets actually baked
// into this binary. Not used by injector/main.cpp (which is compiled once, for neither build).
#if defined(COCLASSIC_TARGET_V1078)
inline constexpr Build kSelfBuild = kSupported[1];
#else
inline constexpr Build kSelfBuild = kSupported[0];
#endif

inline const Build* FindSupported(uint32_t stamp, uint32_t sizeOfImage)
{
    for (const Build& b : kSupported)
        if (b.peTimeDateStamp == stamp && b.sizeOfImage == sizeOfImage)
            return &b;
    return nullptr;
}

inline bool IsSupported(uint32_t stamp, uint32_t sizeOfImage)
{
    return FindSupported(stamp, sizeOfImage) != nullptr;
}

inline bool IsSelfBuild(uint32_t stamp, uint32_t sizeOfImage)
{
    return stamp == kSelfBuild.peTimeDateStamp && sizeOfImage == kSelfBuild.sizeOfImage;
}

// Parse TimeDateStamp / SizeOfImage from the first bytes of a PE image (file or loaded module).
inline bool ParsePeIdentity(const uint8_t* hdr, size_t n, uint32_t* stamp, uint32_t* sizeOfImage)
{
    if (n < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z')
        return false;
    int32_t e = 0;
    memcpy(&e, hdr + 0x3C, 4);
    if (e <= 0 || static_cast<size_t>(e) + 24 + 60 > n)
        return false;
    if (memcmp(hdr + e, "PE\0\0", 4) != 0)
        return false;
    memcpy(stamp, hdr + e + 8, 4);
    memcpy(sizeOfImage, hdr + e + 24 + 56, 4);
    return true;
}

// Read the identity of an exe on disk (before the process exists).
inline bool ReadPeIdentityFromFile(const char* path, uint32_t* stamp, uint32_t* sizeOfImage)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;
    uint8_t buf[0x400] = {};
    const size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return ParsePeIdentity(buf, got, stamp, sizeOfImage);
}

}  // namespace BuildFence
