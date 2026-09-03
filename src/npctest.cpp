// npctest.dll v2 — inject mid-session (System Informer) alongside coclassic.dll.
//
// v1 added the Frida script's RVAs to the GAME's base and crashed: those RVAs
// were symbols in the BOT's OWN DLL (the Frida script does
// Process.getModuleByName('coclassic.dll')). So Frida never called a native
// client function — it called the bot's CHero::ActivateNpc (hand-built packet)
// and then the answers, blindly. This test does exactly that first step, from
// wherever the hero stands, and watches the two fields the bot's gate reads:
//     CHero+0x1050 m_bNpcActive     CHero+0x3774 m_idActiveNpc
// If they flip after the bot's own packet, the gate works when in range and the
// 00:37 failure was distance. If they stay 0, the client only sets them in its
// own click path and the gate can never confirm a packet-opened dialog.
//
// Config C:/Users/Public/npctest_cfg.txt: line1 = npc id; line2 "answer" also
// sends the bot's AnswerNpcEx(0,101) twice, 1.2 s apart (full Frida sequence).
// Log:    C:/Users/Public/npctest.log
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// RVAs inside coclassic.dll, build 2026-09-03 06:51 (tools/resolve_symbol_addrs.ps1)
static const uintptr_t RVA_GET_SINGLETON = 0x3F230;   // CHero::GetSingletonPtr
static const uintptr_t RVA_ACTIVATE_NPC  = 0x3DB40;   // CHero::ActivateNpc(this, id)
static const uintptr_t RVA_ANSWER_NPC_EX = 0x3DC80;   // CHero::AnswerNpcEx(this, answer, taskId)
static const uintptr_t OFF_NPC_ACTIVE    = 0x1050;
static const uintptr_t OFF_ACTIVE_NPC_ID = 0x3774;
static const uintptr_t OFF_POS_X         = 0xD8;

using GetSingletonFn = void*(*)();
using ActivateNpcFn  = void(*)(void*, uint32_t);
using AnswerNpcExFn  = void(*)(void*, int, int);

static bool Read32(uintptr_t a, uint32_t* out) {
    __try { *out = *(volatile uint32_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Read64(uintptr_t a, uint64_t* out) {
    __try { *out = *(volatile uint64_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The 22:06 closed->open differential dump showed, besides +0x1050, three
// neighbouring fields go non-zero when a dialog opens: +0x1060 (pointer-
// sized, 0 -> 0x6CFB000-ish — looks like the dialog window object), +0x1068
// (0 -> 8) and +0x1078 (0 -> 5). +0x1050 turned out to be sticky across a
// close, so these are the candidates for a "dialog is open RIGHT NOW"
// signal that a packet-opened dialog would also set.
static void Log(FILE* f, const char* tag, uintptr_t hero) {
    uint32_t act = 0xFFFFFFFF, id = 0xFFFFFFFF, x = 0, y = 0, f68 = 0, f78 = 0;
    uint64_t p60 = 0;
    Read32(hero + OFF_NPC_ACTIVE, &act); Read32(hero + OFF_ACTIVE_NPC_ID, &id);
    Read32(hero + OFF_POS_X, &x); Read32(hero + OFF_POS_X + 4, &y);
    Read64(hero + 0x1060, &p60); Read32(hero + 0x1068, &f68); Read32(hero + 0x1078, &f78);
    fprintf(f, "[%lu] %-11s hero=(%u,%u)  +0x1050=%u  +0x3774=%u  +0x1060=%p  +0x1068=%u  +0x1078=%u\n",
            GetTickCount(), tag, x, y, act, id, (void*)p60, f68, f78);
    fflush(f);
}
static void* CallSingleton(GetSingletonFn fn) {
    __try { return fn(); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static bool CallActivate(ActivateNpcFn fn, void* h, uint32_t id) {
    __try { fn(h, id); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool CallAnswer(AnswerNpcExFn fn, void* h, int a, int t) {
    __try { fn(h, a, t); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static DWORD WINAPI Run(LPVOID mod) {
    FILE* f = nullptr;
    fopen_s(&f, "C:/Users/Public/npctest.log", "a");
    if (!f) FreeLibraryAndExitThread((HMODULE)mod, 1);

    uint32_t npcId = 101395; bool answer = false;
    FILE* cf = nullptr;
    if (fopen_s(&cf, "C:/Users/Public/npctest_cfg.txt", "r") == 0 && cf) {
        char line[64] = {};
        if (fgets(line, sizeof(line), cf)) npcId = (uint32_t)strtoul(line, nullptr, 10);
        if (fgets(line, sizeof(line), cf) && strncmp(line, "answer", 6) == 0) answer = true;
        fclose(cf);
    }

    HMODULE bot = GetModuleHandleA("coclassic.dll");
    fprintf(f, "\n=== npctest v2: coclassic.dll base=%p npcId=%u answer=%d ===\n", (void*)bot, npcId, (int)answer);
    if (!bot) { fprintf(f, "coclassic.dll not loaded - abort\n"); fclose(f); FreeLibraryAndExitThread((HMODULE)mod, 2); }
    const uintptr_t base = (uintptr_t)bot;
    void* hero = CallSingleton((GetSingletonFn)(base + RVA_GET_SINGLETON));
    fprintf(f, "hero=%p\n", hero);
    if (!hero) { fprintf(f, "GetSingletonPtr null/threw - abort\n"); fclose(f); FreeLibraryAndExitThread((HMODULE)mod, 3); }

    Log(f, "before", (uintptr_t)hero);
    fprintf(f, "bot ActivateNpc(%u): %s\n", npcId,
            CallActivate((ActivateNpcFn)(base + RVA_ACTIVATE_NPC), hero, npcId) ? "sent" : "THREW");
    fflush(f);
    Sleep(300);  Log(f, "+300ms",  (uintptr_t)hero);
    Sleep(1200); Log(f, "+1500ms", (uintptr_t)hero);
    Sleep(1500); Log(f, "+3000ms", (uintptr_t)hero);

    if (answer) {
        for (int i = 0; i < 2; ++i) {
            fprintf(f, "bot AnswerNpcEx(0,101) #%d: %s\n", i + 1,
                    CallAnswer((AnswerNpcExFn)(base + RVA_ANSWER_NPC_EX), hero, 0, 101) ? "sent" : "THREW");
            fflush(f);
            Sleep(1200); Log(f, i == 0 ? "after ans1" : "after ans2", (uintptr_t)hero);
        }
    }
    fprintf(f, "=== done ===\n"); fclose(f);
    FreeLibraryAndExitThread((HMODULE)mod, 0);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Run, h, 0, nullptr); }
    return TRUE;
}
