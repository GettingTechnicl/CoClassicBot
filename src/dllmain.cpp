#include <windows.h>
#include <cstdio>
#include "overlay.h"
#include "discord.h"
#include "hwid_spoof.h"
#include "hooks.h"
#include "packets.h"
#include "game.h"
#include "config.h"
#include "plugin_mgr.h"
#include "itemtype.h"
#include "spawn_memory.h"
#include "log.h"
#include <string>

ULONG64 g_qwModuleBase = 0;
HMODULE g_hModule = nullptr;

namespace {

// launcher.exe writes this file (see injector/main.cpp's supervision loop)
// immediately after relaunching this specific process following an
// UNEXPECTED game exit — never on a normal fresh launch. Its mere presence,
// checked once right here, is the only signal distinguishing "this is a
// crash-recovery resume" from "this is an intentional new session" — see
// IPlugin::ResumeEnabledStateFromSettings.
//
// Keyed by this process's own PID (multi-account manager support): with
// more than one game process able to run concurrently, a single fixed
// filename would race between accounts — whichever process's DLL happened
// to check first would consume the marker meant for a different one. The
// launcher writes resume_hunt_<pid>.flag using the exact PID it just
// launched (from CreateProcessA's own PROCESS_INFORMATION), so reading it
// back via this process's own GetCurrentProcessId() is symmetric with no
// extra coordination needed.
std::string ResumeHuntMarkerPath()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(g_hModule, buf, MAX_PATH);
    std::string path = buf;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos + 1);
    path += "resume_hunt_" + std::to_string(GetCurrentProcessId()) + ".flag";
    return path;
}

// One-shot: deletes the marker so a subsequent NORMAL close+reopen doesn't
// also resume automatically. Returns whether it was present.
bool ConsumeResumeHuntMarker()
{
    const std::string path = ResumeHuntMarkerPath();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
        return false;
    fclose(f);
    remove(path.c_str());
    return true;
}

// Multi-account manager support: the launcher's own login-window-closed
// heuristic (see injector/auto_login.cpp) is a weak signal — the login
// window can stay technically alive (hidden/reparented) well after a real
// login completes, which left the manager UI's status stuck on "Logging
// in..." indefinitely even though the character was already in-game. This
// writes the SAME kind of PID-keyed marker file resume_hunt uses, but in
// the other direction: the DLL telling the launcher "the hero pointer is
// now valid" the instant it actually knows that (right where InitThread
// already polls for it below) — ground truth, not a guess based on window
// lifetime. The launcher polls for this file's existence/deletes it; see
// RunAccountSupervisionLoop's use of it in injector/main.cpp.
std::string LoggedInMarkerPath()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(g_hModule, buf, MAX_PATH);
    std::string path = buf;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos + 1);
    path += "logged_in_" + std::to_string(GetCurrentProcessId()) + ".flag";
    return path;
}

void WriteLoggedInMarker()
{
    FILE* f = nullptr;
    if (fopen_s(&f, LoggedInMarkerPath().c_str(), "w") == 0 && f)
        fclose(f);
}

}  // namespace

static DWORD WINAPI InitThread(LPVOID)
{
    // The game's login process sends an integrity/environment check to the server.
    // If we patch code or create D3D devices before login completes, the server
    // rejects the connection with "virtual machine detected!".
    //
    // Strategy: sit quietly until the hero pointer becomes valid (= logged in),
    // then do all initialization.

    // Multi-account manager support: this used to AllocConsole() here for a
    // visible bot-log window per game process -- with several accounts
    // running at once that meant one extra black window per account, on
    // top of the launcher's own (now also removed, see injector/main.cpp's
    // WinMain). Log::Init() already writes everything to coclassic.log
    // (a rotating file sink, independent of any console) regardless of
    // whether a console exists, so dropping this loses no diagnostics --
    // spdlog's console sink just silently no-ops against a detached stdout.
    Log::Init();
    spdlog::info("[init] Logger ready");

    // Install HWID spoof hooks early — before the game collects hardware
    // identifiers for the login packet.  These hook Windows system DLLs
    // (kernel32, advapi32, iphlpapi), NOT game code, so they don't trigger
    // the server's VM/integrity check.
    HwidSpoof::Init(g_hModule);

    Game::Init();   // benign - just reads the module base address

    // Start the entity scan worker explicitly. Get()/GetStats() also start it
    // lazily, but doing it here means the first list is ready before anything
    // asks — and avoids the failure mode where the only caller was GetStats()
    // (overlay status line) and the thread therefore never started at all.
    Entities::Start();

    // Poll for login completion - hero pointer exists early, but UID
    // is only assigned once the server confirms the login.
    spdlog::info("[init] Waiting for login...");
    while (true) {
        CHero* hero = Game::GetHero();
        if (hero && hero->GetID() > 0)
            break;
        Sleep(500);
    }
    WriteLoggedInMarker();
    spdlog::info("[init] Wrote login-confirmed marker: {}", LoggedInMarkerPath());

    // Player is logged in - safe to initialize everything now.
    spdlog::info("[init] DLL attached (post-login init)");
    spdlog::info("[init] Game base: 0x{:X}", (uintptr_t)Game::Base());

    CHero* hero = Game::GetHero();
    if (hero) {
        spdlog::info("[init] Hero: {} (ID={})", hero->GetName(), hero->GetID());
    }

    LoadConfig();
    LoadItemTypes();
    // Spawn memory persists across sessions so the bot does not relearn a
    // hunting ground it already knows. Decay keeps it current; see
    // spawn_memory.h for why this cannot grow without bound.
    SpawnMemory::Load();
    InitHooks();
    SetWhisperCallback([](const std::string& sender, const std::string& message) {
        const MiscSettings& misc = GetMiscSettings();
        if (!misc.whisperNotifyEnabled)
            return;
        CHero* hero = Game::GetHero();
        char buf[512];
        snprintf(buf, sizeof(buf), "[%s] Whisper from %s: %s",
                 hero ? hero->GetName() : "?", sender.c_str(), message.c_str());
        SendDiscordNotification(buf);
    });
    InitPacketHook();

    // Small extra delay ensures the game's D3D10 swapchain is stable
    Sleep(1000);
    InitOverlay();
    PluginManager::Get().Init();

    if (ConsumeResumeHuntMarker()) {
        spdlog::info("[init] Crash-recovery relaunch detected — resuming previously-enabled plugins");
        for (const auto& plugin : PluginManager::Get().GetPlugins())
            plugin->ResumeEnabledStateFromSettings();
    }

    spdlog::info("[init] All systems initialized");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        spdlog::info("[shutdown] DLL detaching");
        PluginManager::Get().Shutdown();
        SaveConfig();
        ShutdownOverlay();
        CleanupPacketHook();
        CleanupHooks();
        HwidSpoof::Shutdown();
        spdlog::info("[shutdown] Cleanup complete");
        Log::Shutdown();
        break;
    }

    return TRUE;
}
