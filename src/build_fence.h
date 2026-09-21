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
// (tools/sigscan.py proposes, the self-test / paired dumps confirm), append its stamp/size here.
// Do NOT add a build just to make the fence go away.
// =====================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace BuildFence {

struct Build {
    uint32_t peTimeDateStamp;
    uint32_t sizeOfImage;
    const char* label;
};

// Verified builds. v1074: ImConquer.exe SHA-256 C2B53437EF68D687A1EF0F70C74BCF2DF6027BF82B558E93330C839EB5E1C396
// (docs/investigation/CLIENT_V1074_BASELINE.md). v1078 (stamp 0x6AB0822B, image 0x2A26000) is NOT listed
// on purpose: nothing has been re-derived for it yet.
inline constexpr Build kSupported[] = {
    { 0x6A51CFB9u, 0x28C5000u, "v1074" },
};

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
