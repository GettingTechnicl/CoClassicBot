#include "overlay.h"
#include "discord.h"
#include "game.h"
#include "map_items.h"
#include "hooks.h"
#include "itemtype.h"
#include "packets.h"
#include "config.h"
#include "map_probe.h"
#include "monster_scan.h"
#include "mapdata.h"
#include "spawn_memory.h"
#include "mem_stats.h"
#include "pathfinder.h"
#include "plugin_mgr.h"
#include "gateway.h"
#include "itemtype.h"
#include "CEntitySet.h"
#include "CEntityInfo.h"
#include "plugins/base_hunt_plugin.h"
#include "plugins/melee_hunt_plugin.h"
#include "plugins/archer_hunt_plugin.h"
#include "hunt_settings.h"
#include "hunt_targeting.h"
#include "hunt_intervals.h"
#include "inventory_utils.h"
#include "plugins/travel_plugin.h"
#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_win32.h"

#include <detours.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =====================================================================
// Draw isometric diamond tiles on the minimap
// =====================================================================
static void DrawMapCells(ImDrawList* dl, CellInfo* cells, int mapW, int mapH,
                         float camX, float camY, int viewRadius, float fs,
                         float centerX, float centerY,
                         float clipL, float clipT, float clipR, float clipB)
{
    int camIX = (int)camX;
    int camIY = (int)camY;
    for (int tdy = -viewRadius; tdy <= viewRadius; tdy++) {
        int tileY = camIY + tdy;
        if (tileY < 0 || tileY >= mapH) continue;
        for (int tdx = -viewRadius; tdx <= viewRadius; tdx++) {
            int tileX = camIX + tdx;
            if (tileX < 0 || tileX >= mapW) continue;

            float cx = centerX + ((float)tileX - camX - ((float)tileY - camY)) * fs;
            float cy = centerY + ((float)tileX - camX + ((float)tileY - camY)) * fs;

            if (cx + fs < clipL || cx - fs > clipR) continue;
            if (cy + fs < clipT || cy - fs > clipB) continue;

            CellInfo& cell = cells[tileX + tileY * mapW];
            uint16_t mask = CGameMap::GetMask(&cell);
            ImU32 col;
            if (mask == 1)
                col = IM_COL32(40, 40, 40, 255);       // blocked
            else if (mask != 0)
                col = IM_COL32(180, 60, 220, 255);      // portal / special
            else
                col = IM_COL32(60, 100, 60, 255);       // walkable

            dl->AddQuadFilled(
                ImVec2(cx, cy - fs),
                ImVec2(cx + fs, cy),
                ImVec2(cx, cy + fs),
                ImVec2(cx - fs, cy),
                col);
        }
    }
}

// =====================================================================
// State
// =====================================================================
static bool                    g_showOverlay   = true;
static bool                    g_initialized   = false;

// ── Map camera state ──
static float g_mapCamX = 0.0f;   // camera center (tile coords)
static float g_mapCamY = 0.0f;
static bool  g_mapDragging = false;
static float g_mapDragStartMouseX = 0.0f;
static float g_mapDragStartMouseY = 0.0f;
static float g_mapDragStartCamX = 0.0f;
static float g_mapDragStartCamY = 0.0f;

// Entity filter (shared between minimap + entity table)
// 0=All, 1=NPCs, 2=Players, 3=Monsters, 4=Items
static int g_entityFilter = 0;

// Game window + WndProc
static HWND                    g_hGameWnd      = nullptr;
static WNDPROC                 g_origWndProc   = nullptr;

// D3D10 device (the game's actual device)
static ID3D10Device*           g_pDevice       = nullptr;

// Present hook
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn OrigPresent = nullptr;

// =====================================================================
// WndProc subclass on GAME window — forward input to ImGui
// =====================================================================
static LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Toggle with Insert
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_showOverlay = !g_showOverlay;
        spdlog::debug("[overlay] {}", g_showOverlay ? "shown" : "hidden");
        return 0;
    }

    // Feed input to ImGui when visible
    if (g_showOverlay) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
            return 0;
        if (io.WantCaptureKeyboard && (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
            return 0;
    }

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// =====================================================================
// Find IDXGISwapChain::Present address via dummy device
// =====================================================================
static uintptr_t FindPresentAddress()
{
    // Register temp window class
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "coclassic_dummy";
    RegisterClassExA(&wc);

    // Create 1x1 hidden window
    HWND hDummy = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hDummy) {
        spdlog::error("[overlay] CreateWindowEx for dummy failed");
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    // Create dummy D3D11 device + swapchain (just to find Present vtable address)
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1;
    sd.BufferDesc.Height = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hDummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dummyDevice = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &dummySwapChain, &dummyDevice, &fl, &dummyCtx);

    if (FAILED(hr)) {
        spdlog::error("[overlay] Dummy D3D11CreateDeviceAndSwapChain: 0x{:08X}", (unsigned long)hr);
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    // Read vtable: Present is at index 8
    void** vtable = *(void***)dummySwapChain;
    uintptr_t presentAddr = (uintptr_t)vtable[8];
    spdlog::info("[overlay] Present address: 0x{:X}", presentAddr);

    // Cleanup dummy resources
    dummyCtx->Release();
    dummySwapChain->Release();
    dummyDevice->Release();
    DestroyWindow(hDummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return presentAddr;
}

// =====================================================================
// Present hook — renders ImGui into the game's back buffer
// =====================================================================
// Session 13 [HkPresent split, phase 1]: the two lowest-risk extractions —
// zero dependency on anything else in HkPresent, not even `hero` from the
// tab-bar scope below. See the approved plan (Split HkPresent() into smaller
// functions) for the full phased breakdown; this is phase 1 of 4.

// Runs every frame regardless of overlay visibility.
static void TickBackgroundLogic()
{
    CHero* hero = Game::GetHero();
    if (hero) {
        UpdateCharacterConfigBinding();
        Pathfinder::Get().Update();
        PluginManager::Get().UpdateAll();
        UpdateItemNotifications();
        MaybeAutoSaveConfig();
        // Piggyback on the config autosave cadence rather than adding a
        // second timer — same rhythm, no extra frame cost.
        MaybeAutoSaveSpawnMemory();
    }
    // Session 10: memory-leak tracking — runs regardless of hero/login
    // state so the process's baseline footprint is visible too, not
    // just once in-world.
    MaybeLogMemoryStats();
}

// Runs only when the overlay is visible, before any tab renders.
static void TickOverlayRecording()
{
    // Session 10: accumulate the walk trace every frame. Cheap (a dedup'd
    // tile append), and it is what makes the cell-grid scan decisive —
    // see map_probe.h.
    MapProbe_RecordHeroTile();

    // Route recording samples the hero's tile each frame while armed;
    // it dedupes standing still, so only real movement is captured.
    if (RouteRecordIsActive()) {
        if (CHero* rh = Game::GetHero())
            RouteRecordSample(rh->m_posMap);
    }
}

// Session 13 [HkPresent split, phase 2]: fully self-contained tab bodies —
// only reads `hero` from outer scope. The BeginTabItem/EndTabItem call
// sites stay in HkPresent itself (ImGui's ID stack is keyed by call order,
// not C++ function boundaries, so this is invisible to widget state as
// long as those two calls don't move).
static void RenderPlayerTab(CHero* hero)
{
    constexpr ImGuiTreeNodeFlags kPlayerSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    const uint32_t silver = hero->GetSilver();
    const uint64_t silverRuntime = hero->GetSilverRuntimeValue();
    if (ImGui::CollapsingHeader("Overview", kPlayerSectionFlags)) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Player: %s", hero->GetName());
    ImGui::Text("UID:      %u", hero->GetID());
    ImGui::Text("Position: (%d, %d)", hero->m_posMap.x, hero->m_posMap.y);
    ImGui::Text("World:    (%d, %d)", hero->m_posWorld.x, hero->m_posWorld.y);
    ImGui::Text("Screen:   (%d, %d)", hero->m_posScr.x, hero->m_posScr.y);
    ImGui::Text("Status:   0x%llX", (unsigned long long)hero->m_nStatusFlag);
    ImGui::Text("Silver:   %u", silver);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", hero->HasTrustedSilverCache() ? "(server update)" : "(cached fallback)");
    ImGui::Text("A30 Raw:  %llu", (unsigned long long)silverRuntime);

    if (hero->HasSyndicate()) {
        auto* entSet = CEntitySet::GetInstance();
        const char* guildName = entSet ? entSet->GetSyndicateName(hero->m_idSyndicate) : nullptr;
        const char* rankName = GetSyndicateRankName(hero->m_nSyndicateRank);
        ImGui::Text("Guild:   ");
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", guildName ? guildName : "Unknown");
        if (rankName[0]) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%s)", rankName);
        }
    }

    if (hero->IsDead())
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "** DEAD **");

    // Session 9: entity source is the heap scan — see entities.h.
    {
        const Entities::Stats st = Entities::GetStats();
        ImGui::Text("Nearby:   %d players, %d monsters, %d NPCs",
                    st.players, st.monsters, st.npcs);
        ImGui::TextDisabled("entity scan #%u: %d found, %u ms",
                            st.scans, st.total, st.lastScanMs);
        if (st.total == 0) {
            // Funnel readout: shows which predicate rejected
            // everything rather than just "0 found".
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1),
                "  regions=%u addrs=%llu vt=%u id=%u pos=%u name=%u",
                st.regions, (unsigned long long)st.addrs,
                st.passVtable, st.passId, st.passPos, st.passName);
        }
    }

    // ── Equipment (collapsible) ──
    }
    if (ImGui::CollapsingHeader("Equipment")) {
        if (ImGui::BeginTable("##equip", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable,
                ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Slot",    ImGuiTableColumnFlags_WidthFixed, 65.0f);
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("+",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
            ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Sockets", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (int s = 0; s < EquipSlot::COUNT; s++) {
                CItem* eq = hero->GetEquip(s);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", GetEquipSlotName(s));
                ImGui::TableNextColumn();
                if (eq)
                    ImGui::Text("%s", eq->GetName());
                else
                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "-");
                ImGui::TableNextColumn();
                if (eq) {
                    int q = eq->GetQuality();
                    ImVec4 qc = (q >= ItemQuality::SUPER) ? ImVec4(1,0.8f,0,1) :
                                (q >= ItemQuality::ELITE) ? ImVec4(0.6f,0.4f,1,1) :
                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                ImVec4(1,1,1,1);
                    ImGui::TextColored(qc, "%s", eq->GetQualityName());
                }
                ImGui::TableNextColumn();
                if (eq && eq->GetPlus() > 0)
                    ImGui::Text("+%d", eq->GetPlus());
                ImGui::TableNextColumn();
                if (eq)
                    ImGui::Text("%d/%d", eq->GetDurability(), eq->GetMaxDurability());
                ImGui::TableNextColumn();
                if (eq) {
                    if (eq->HasSocket1() || eq->HasSocket2()) {
                        ImGui::Text("%s%s%s",
                            GetGemClassName(eq->GetGem1()),
                            eq->HasSocket2() ? ", " : "",
                            eq->HasSocket2() ? GetGemClassName(eq->GetGem2()) : "");
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    // ── Inventory (collapsible) ──
    if (ImGui::CollapsingHeader("Inventory")) {
        ImGui::Text("Items: %zu / %d",
                    hero->m_deqItem.size(), CHero::MAX_BAG_ITEMS);

        if (hero->m_deqItem.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Inventory is empty.");
        } else if (ImGui::BeginTable("##inv", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 200.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("+",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
            ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Sockets", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < hero->m_deqItem.size() && i < 40; i++) {
                auto& ref = hero->m_deqItem[i];
                if (!ref) continue;
                CItem* item = ref.get();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", i + 1);
                ImGui::TableNextColumn();
                ImGui::Text("%s", item->GetName());
                ImGui::TableNextColumn();
                {
                    int q = item->GetQuality();
                    ImVec4 qc = (q >= ItemQuality::SUPER) ? ImVec4(1,0.8f,0,1) :
                                (q >= ItemQuality::ELITE) ? ImVec4(0.6f,0.4f,1,1) :
                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                ImVec4(1,1,1,1);
                    ImGui::TextColored(qc, "%s", item->GetQualityName());
                }
                ImGui::TableNextColumn();
                if (item->GetPlus() > 0)
                    ImGui::Text("+%d", item->GetPlus());
                ImGui::TableNextColumn();
                ImGui::Text("%d/%d", item->GetDurability(), item->GetMaxDurability());
                ImGui::TableNextColumn();
                if (item->HasSocket1() || item->HasSocket2()) {
                    ImGui::Text("%s%s%s",
                        GetGemClassName(item->GetGem1()),
                        item->HasSocket2() ? ", " : "",
                        item->HasSocket2() ? GetGemClassName(item->GetGem2()) : "");
                }
            }
            ImGui::EndTable();
        }
    }

    // ── Skills (collapsible) ──
    if (ImGui::CollapsingHeader("Skills")) {
        if (hero->m_vecMagic.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No skills learned.");
        } else if (hero->m_vecMagic.size() < 200 &&
                   ImGui::BeginTable("##skills", 7,
                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                       ImVec2(0, 200.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
            // Session 9: the magic-type ID is what MagicAttack()
            // takes, so it needs to be visible, not just internal.
            ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed, 45.0f);
            ImGui::TableSetupColumn("Lv",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
            ImGui::TableSetupColumn("MP",       ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Stam",     ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Dist",     ImGuiTableColumnFlags_WidthFixed, 35.0f);
            ImGui::TableSetupColumn("Exp",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < hero->m_vecMagic.size(); i++) {
                auto& ref = hero->m_vecMagic[i];
                if (!ref) continue;
                CMagic* magic = ref.get();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (magic->IsXpSkill()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[XP]");
                    ImGui::SameLine();
                }
                ImGui::Text("%s", magic->GetName());
                ImGui::TableNextColumn();
                ImGui::Text("%u", magic->GetMagicType());
                ImGui::TableNextColumn();
                ImGui::Text("%u", magic->GetLevel());
                ImGui::TableNextColumn();
                if (magic->GetMpCost() > 0)
                    ImGui::Text("%u", magic->GetMpCost());
                ImGui::TableNextColumn();
                if (magic->GetStaminaCost() > 0)
                    ImGui::Text("%u", magic->GetStaminaCost());
                ImGui::TableNextColumn();
                if (magic->GetDistance() > 0)
                    ImGui::Text("%u", magic->GetDistance());
                ImGui::TableNextColumn();
                if (magic->GetExpRequired() > 0) {
                    float progress = (float)magic->GetExp() / (float)magic->GetExpRequired();
                    if (progress > 1.0f) progress = 1.0f;
                    char overlay[32];
                    snprintf(overlay, sizeof(overlay), "%u/%u",
                             magic->GetExp(), magic->GetExpRequired());
                    ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
                }
            }
            ImGui::EndTable();
        }
    }
}

// Fetches its own packet log — zero outer-scope dependency at all.
static void RenderPacketsTab()
{
    PacketLog& plog = GetPacketLog();
    constexpr ImGuiTreeNodeFlags kPacketSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Controls", kPacketSectionFlags)) {
    ImGui::Checkbox("Logging", &plog.enabled);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        plog.Clear();
    ImGui::SameLine();
    ImGui::Text("(%zu packets)", plog.Count());
    }

    if (ImGui::CollapsingHeader("Log", kPacketSectionFlags)) {
    if (plog.Count() == 0) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "No packets captured yet.");
    } else {
        if (ImGui::BeginChild("##pktscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                ImGuiWindowFlags_HorizontalScrollbar)) {
            for (size_t i = 0; i < plog.Count(); i++) {
                const PacketEntry& pkt = plog.Get(i);

                // Build full hex dump string (reused for expand & copy)
                std::string fullDump;
                for (size_t r = 0; r < pkt.data.size(); r += 16) {
                    char line[128] = {};
                    int p = snprintf(line, sizeof(line), "%04X  ", (unsigned)r);
                    for (size_t c = 0; c < 16; c++) {
                        if (r + c < pkt.data.size())
                            p += snprintf(line + p, sizeof(line) - p,
                                          "%02X ", pkt.data[r + c]);
                        else
                            p += snprintf(line + p, sizeof(line) - p, "   ");
                        if (c == 7)
                            p += snprintf(line + p, sizeof(line) - p, " ");
                    }
                    p += snprintf(line + p, sizeof(line) - p, " ");
                    for (size_t c = 0; c < 16 && r + c < pkt.data.size(); c++) {
                        uint8_t b = pkt.data[r + c];
                        line[p++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                    }
                    line[p] = '\0';
                    if (!fullDump.empty()) fullDump += '\n';
                    fullDump += line;
                }

                // Header label: click to expand, right-click to copy
                char label[128];
                snprintf(label, sizeof(label),
                         "[%zu] Type=0x%04X  Size=%u##pkt%zu",
                         i, pkt.msgType, pkt.rawSize, i);

                bool open = ImGui::TreeNode(label);

                // Right-click copies entire hex dump to clipboard
                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    ImGui::SetClipboardText(fullDump.c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Right-click to copy hex dump");
                }

                if (open) {
                    // Session 10: decoded field list — the packet
                    // family used here (MsgAction and others) is a
                    // tag/varint encoding, so raw hex alone means
                    // hand-parsing every field by eye. This is what
                    // makes reading off a captured packet's mode
                    // value (e.g. to find real walk's mode, vs.
                    // jump's confirmed 19) actually practical.
                    const auto fields = DecodeVarintFields(pkt.data.data(), pkt.data.size());
                    if (!fields.empty()) {
                        std::string decoded = "Fields: ";
                        for (size_t f = 0; f < fields.size(); ++f) {
                            if (f) decoded += "  ";
                            char fbuf[32];
                            snprintf(fbuf, sizeof(fbuf), "#%d=%u", fields[f].fieldNumber, fields[f].value);
                            decoded += fbuf;
                        }
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", decoded.c_str());
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(fullDump.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndChild();
    }
    }
}

// Session 13 [HkPresent split, phase 3]: the Misc tab's independent headers.
// Each is already its own self-contained { }-scoped block fetching its own
// settings reference — extracted verbatim into its own function, `hero`
// passed to every one (even where unused) for a consistent signature. The
// CollapsingHeader call itself moves WITH the body (unlike BeginTabItem/
// EndTabItem in phases 1-2) since each section is independently gated, not
// wrapping shared tab-level content.
static void RenderMiscDiscordWebhookSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Discord Webhook", kSectionFlags)) {
        DiscordSettings& discord = GetDiscordSettings();
        ImGui::Checkbox("Enable Discord Webhook", &discord.webhookEnabled);
        // Session 12: masked -- a webhook URL is a bearer credential
        // (anyone with it can post to the channel), same as a password.
        ImGui::InputText("Webhook URL##discord", discord.webhookUrl, IM_ARRAYSIZE(discord.webhookUrl),
            ImGuiInputTextFlags_Password);
        ImGui::InputText("Mention User ID##discord", discord.mentionUserId, IM_ARRAYSIZE(discord.mentionUserId));
        if (ImGui::Button("Test Webhook")) {
            if (discord.webhookUrl[0] != '\0') {
                CHero* heroTest = Game::GetHero();
                char msg[256];
                if (heroTest)
                    snprintf(msg, sizeof(msg), "[%s] Discord webhook test.", heroTest->GetName());
                else
                    snprintf(msg, sizeof(msg), "[coclassic] Discord webhook test.");
                std::string payload = BuildDiscordWebhookPayload(msg, discord.mentionUserId);
                SendDiscordWebhookPayloadAsync(discord.webhookUrl, std::move(payload));
            }
        }
        ImGui::TextDisabled("Shared webhook used by all notification features.");
    }
}

static void RenderMiscWhisperNotificationsSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Whisper Notifications", kSectionFlags)) {
        MiscSettings& misc = GetMiscSettings();
        ImGui::Checkbox("Notify on Whisper", &misc.whisperNotifyEnabled);
        ImGui::TextDisabled("Sends a Discord notification when another player whispers you.");
    }
}

static void RenderMiscLoggingDiagnosticsSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Logging & Diagnostics", kSectionFlags)) {
        MiscSettings& misc = GetMiscSettings();
        static const char* kLevelNames[] = {
            "Trace (very verbose)", "Debug", "Info", "Warning", "Error", "Critical", "Off"
        };
        int levelIdx = std::clamp(misc.logLevel, 0, 6);
        if (ImGui::Combo("Log Level", &levelIdx, kLevelNames, IM_ARRAYSIZE(kLevelNames))) {
            misc.logLevel = levelIdx;
            Log::SetLevel(levelIdx);
        }
        ImGui::TextDisabled("Trace generates tens of thousands of lines in a few minutes of "
            "play. Info or Warning is enough for normal use; turn Trace/Debug back on only "
            "when actively diagnosing something.");

        const std::string& logPath = Log::GetLogPath();
        ImGui::Text("Log file:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", logPath.empty() ? "(file logging unavailable)" : logPath.c_str());
        if (!logPath.empty() && ImGui::Button("Copy Log Path"))
            ImGui::SetClipboardText(logPath.c_str());

        ImGui::Separator();
        const MemStats mem = GetCurrentMemoryStats();
        if (mem.valid) {
            ImGui::Text("Working Set: %zu MB", mem.workingSetKb / 1024);
            ImGui::SameLine();
            ImGui::Text("| Private: %zu MB", mem.privateKb / 1024);
            ImGui::SameLine();
            ImGui::Text("| Peak: %zu MB", mem.peakWorkingSetKb / 1024);
        } else {
            ImGui::TextDisabled("Memory stats unavailable.");
        }
    }
}

static void RenderMiscNativePickupTestSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Native Pickup Test", kSectionFlags)) {
        static int testItemId = 0;
        static int testX = 0;
        static int testY = 0;
        ImGui::TextDisabled("Session 5: tests CHero::PickupItem's new native call path");
        ImGui::TextDisabled("(GameRva::CNETCLIENT_SEND_MAPITEM_MSG). SEH-guarded.");
        ImGui::InputInt("Item ID", &testItemId);
        ImGui::InputInt("X", &testX);
        ImGui::InputInt("Y", &testY);
        if (ImGui::Button("Test Native Pickup") && testItemId != 0) {
            CMapItem testItem = {};
            testItem.m_id = static_cast<OBJID>(testItemId);
            testItem.m_idType = 1000000; // Stancher, only used if plus lookup needed
            testItem.m_pos = Position(testX, testY);
            testItem.m_pInfo = nullptr; // GetPlus() safely returns 0 when null
            spdlog::info("[debug] Test Native Pickup: id={} x={} y={}", testItemId, testX, testY);
            DebugTestNativePickup(testItem);
        }
    }
}

static void RenderMiscNativeJumpTestSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Native Jump Test", kSectionFlags)) {
        static int jumpX = 0;
        static int jumpY = 0;
        // Session 9: defaults ON now. This used to drive the
        // CCommand/SetCommand prediction path (which crashed
        // the game repeatedly and defaulted OFF for safety);
        // it now drives CRole::SyncClientPosition(), which is
        // three plain data writes and live-verified safe.
        static bool jumpApplyPrediction = true;
        ImGui::TextDisabled("Sends the move packet, then syncs the client's own position");
        ImGui::TextDisabled("fields (start/dest/world). Live-verified; no SetCommand involved.");
        ImGui::InputInt("Dest X", &jumpX);
        ImGui::InputInt("Dest Y", &jumpY);
        ImGui::Checkbox("Sync client position after move", &jumpApplyPrediction);
        if (ImGui::Button("Test Native Jump")) {
            spdlog::info("[debug] Test Native Jump: x={} y={} predict={}", jumpX, jumpY, jumpApplyPrediction);
            DebugTestJump(jumpX, jumpY, jumpApplyPrediction);
        }
    }
}

static void RenderMiscNativeWalkTestSection(CHero* hero)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Native Walk Test", kSectionFlags)) {
        static int walkX = 0;
        static int walkY = 0;
        ImGui::TextDisabled("Sends the walk packet, then CRole::SetCommand(iType=15).");
        ImGui::TextDisabled("Use a destination 1-2 tiles from your current position.");
        ImGui::InputInt("Walk Dest X", &walkX);
        ImGui::InputInt("Walk Dest Y", &walkY);
        if (hero) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Use +1,+0##walk"))
                { walkX = hero->m_posMap.x + 1; walkY = hero->m_posMap.y; }
        }
        if (ImGui::Button("Test Native Walk")) {
            spdlog::info("[debug] Test Native Walk: x={} y={}", walkX, walkY);
            DebugTestWalk(walkX, walkY);
        }
    }
}

// Session 10: one-shot structural probe for the active
// CGameMap. Game::GetMap() is confirmed broken (its RVA
// dereferences to garbage); this pins down BOTH the access
// path and the field offsets before anything is changed.
static void RenderMiscMapProbeSection(CHero* hero)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Map Probe", kSectionFlags)) {
        static int expectedMapId = 1002;   // Twin City
        ImGui::TextDisabled("Read-only. Writes C:\\Users\\Public\\coclassic_mapprobe.json");
        ImGui::TextDisabled("Set the ID of the map you're standing in, then probe.");
        ImGui::InputInt("Current map ID", &expectedMapId);
        ImGui::TextDisabled("1000 Desert  1002 Twin City  1003 Mine  1004 Market  1020 Bird Is.");
        if (ImGui::Button("Run Map Probe")) {
            const bool ok = DebugProbeGameMap(expectedMapId);
            spdlog::info("[debug] Map probe run, expectedMapId={} ok={}", expectedMapId, ok);
        }

        ImGui::Separator();
        // Stage 2: the vtable objects are descriptors with no
        // cell data, so find the live grid by its dimensions.
        static int mapW = 543, mapH = 772;   // Twin City
        ImGui::TextDisabled("Stage 2: find the live cell grid by map dimensions.");
        ImGui::InputInt("Map width", &mapW);
        ImGui::InputInt("Map height", &mapH);
        if (ImGui::Button("Find Map Data")) {
            const bool ok = DebugProbeMapData(mapW, mapH);
            spdlog::info("[debug] Map data probe {}x{} ok={}", mapW, mapH, ok);
        }

        ImGui::SameLine();
        if (ImGui::Button("Auto-Find Grid")) {
            const bool ok = DebugAutoFindGrid(mapW, mapH);
            spdlog::info("[debug] Auto grid scan {}x{} ok={}", mapW, mapH, ok);
        }

        // With terrain coming from the .DMap files on disk, the
        // current map id is the only thing still needed from
        // memory. Run this once per map and intersect.
        // Label is just a tag for the diff — NOT a map id.
        // Nothing here depends on knowing which map you are in.
        static int sceneLabel = 1;
        ImGui::InputInt("Snapshot label", &sceneLabel);
        if (ImGui::Button("Snapshot Scene")) {
            const bool ok = DebugSnapshotScene(sceneLabel);
            spdlog::info("[debug] Scene snapshot {} ok={}", sceneLabel, ok);
            ++sceneLabel;
        }
        ImGui::TextDisabled("Snapshot in several maps; label is just a tag.");
        ImGui::SameLine();
        // Asks the OS which files are mapped — if the active
        // .DMap is among them, that names the map directly.
        if (ImGui::Button("List Mapped Files")) {
            const bool ok = DebugListMappedFiles();
            spdlog::info("[debug] Mapped file list ok={}", ok);
        }
        ImGui::Separator();

        if (ImGui::Button("Scan For Map ID")) {
            const bool ok = DebugScanForMapId(expectedMapId);
            spdlog::info("[debug] Map-id scan for {} ok={}", expectedMapId, ok);
        }
        ImGui::TextDisabled("Appends to C:\\Users\\Public\\coclassic_mapid.json.");
        ImGui::TextDisabled("Run in THREE+ maps, then intersect the hits.");

        // Live readout of the resolved chain — walk between
        // maps and this should always match where you are.
        ImGui::Separator();
        const OBJID liveMapId = Game::GetCurrentMapId();
        if (liveMapId)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "Current map id (live): %u", liveMapId);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Current map id (live): UNAVAILABLE");
        {
            // Show each step of the chain so a failure is
            // attributable rather than opaque.
            const MapIdDiag d = MapProbe_MapIdDiag();
            ImGui::TextDisabled("  0x699560=%u   0x699564=%u  (direct u32 globals)",
                                d.val1, d.val2);

            // Terrain loaded from the client's own .DMap file
            // for whichever map we're on.
            if (MapGrid* g = GetCurrentMapGrid()) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "Terrain: map %d  %dx%d loaded",
                    g->GetMapId(), g->GetWidth(), g->GetHeight());
                if (hero) {
                    const int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
                    ImGui::TextDisabled("  hero (%d,%d) walkable=%d alt=%d",
                        hx, hy, (int)g->IsWalkable(hx, hy), g->GetAltitude(hx, hy));
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "Terrain: not loaded");
            }
        }

        ImGui::Separator();
        // Ground-truth search: the pattern file is cut from the
        // map's own .DMap on disk, so a hit proves the client
        // holds that map data verbatim at that address.
        if (ImGui::Button("Search DMap Pattern")) {
            const bool ok = DebugSearchPattern();
            spdlog::info("[debug] Pattern search ok={}", ok);
        }
        ImGui::TextDisabled("Uses C:\\Users\\Public\\coclassic_pattern.bin");

        // The walk trace is what makes the scan decisive, so
        // surface how much of it has been collected.
        ImGui::Text("Walk trace: %d tiles", MapProbe_TraceCount());
        ImGui::SameLine();
        if (ImGui::Button("Clear trace"))
            MapProbe_ClearTrace();
        ImGui::SameLine();
        // Identifies the current map empirically, by matching
        // walked tiles against every .DMap on disk — map-name
        // tables disagree with each other and can't be trusted.
        if (ImGui::Button("Dump Walk Trace")) {
            const bool ok = DebugDumpWalkTrace();
            spdlog::info("[debug] Walk trace dump ok={}", ok);
        }
        ImGui::TextDisabled("Walk around a while before scanning - every tile you");
        ImGui::TextDisabled("stand on must be walkable in the real grid.");

        ImGui::Separator();
        // Stage 3: verify a candidate grid by CONTENT. The
        // hero is necessarily on a walkable cell, so the
        // neighbourhood around them is the ground truth.
        static char gridBaseHex[32] = "542C3000";
        static int  cellStride = 12;
        static int  cellRadius = 5;
        ImGui::TextDisabled("Stage 3: verify a candidate cell grid around the hero.");
        ImGui::InputText("Grid base (hex)", gridBaseHex, sizeof(gridBaseHex),
                         ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputInt("Bytes per cell", &cellStride);
        ImGui::InputInt("Radius (tiles)", &cellRadius);
        if (ImGui::Button("Dump Cells Around Hero")) {
            const unsigned long long b = strtoull(gridBaseHex, nullptr, 16);
            const bool ok = DebugProbeCells(b, cellStride, mapW, mapH, cellRadius);
            spdlog::info("[debug] Cell dump base=0x{:X} stride={} ok={}", b, cellStride, ok);
        }
    }
}

// Session 12: hunting whatever drives green/white/red/black
// monster name color (a level-relative "con color" per the
// user's explanation -- USERSTATUS_RED/BLACK were checked
// and ruled out for monsters). Dumps every nearby monster's
// name/ID plus raw unmapped CRole memory around the
// already-known HP/stamina fields, so multiple presses
// across a play session build up a correlatable dataset.
static void RenderMiscMonsterStatScanSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Monster Stat Scan", kSectionFlags)) {
        ImGui::TextDisabled("Read-only. Appends to C:\\Users\\Public\\coclassic_monsterscan.json");
        ImGui::TextDisabled("Press near any monster(s) -- name/color doesn't need to be identified now,");
        ImGui::TextDisabled("just remember roughly when/where so it can be matched up afterward.");
        if (ImGui::Button("Dump Nearby Monster Stats")) {
            const int n = DumpNearbyMonsterStats();
            spdlog::info("[debug] Monster stat scan wrote {} entries", n);
        }
    }
}

// Session 9: combat packet verification. AttackTarget /
// ShootTarget / MagicAttack are all plain packet builders on
// the proven SendMsg path (no RVAs, no VERIFIED_V1074 gate),
// so they were expected to work as soon as entity
// enumeration was restored — this tests one action at a
// time rather than switching on a whole hunt loop.
static void RenderMiscCombatTestSection(CHero* hero)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Debug: Combat Test", kSectionFlags)) {
        CRole* nearest = nullptr;
        float bestDist = 1e9f;
        if (hero) {
            for (CRole* r : Entities::Get()) {
                if (!r || !Entities::IsAlive(r) || !r->IsMonster() || r->IsDead())
                    continue;
                const float d = hero->m_posMap.DistanceTo(r->m_posMap);
                if (d < bestDist) { bestDist = d; nearest = r; }
            }
        }

        if (!nearest) {
            ImGui::TextDisabled("No live monster in the entity list.");
        } else {
            ImGui::Text("Nearest: %s  id=%u  pos=(%d,%d)  dist=%.1f",
                        nearest->GetName(), nearest->GetID(),
                        nearest->m_posMap.x, nearest->m_posMap.y, bestDist);

            const OBJID tid = nearest->GetID();
            const Position tpos = nearest->m_posMap;

            if (ImGui::Button("Shoot (ranged)")) {
                spdlog::info("[debug] Combat: ShootTarget id={}", tid);
                hero->ShootTarget(tid);
            }
            ImGui::SameLine();
            if (ImGui::Button("Attack (melee)")) {
                spdlog::info("[debug] Combat: AttackTarget id={}", tid);
                hero->AttackTarget(tid, tpos);
            }

            static int magicId = 0;
            ImGui::InputInt("Magic/skill ID", &magicId);
            if (ImGui::Button("Cast magic at target")) {
                spdlog::info("[debug] Combat: MagicAttack magic={} id={}", magicId, tid);
                hero->MagicAttack((OBJID)magicId, tid, tpos);
            }
            ImGui::TextDisabled("Scatter is a magic ID — see the Skills list for yours.");
        }
    }
}

static void RenderMiscLootDropNotificationsSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Loot Drop Notifications", kSectionFlags)) {
        MiscSettings& misc = GetMiscSettings();
        ImGui::Checkbox("Notify on Loot Drop", &misc.lootDropNotifyEnabled);
        ImGui::TextDisabled("Sends a Discord notification when an item drops from your kill.");
    }
}

static void RenderMiscItemNotificationsSection(CHero* /*hero*/)
{
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Item Notifications", kSectionFlags)) {
        MiscSettings& misc = GetMiscSettings();
        ImGui::Checkbox("Enable Item Notifications", &misc.itemNotifyEnabled);
        ImGui::TextDisabled("Sends a Discord notification when a tracked item enters your inventory.");

        if (ImGui::SmallButton("Clear Notify Items"))
            misc.notifyItemIds.clear();
        ImGui::SameLine();
        ImGui::Text("Tracked: %d", (int)misc.notifyItemIds.size());

        static char notifyItemSearch[128] = "";
        ImGui::InputText("Search##notifyitem", notifyItemSearch, sizeof(notifyItemSearch));

        std::string searchLower;
        for (const char* p = notifyItemSearch; *p; ++p)
            searchLower.push_back((char)std::tolower((unsigned char)*p));

        ImGui::BeginChild("##notifyitembrowser", ImVec2(0, 260.0f), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("##notifyitemtable", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Notify", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Mention", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            int shown = 0;
            for (const ItemTypeInfo* info : GetAllItemTypes()) {
                if (!info)
                    continue;

                if (!searchLower.empty()) {
                    std::string nameLower;
                    for (char c : info->name)
                        nameLower.push_back((char)std::tolower((unsigned char)c));
                    if (nameLower.find(searchLower) == std::string::npos
                        && std::to_string(info->id).find(notifyItemSearch) == std::string::npos)
                        continue;
                }

                const bool isNotify = std::find(misc.notifyItemIds.begin(),
                    misc.notifyItemIds.end(), info->id) != misc.notifyItemIds.end();
                const bool isMention = std::find(misc.mentionItemIds.begin(),
                    misc.mentionItemIds.end(), info->id) != misc.mentionItemIds.end();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", info->name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", info->id);

                ImGui::TableNextColumn();
                {
                    char btnId[64];
                    snprintf(btnId, sizeof(btnId), "%s##miscnotify%u",
                             isNotify ? "Remove" : "Add", info->id);
                    if (ImGui::SmallButton(btnId)) {
                        if (isNotify) {
                            misc.notifyItemIds.erase(
                                std::remove(misc.notifyItemIds.begin(),
                                            misc.notifyItemIds.end(), info->id),
                                misc.notifyItemIds.end());
                            misc.mentionItemIds.erase(
                                std::remove(misc.mentionItemIds.begin(),
                                            misc.mentionItemIds.end(), info->id),
                                misc.mentionItemIds.end());
                        } else {
                            misc.notifyItemIds.push_back(info->id);
                        }
                    }
                }

                ImGui::TableNextColumn();
                if (isNotify) {
                    char btnId[64];
                    snprintf(btnId, sizeof(btnId), "%s##miscmention%u",
                             isMention ? "Remove" : "Add", info->id);
                    if (ImGui::SmallButton(btnId)) {
                        if (isMention)
                            misc.mentionItemIds.erase(
                                std::remove(misc.mentionItemIds.begin(),
                                            misc.mentionItemIds.end(), info->id),
                                misc.mentionItemIds.end());
                        else
                            misc.mentionItemIds.push_back(info->id);
                    }
                }

                if (searchLower.empty() && ++shown >= 250)
                    break;
            }

            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
}

static void RenderMapOverviewSection(CHero* hero, CGameMap* map, bool hasMap, OBJID curMapId,
    const std::vector<Gateway>& gateways, MapSettings& ms)
{
    constexpr ImGuiTreeNodeFlags kMapSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
    int heroTileX = hero->m_posMap.x;
    int heroTileY = hero->m_posMap.y;
                    if (ImGui::CollapsingHeader("Overview", kMapSectionFlags)) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                        "Map: %s  (ID: %u)", GetMapName(curMapId), curMapId);
                    if (map) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "  %dx%d", map->m_sizeMap.iWidth, map->m_sizeMap.iHeight);
                    }

                    // Hero position with copy button
                    ImGui::Text("Hero: (%d, %d)", heroTileX, heroTileY);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##pos")) {
                        char posBuf[64];
                        snprintf(posBuf, sizeof(posBuf), "{%d,%d}", heroTileX, heroTileY);
                        ImGui::SetClipboardText(posBuf);
                    }

                    // Show known gateways for current map
                    if (!gateways.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "  [%zu gateways]", gateways.size());
                    }

                    ImGui::Checkbox("Entities", &ms.showEntities);
                    ImGui::SameLine();
                    ImGui::Checkbox("Follow", &ms.followHero);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset View")) {
                        ms.followHero = true;
                        ms.cellSize = 1.0f;
                        g_mapCamX = (float)hero->m_posMap.x;
                        g_mapCamY = (float)hero->m_posMap.y;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("Wheel  - zoom (centred on the cursor)");
                        ImGui::Text("Middle or Right drag - pan");
                        ImGui::Text("Right double-click   - recentre on hero");
                        ImGui::Text("Left click           - capture, when a capture mode is armed");
                        ImGui::EndTooltip();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("zoom %.2fx", ms.cellSize);
                    if (hasMap) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Dump Map")) {
                            char dumpPath[MAX_PATH];
                            snprintf(dumpPath, sizeof(dumpPath), "mapdump_%u.bin", curMapId);
                            if (map->DumpToFile(dumpPath))
                                spdlog::info("[map] Dumped to {}", dumpPath);
                            else
                                spdlog::error("[map] Failed to dump map");
                        }
                    }
                    }
}

static void RenderMinimapSection(CHero* hero, CGameMap* map,
    const std::vector<CRole*>& entityList, const std::vector<CMapItem*>& mapItemList,
    bool hasMgr, bool hasMap, MapSettings& ms, const std::vector<Gateway>& gateways)
{
    constexpr ImGuiTreeNodeFlags kMapSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

                    // Snapshot map state
                    CellInfo* cells   = map ? map->m_pCellInfo : nullptr;
                    int       mapW_t  = map ? map->m_sizeMap.iWidth  : 0;
                    int       mapH_t  = map ? map->m_sizeMap.iHeight : 0;
                    bool      mapOk   = cells && mapW_t > 0 && mapH_t > 0
                                        && mapW_t < 10000 && mapH_t < 10000;
                    if (ImGui::CollapsingHeader("Minimap", kMapSectionFlags)) {
                    if (mapOk) {
                        int heroX = hero->m_posMap.x;
                        int heroY = hero->m_posMap.y;
                        float fs  = ms.cellSize;

                        // ── Compute camera center ──
                        float camTileX, camTileY;
                        if (ms.followHero) {
                            camTileX = (float)heroX;
                            camTileY = (float)heroY;
                        } else {
                            camTileX = g_mapCamX;
                            camTileY = g_mapCamY;
                        }

                        // Sync camera state so switching to free-cam starts from current view
                        if (ms.followHero) {
                            g_mapCamX = camTileX;
                            g_mapCamY = camTileY;
                        }

                        // Canvas: use all available space (entity table is collapsible)
                        float availW = ImGui::GetContentRegionAvail().x;
                        float availH = ImGui::GetContentRegionAvail().y;
                        float canvasW = availW;
                        float canvasH = availH;
                        // Session 10: was 80px — with Overview/Travel already open above
                        // it, that's often all the remaining window space left, so the
                        // minimap ended up tiny. The Map tab already scrolls, so a much
                        // taller floor just uses more of that scroll room instead of
                        // squeezing into whatever happened to be left over.
                        if (canvasH < 600.0f) canvasH = 600.0f;

                        // Auto-fit fs so the entire map diamond fills the canvas
                        float mapSpan = (float)(mapW_t + mapH_t);
                        float autoFitFs = (mapSpan > 0.0f) ? fminf(canvasW, canvasH) / mapSpan : 1.0f;
                        fs = autoFitFs;

                        // Effective radius: enough to cover the entire map from any camera position
                        int maxDim = (mapW_t > mapH_t) ? mapW_t : mapH_t;
                        int effectiveRadius = maxDim + 1;

                        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##mapcanvas", ImVec2(canvasW, canvasH));
                        bool canvasHovered = ImGui::IsItemHovered();
                        bool canvasClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        ImVec2 mousePos = ImGui::GetIO().MousePos;
                        ImDrawList* dl = ImGui::GetWindowDrawList();

                        fs = autoFitFs * ms.cellSize;

                        // Screen offset from canvas centre -> tile offset from
                        // the camera. Inverse of the isometric projection used
                        // when drawing:  sx = (dx-dy)*fs,  sy = (dx+dy)*fs.
                        auto screenToTileDelta = [](float sx, float sy, float scale,
                                                    float& outDX, float& outDY) {
                            if (scale <= 0.0f) { outDX = outDY = 0.0f; return; }
                            outDX = (sx + sy) / (2.0f * scale);
                            outDY = (sy - sx) / (2.0f * scale);
                        };

                        // ── Scroll wheel zoom, anchored on the cursor ──
                        // Previously this only scaled cellSize, so the view
                        // always grew from the canvas centre and whatever you
                        // were pointing at slid away — the main reason zooming
                        // felt uncontrollable. Now the tile under the cursor
                        // stays under the cursor.
                        if (canvasHovered) {
                            const float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f) {
                                const float cx0 = canvasPos.x + canvasW * 0.5f;
                                const float cy0 = canvasPos.y + canvasH * 0.5f;
                                const float mx = mousePos.x - cx0;
                                const float my = mousePos.y - cy0;

                                float adx, ady;
                                screenToTileDelta(mx, my, fs, adx, ady);
                                const float anchorTileX = camTileX + adx;
                                const float anchorTileY = camTileY + ady;

                                // Exponential step: constant feel per notch,
                                // instead of 1+0.15*w which is coarse zoomed
                                // out and sluggish zoomed in.
                                ms.cellSize *= powf(1.15f, wheel);
                                if (ms.cellSize < 0.2f)  ms.cellSize = 0.2f;
                                if (ms.cellSize > 40.0f) ms.cellSize = 40.0f;
                                fs = autoFitFs * ms.cellSize;

                                if (!ms.followHero) {
                                    // Re-place the camera so the anchor tile
                                    // lands back under the mouse at the new scale.
                                    float ndx, ndy;
                                    screenToTileDelta(mx, my, fs, ndx, ndy);
                                    g_mapCamX = anchorTileX - ndx;
                                    g_mapCamY = anchorTileY - ndy;
                                    camTileX = g_mapCamX;
                                    camTileY = g_mapCamY;
                                }
                            }
                        }

                        // ── Drag panning: middle OR right mouse ──
                        // Right-only was unintuitive and collides with context
                        // menus; middle-drag is the conventional pan gesture.
                        const bool panDown = ImGui::IsMouseDown(ImGuiMouseButton_Right)
                                          || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                        const bool panClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                                             || ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
                        if (canvasHovered && panClicked) {
                            g_mapDragging = true;
                            g_mapDragStartMouseX = mousePos.x;
                            g_mapDragStartMouseY = mousePos.y;
                            g_mapDragStartCamX = g_mapCamX;
                            g_mapDragStartCamY = g_mapCamY;
                            if (ms.followHero) ms.followHero = false;
                        }
                        if (g_mapDragging) {
                            if (panDown) {
                                const float sx = mousePos.x - g_mapDragStartMouseX;
                                const float sy = mousePos.y - g_mapDragStartMouseY;
                                float ddx, ddy;
                                screenToTileDelta(sx, sy, fs, ddx, ddy);
                                g_mapCamX = g_mapDragStartCamX - ddx;
                                g_mapCamY = g_mapDragStartCamY - ddy;
                                camTileX = g_mapCamX;
                                camTileY = g_mapCamY;
                            } else {
                                g_mapDragging = false;
                            }
                        }

                        // Double-click empty space to recentre on the hero —
                        // an easy way back after panning away.
                        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right)) {
                            ms.followHero = true;
                            g_mapCamX = (float)heroX;
                            g_mapCamY = (float)heroY;
                            camTileX = g_mapCamX;
                            camTileY = g_mapCamY;
                        }

                        ImVec2 canvasEnd(canvasPos.x + canvasW, canvasPos.y + canvasH);
                        dl->PushClipRect(canvasPos, canvasEnd, true);
                        dl->AddRectFilled(canvasPos, canvasEnd, IM_COL32(20, 20, 20, 255));

                        float centerX = canvasPos.x + canvasW * 0.5f;
                        float centerY = canvasPos.y + canvasH * 0.5f;

                        // Draw isometric diamond tiles
                        DrawMapCells(dl, cells, mapW_t, mapH_t,
                                     camTileX, camTileY, effectiveRadius, fs,
                                     centerX, centerY,
                                     canvasPos.x, canvasPos.y,
                                     canvasEnd.x, canvasEnd.y);

                        // Draw entity dots on the minimap
                        if (ms.showEntities) {
                            // Roles (NPCs, players, monsters)
                            if (hasMgr && g_entityFilter != 4) {
                                GuildSettings& guild = GetGuildSettings();
                                bool deadFilter = guild.showDeadOnly && hero->HasSyndicate();
                                for (CRole* e : entityList) {
                                    // Session 11 [CRASH FIX]: same hazard class as the
                                    // ground-item loop below (and the loot-scanning
                                    // functions fixed earlier this session) - a raw
                                    // pointer into the game's own heap, freeable at
                                    // any moment (entity dies/despawns) between the
                                    // scan snapshot and this render loop touching it.
                                    if (!e || !Entities::IsAlive(e)) continue;
                                    if (e->GetID() == hero->GetID()) continue;

                                    bool isPlayer = e->IsPlayer();
                                    bool isMonster = e->IsMonster();
                                    bool isNpc = !isPlayer && !isMonster;

                                    if (g_entityFilter == 1 && !isNpc) continue;
                                    if (g_entityFilter == 2 && !isPlayer) continue;
                                    if (g_entityFilter == 3 && !isMonster) continue;

                                    if (deadFilter) {
                                        if (!isPlayer || e->m_idSyndicate != hero->m_idSyndicate) continue;
                                        if (!e->IsDead() && !e->TestState(USERSTATUS_GHOST)) continue;
                                    }

                                    float edx = (float)e->m_posMap.x - camTileX;
                                    float edy = (float)e->m_posMap.y - camTileY;
                                    if (edx > effectiveRadius || edx < -effectiveRadius) continue;
                                    if (edy > effectiveRadius || edy < -effectiveRadius) continue;

                                    const bool isDead = e->IsDead() || e->TestState(USERSTATUS_GHOST);
                                    ImU32 entCol;
                                    if (isDead && (isPlayer || isMonster))
                                                        entCol = IM_COL32(140, 140, 140, 180);
                                    else if (isPlayer)  entCol = IM_COL32(80, 180, 255, 255);
                                    else if (isMonster) entCol = IM_COL32(255, 80, 80, 255);
                                    else                entCol = IM_COL32(255, 255, 100, 255);

                                    float cx = centerX + (edx - edy) * fs;
                                    float cy = centerY + (edx + edy) * fs;
                                    dl->AddCircleFilled(ImVec2(cx, cy), fs * 0.8f, entCol);
                                }
                            }

                            // Ground items
                            if (hasMap && (g_entityFilter == 0 || g_entityFilter == 4)) {
                                for (size_t i = 0; i < mapItemList.size() && i < 500; i++) {
                                    CMapItem* item = mapItemList[i];
                                    // Session 11 [CRASH FIX]: confirmed live crash site -
                                    // same hazard as the loot-scanning functions fixed
                                    // earlier this session (hunt_loot.cpp) - ground items
                                    // are raw pointers into the game's own heap, freeable
                                    // at any moment (picked up/despawned) between the scan
                                    // snapshot and this render loop touching item->m_pos.
                                    if (!item || !MapItems::IsAlive(item)) continue;

                                    float edx = (float)item->m_pos.x - camTileX;
                                    float edy = (float)item->m_pos.y - camTileY;
                                    if (edx > effectiveRadius || edx < -effectiveRadius) continue;
                                    if (edy > effectiveRadius || edy < -effectiveRadius) continue;

                                    float cx = centerX + (edx - edy) * fs;
                                    float cy = centerY + (edx + edy) * fs;
                                    dl->AddCircleFilled(ImVec2(cx, cy), fs * 0.8f,
                                        IM_COL32(220, 160, 255, 255)); // purple for items
                                }
                            }
                        }

                        // ── Draw gateway markers on minimap ──
                        for (auto& gw : gateways) {
                            float gdx = (float)gw.pos.x - camTileX;
                            float gdy = (float)gw.pos.y - camTileY;
                            if (gdx > effectiveRadius || gdx < -effectiveRadius) continue;
                            if (gdy > effectiveRadius || gdy < -effectiveRadius) continue;

                            float gcx = centerX + (gdx - gdy) * fs;
                            float gcy = centerY + (gdx + gdy) * fs;

                            ImU32 gwCol = (gw.type == GatewayType::Portal)
                                ? IM_COL32(0, 200, 255, 220)    // cyan for portals
                                : IM_COL32(255, 200, 0, 220);   // yellow for NPCs

                            // Diamond marker
                            float gs = fs * 1.5f;
                            dl->AddQuad(
                                ImVec2(gcx, gcy - gs),
                                ImVec2(gcx + gs, gcy),
                                ImVec2(gcx, gcy + gs),
                                ImVec2(gcx - gs, gcy),
                                gwCol, 2.0f);
                        }

                        // Hero dot (at hero's actual position, not necessarily center)
                        {
                            float hdx = (float)heroX - camTileX;
                            float hdy = (float)heroY - camTileY;
                            float hsx = centerX + (hdx - hdy) * fs;
                            float hsy = centerY + (hdx + hdy) * fs;
                            dl->AddCircleFilled(ImVec2(hsx, hsy), fs + 1.0f,
                                IM_COL32(255, 255, 255, 255));
                        }

                        // ── Auto Hunt debug range circles ──
                        {
                            const AutoHuntSettings& ah = GetAutoHuntSettings();
                            auto DrawDebugCircle = [&](int radius, ImU32 col) {
                                if (radius <= 0) return;
                                constexpr int kSeg = 48;
                                for (int i = 0; i <= kSeg; ++i) {
                                    float angle = (float)i / (float)kSeg * 6.2831853f;
                                    float tx = (float)heroX + cosf(angle) * (float)radius;
                                    float ty = (float)heroY + sinf(angle) * (float)radius;
                                    float ddx = tx - camTileX;
                                    float ddy = ty - camTileY;
                                    dl->PathLineTo(ImVec2(centerX + (ddx - ddy) * fs,
                                                          centerY + (ddx + ddy) * fs));
                                }
                                dl->PathStroke(col, ImDrawFlags_Closed, 1.5f);
                            };
                            if (ah.debugShowActionRadius)
                                DrawDebugCircle(ah.actionRadius, IM_COL32(0, 220, 0, 180));
                            if (ah.debugShowClumpRadius)
                                DrawDebugCircle(ah.clumpRadius, IM_COL32(255, 165, 0, 180));
                            if (ah.debugShowMobSearchRange && ah.mobSearchRange > 0)
                                DrawDebugCircle(ah.mobSearchRange, IM_COL32(100, 160, 255, 180));
                            if (ah.debugShowLootRange)
                                DrawDebugCircle(ah.lootRange, IM_COL32(220, 160, 255, 180));
                            if (ah.debugShowSafetyRange && ah.safetyEnabled)
                                DrawDebugCircle(ah.safetyPlayerRange, IM_COL32(255, 60, 60, 180));
                            if (ah.debugShowAttackRange)
                                DrawDebugCircle(ah.rangedAttackRange, IM_COL32(0, 220, 0, 200));
                            if (ah.debugShowArcherSafety)
                                DrawDebugCircle(ah.archerSafetyDistance, IM_COL32(255, 60, 60, 200));
                            if (ah.debugShowScatterRange && ah.useScatterLogic) {
                                if (auto* hunt = PluginManager::Get().GetPlugin<ArcherHuntPlugin>()) {
                                    int scatterR = hunt->GetLastScatterRange();
                                    DrawDebugCircle(scatterR, IM_COL32(100, 160, 255, 160));
                                    if (scatterR > 0) {
                                        Position targetPos = hunt->GetLastTargetPos();
                                        float facingAngle = 0.0f;
                                        if (targetPos.x != heroX || targetPos.y != heroY) {
                                            facingAngle = atan2f(
                                                (float)(targetPos.y - heroY),
                                                (float)(targetPos.x - heroX));
                                        }
                                        constexpr int kArcSeg = 32;
                                        const float halfArc = 3.14159265f;
                                        const float startAngle = facingAngle - halfArc * 0.5f;
                                        auto TileToMinimap = [&](float ttx, float tty) {
                                            float ddx = ttx - camTileX;
                                            float ddy = tty - camTileY;
                                            return ImVec2(centerX + (ddx - ddy) * fs,
                                                          centerY + (ddx + ddy) * fs);
                                        };
                                        dl->PathLineTo(TileToMinimap((float)heroX, (float)heroY));
                                        for (int i = 0; i <= kArcSeg; ++i) {
                                            float a = startAngle + halfArc * (float)i / (float)kArcSeg;
                                            float atx = (float)heroX + cosf(a) * (float)scatterR;
                                            float aty = (float)heroY + sinf(a) * (float)scatterR;
                                            dl->PathLineTo(TileToMinimap(atx, aty));
                                        }
                                        dl->PathLineTo(TileToMinimap((float)heroX, (float)heroY));
                                        dl->PathStroke(IM_COL32(100, 160, 255, 200), ImDrawFlags_None, 1.5f);
                                    }
                                }
                            }
                            if (ah.debugShowBestClump) {
                                // Find the active (enabled) hunt plugin — either melee or archer
                                BaseHuntPlugin* hunt = nullptr;
                                if (auto* m = PluginManager::Get().GetPlugin<MeleeHuntPlugin>(); m && m->m_enabled)
                                    hunt = m;
                                else if (auto* a = PluginManager::Get().GetPlugin<ArcherHuntPlugin>(); a && a->m_enabled)
                                    hunt = a;
                                if (hunt) {
                                    Position clumpCenter = hunt->GetDebugBestClumpCenter();
                                    int clumpSize = hunt->GetDebugBestClumpSize();
                                    if (clumpSize >= 2 && (clumpCenter.x != 0 || clumpCenter.y != 0)) {
                                        // Draw clump radius circle at best clump center
                                        int clumpR = (std::max)(1, ah.clumpRadius);
                                        constexpr int kClumpSeg = 48;
                                        for (int i = 0; i <= kClumpSeg; ++i) {
                                            float angle = (float)i / (float)kClumpSeg * 6.2831853f;
                                            float tx = (float)clumpCenter.x + cosf(angle) * (float)clumpR;
                                            float ty = (float)clumpCenter.y + sinf(angle) * (float)clumpR;
                                            float ddx = tx - camTileX;
                                            float ddy = ty - camTileY;
                                            dl->PathLineTo(ImVec2(centerX + (ddx - ddy) * fs,
                                                                  centerY + (ddx + ddy) * fs));
                                        }
                                        dl->PathStroke(IM_COL32(255, 220, 50, 200), ImDrawFlags_Closed, 2.0f);
                                        // Center dot
                                        float cdx = (float)clumpCenter.x - camTileX;
                                        float cdy = (float)clumpCenter.y - camTileY;
                                        dl->AddCircleFilled(
                                            ImVec2(centerX + (cdx - cdy) * fs, centerY + (cdx + cdy) * fs),
                                            fs * 0.5f, IM_COL32(255, 220, 50, 220));
                                    }
                                }
                            }
                        }

                        {
                            AutoHuntSettings& autoHunt = GetAutoHuntSettings();
                            if (autoHunt.zoneMapId == Game::GetCurrentMapId()) {
                                auto TileToMiniMap = [&](const Position& tile) {
                                    float dx = (float)tile.x - camTileX;
                                    float dy = (float)tile.y - camTileY;
                                    return ImVec2(
                                        centerX + (dx - dy) * fs,
                                        centerY + (dx + dy) * fs);
                                };

                                const ImU32 zoneCol = IM_COL32(255, 215, 0, 220);
                                if (autoHunt.zoneMode == AutoHuntZoneMode::Circle
                                    && autoHunt.zoneCenter.x != 0
                                    && autoHunt.zoneCenter.y != 0
                                    && autoHunt.zoneRadius > 0) {
                                    for (int i = 0; i <= 32; ++i) {
                                        const float t = (float)i / 32.0f;
                                        const float angle = t * 6.2831853f;
                                        Position edge = {
                                            autoHunt.zoneCenter.x + (int)roundf(cosf(angle) * autoHunt.zoneRadius),
                                            autoHunt.zoneCenter.y + (int)roundf(sinf(angle) * autoHunt.zoneRadius)
                                        };
                                        dl->PathLineTo(TileToMiniMap(edge));
                                    }
                                    dl->PathStroke(zoneCol, ImDrawFlags_Closed, 2.0f);
                                    dl->AddCircleFilled(TileToMiniMap(autoHunt.zoneCenter), fs * 0.45f, zoneCol);
                                } else if (autoHunt.zoneMode == AutoHuntZoneMode::Polygon
                                           && autoHunt.zonePolygon.size() >= 2) {
                                    for (const Position& vertex : autoHunt.zonePolygon)
                                        dl->PathLineTo(TileToMiniMap(vertex));
                                    if (autoHunt.zonePolygon.size() >= 3)
                                        dl->PathStroke(zoneCol, ImDrawFlags_Closed, 2.0f);
                                    else
                                        dl->PathStroke(zoneCol, ImDrawFlags_None, 2.0f);

                                    // Draw interactive vertex handles
                                    bool huntActive = PluginManager::Get().GetPlugin<MeleeHuntPlugin>() != nullptr
                                                   || PluginManager::Get().GetPlugin<ArcherHuntPlugin>() != nullptr;
                                    int dragIdx = GetAutoHuntSettings().editDragVertex;

                                    for (size_t vi = 0; vi < autoHunt.zonePolygon.size(); vi++) {
                                        ImVec2 vScr = TileToMiniMap(autoHunt.zonePolygon[vi]);
                                        bool isHot = false;
                                        if ((int)vi == dragIdx) {
                                            isHot = true;
                                        } else if (canvasHovered && dragIdx < 0) {
                                            float vdx = mousePos.x - vScr.x;
                                            float vdy = mousePos.y - vScr.y;
                                            if (vdx * vdx + vdy * vdy < 8.0f * 8.0f)
                                                isHot = true;
                                        }
                                        float r = isHot ? fs * 0.9f : fs * 0.6f;
                                        dl->AddCircleFilled(vScr, r,
                                            isHot ? IM_COL32(255, 255, 255, 255) : zoneCol);
                                        dl->AddCircle(vScr, r, IM_COL32(0, 0, 0, 180), 0, 1.5f);
                                    }

                                    // ── Polygon edit interactions (always active) ──
                                    if (huntActive && canvasHovered) {
                                        auto& poly = autoHunt.zonePolygon;

                                        // Hit-test vertices
                                        int hoveredVtx = -1;
                                        float bestVtxDist2 = 8.0f * 8.0f;
                                        for (size_t vi = 0; vi < poly.size(); vi++) {
                                            ImVec2 vs = TileToMiniMap(poly[vi]);
                                            float vdx = mousePos.x - vs.x;
                                            float vdy = mousePos.y - vs.y;
                                            float d2 = vdx * vdx + vdy * vdy;
                                            if (d2 < bestVtxDist2) {
                                                bestVtxDist2 = d2;
                                                hoveredVtx = (int)vi;
                                            }
                                        }

                                        // Mouse-to-tile helper
                                        auto MouseToTile = [&]() -> Position {
                                            float mdx = (mousePos.x - centerX) / fs;
                                            float mdy = (mousePos.y - centerY) / fs;
                                            return {
                                                (int)roundf(camTileX + (mdx + mdy) * 0.5f),
                                                (int)roundf(camTileY + (mdy - mdx) * 0.5f)
                                            };
                                        };

                                        // Start drag or insert vertex on edge
                                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                            if (hoveredVtx >= 0) {
                                                GetAutoHuntSettings().editDragVertex = hoveredVtx;
                                                dragIdx = hoveredVtx;
                                                canvasClicked = false;
                                            } else if (poly.size() >= 2) {
                                                // Find closest edge for insertion
                                                float bestEdgeDist2 = 12.0f * 12.0f;
                                                int insertAfter = -1;
                                                for (size_t ei = 0; ei < poly.size(); ei++) {
                                                    size_t ej = (ei + 1) % poly.size();
                                                    ImVec2 a = TileToMiniMap(poly[ei]);
                                                    ImVec2 b = TileToMiniMap(poly[ej]);
                                                    float abx = b.x - a.x, aby = b.y - a.y;
                                                    float abLen2 = abx * abx + aby * aby;
                                                    if (abLen2 < 1.0f) continue;
                                                    float t = ((mousePos.x - a.x) * abx +
                                                               (mousePos.y - a.y) * aby) / abLen2;
                                                    if (t < 0.05f || t > 0.95f) continue;
                                                    float px = a.x + t * abx - mousePos.x;
                                                    float py = a.y + t * aby - mousePos.y;
                                                    float d2 = px * px + py * py;
                                                    if (d2 < bestEdgeDist2) {
                                                        bestEdgeDist2 = d2;
                                                        insertAfter = (int)ei;
                                                    }
                                                }
                                                if (insertAfter >= 0) {
                                                    Position newVtx = MouseToTile();
                                                    poly.insert(poly.begin() + insertAfter + 1, newVtx);
                                                    GetAutoHuntSettings().editDragVertex = insertAfter + 1;
                                                    dragIdx = insertAfter + 1;
                                                    canvasClicked = false;
                                                }
                                            }
                                        }

                                        // Continue drag
                                        if (dragIdx >= 0 && dragIdx < (int)poly.size()
                                            && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                            poly[dragIdx] = MouseToTile();
                                            canvasClicked = false;
                                        }

                                        // End drag
                                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                                            GetAutoHuntSettings().editDragVertex = -1;
                                    }
                                }
                            }
                        }

                        // ── Draw path waypoints on minimap ──
                        {
                            auto& pf = Pathfinder::Get();
                            auto& waypoints = pf.GetWaypoints();
                            size_t wpIdx = pf.GetCurrentIndex();
                            if (pf.IsActive() && wpIdx < waypoints.size()) {
                                float h_dx = (float)heroX - camTileX;
                                float h_dy = (float)heroY - camTileY;
                                ImVec2 prev(centerX + (h_dx - h_dy) * fs,
                                            centerY + (h_dx + h_dy) * fs);
                                for (size_t wi = wpIdx; wi < waypoints.size(); wi++) {
                                    float wdx = (float)waypoints[wi].x - camTileX;
                                    float wdy = (float)waypoints[wi].y - camTileY;
                                    float wcx = centerX + (wdx - wdy) * fs;
                                    float wcy = centerY + (wdx + wdy) * fs;
                                    dl->AddLine(prev, ImVec2(wcx, wcy),
                                        IM_COL32(0, 255, 128, 180), 2.0f);
                                    dl->AddCircleFilled(ImVec2(wcx, wcy),
                                        fs * 0.6f, IM_COL32(0, 255, 128, 220));
                                    prev = ImVec2(wcx, wcy);
                                }
                            }
                        }

                        // ── Click-to-jump / pathfind ──
                        // Inverse isometric: screen → tile (relative to camera)
                        if (canvasClicked) {
                            float dx = (mousePos.x - centerX) / fs;
                            float dy = (mousePos.y - centerY) / fs;
                            float ftdx = (dx + dy) * 0.5f;
                            float ftdy = (dy - dx) * 0.5f;
                            int tileX = (int)roundf(camTileX + ftdx);
                            int tileY = (int)roundf(camTileY + ftdy);

                            if (!PluginManager::Get().HandleMapClick({tileX, tileY})) {
                                // Cancel any active path on manual click
                                Pathfinder::Get().Stop();

                                const int clickDist = CGameMap::TileDist(heroX, heroY, tileX, tileY);
                                const bool preferWalk = clickDist <= kWalkInsteadOfJumpTiles
                                    && !ShouldUseAggressiveSpeeds(GetAutoHuntSettings())
                                    && !IsTileOccupied(tileX, tileY)
                                    && map->IsWalkable(tileX, tileY)
                                    && map->CanReach(heroX, heroY, tileX, tileY);

                                if (preferWalk) {
                                    // Short hop, no player nearby to hide movement style
                                    // from (or speedhack is off) — walk instead of jump.
                                    hero->Walk(tileX, tileY);
                                } else if (map->CanJump(heroX, heroY, tileX, tileY, CGameMap::GetHeroAltThreshold())
                                    && !IsTileOccupied(tileX, tileY)) {
                                    // Direct jump
                                    hero->Jump(tileX, tileY);
                                } else if (map->IsWalkable(tileX, tileY)) {
                                    // Pathfind: A* tile path → simplify into jumps
                                    auto tilePath = map->FindPath(heroX, heroY, tileX, tileY, 1000000);
                                    if (!tilePath.empty()) {
                                        auto waypoints = map->SimplifyPath(tilePath);
                                        if (!waypoints.empty()) {
                                            Pathfinder::Get().StartPath(
                                                waypoints,
                                                [] { return GetMovementIntervalMs(GetAutoHuntSettings()); });
                                        }
                                    }
                                }
                            }
                        }

                        // ── Hover tooltip ──
                        if (canvasHovered) {
                            float dx = (mousePos.x - centerX) / fs;
                            float dy = (mousePos.y - centerY) / fs;
                            float ftdx = (dx + dy) * 0.5f;
                            float ftdy = (dy - dx) * 0.5f;
                            int tileX = (int)roundf(camTileX + ftdx);
                            int tileY = (int)roundf(camTileY + ftdy);
                            int dist = CGameMap::TileDist(heroX, heroY, tileX, tileY);
                            bool occupied = IsTileOccupied(tileX, tileY);
                            bool canJump = map->CanJump(heroX, heroY, tileX, tileY, CGameMap::GetHeroAltThreshold())
                                           && !occupied;

                            // Highlight hovered tile
                            float htdx = (float)tileX - camTileX;
                            float htdy = (float)tileY - camTileY;
                            float hcx = centerX + (htdx - htdy) * fs;
                            float hcy = centerY + (htdx + htdy) * fs;
                            ImU32 hlCol = canJump
                                ? IM_COL32(255, 255, 255, 80)
                                : IM_COL32(255, 0, 0, 80);
                            dl->AddQuadFilled(
                                ImVec2(hcx, hcy - fs),
                                ImVec2(hcx + fs, hcy),
                                ImVec2(hcx, hcy + fs),
                                ImVec2(hcx - fs, hcy),
                                hlCol);

                            // Detailed tooltip with cell info
                            CellInfo* hoverCell = map->GetCell(tileX, tileY);
                            uint16_t mask = hoverCell ? CGameMap::GetMask(hoverCell) : 0;
                            uint16_t terrain = hoverCell ? CGameMap::GetTerrain(hoverCell) : 0;
                            int16_t alt = hoverCell ? CGameMap::GetAltitude(hoverCell) : 0;
                            bool walkable = map->IsWalkable(tileX, tileY);

                            const char* reason = "";
                            if (!walkable)
                                reason = " [blocked]";
                            else if (canJump)
                                reason = "";
                            else if (dist > CGameMap::MAX_JUMP_DIST
                                     || !map->CanReach(heroX, heroY, tileX, tileY))
                                reason = " [pathfind]";
                            else if (occupied)
                                reason = " [occupied]";

                            ImGui::BeginTooltip();
                            ImGui::Text("(%d, %d) dist=%d%s",
                                tileX, tileY, dist, reason);
                            ImGui::Text("terrain=%u mask=%u alt=%d",
                                terrain, mask, alt);
                            ImGui::EndTooltip();
                        }

                        dl->PopClipRect();

                        // ── Gateway table for current map ──
                        if (!gateways.empty() && ImGui::TreeNode("Gateways##gwlist")) {
                            for (size_t gi = 0; gi < gateways.size(); gi++) {
                                auto& gw = gateways[gi];
                                const char* typeStr = (gw.type == GatewayType::Portal)
                                    ? "Portal" : "NPC";
                                if (gw.IsIntraMap())
                                    typeStr = "Warp";
                                int gdist = CGameMap::TileDist(heroX, heroY,
                                    gw.pos.x, gw.pos.y);
                                if (gw.HasDestPos()) {
                                    ImGui::Text("[%zu] %s (%d,%d) -> (%d,%d) %s  dist=%d",
                                        gi, typeStr, gw.pos.x, gw.pos.y,
                                        gw.destPos.x, gw.destPos.y,
                                        gw.IsIntraMap() ? "" : GetMapName(gw.destMapId),
                                        gdist);
                                } else {
                                    ImGui::Text("[%zu] %s (%d,%d) -> %s  dist=%d",
                                        gi, typeStr, gw.pos.x, gw.pos.y,
                                        GetMapName(gw.destMapId), gdist);
                                }
                            }
                            ImGui::TreePop();
                        }

                    } else {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "Map data not available.");
                    }

                    // ── Entity table (collapsible) ──
                    }
}

static void RenderEntitiesTableSection(CHero* hero,
    const std::vector<CRole*>& entityList, const std::vector<CMapItem*>& mapItemList,
    bool hasMgr, bool hasMap)
{
                    if (ImGui::CollapsingHeader("Entities##table")) {
                        ImGui::Text("Show:");
                        ImGui::SameLine();
                        ImGui::RadioButton("All", &g_entityFilter, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("NPCs", &g_entityFilter, 1);
                        ImGui::SameLine();
                        ImGui::RadioButton("Players", &g_entityFilter, 2);
                        ImGui::SameLine();
                        ImGui::RadioButton("Monsters", &g_entityFilter, 3);
                        ImGui::SameLine();
                        ImGui::RadioButton("Items", &g_entityFilter, 4);

                        bool noData = !hasMgr && !hasMap;
                        if (noData) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                "No entities nearby.");
                        } else if (ImGui::BeginTable("##ent", 8,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                ImVec2(0, 200.0f))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("ID",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Guild", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
                            // Session 13: CRole+0x6E8, found via a full-range live
                            // memory scan correlated against user-confirmed
                            // green/white/red/black monster tiers — see
                            // CRole::GetLevel's comment. Live-verify the numbers
                            // shown here against the actual displayed name color
                            // before trusting this for Auto-Level danger logic.
                            ImGui::TableSetupColumn("Lvl##level", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                            // Session 12: unverified — see CRole::IsRedName/
                            // IsBlackName's comment. Cross-check this column
                            // against a known redname/blackname player's
                            // actual in-game name color before trusting it
                            // for anything (e.g. Paranoia Mode threat tiers).
                            ImGui::TableSetupColumn("PK?##pkstatus", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Pos",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Dist",  ImGuiTableColumnFlags_WidthFixed, 50.0f);
                            ImGui::TableHeadersRow();

                            // Roles (NPCs, players, monsters)
                            if (hasMgr && g_entityFilter != 4) {
                                GuildSettings& guild = GetGuildSettings();
                                bool deadFilter = guild.showDeadOnly && hero->HasSyndicate();
                                // Session 12: reuse entityList (fetched once above for the
                                // minimap) instead of a second Entities::Get() heap-scan
                                // copy + IsAlive() pass over the same entities this frame.
                                const std::vector<CRole*>& tableRoles = entityList;
                                for (size_t i = 0; i < tableRoles.size() && i < 500; i++) {
                                    CRole* e = tableRoles[i];
                                    // Session 11 [CRASH FIX]: same hazard as the minimap's
                                    // entity loop earlier in this function - see its comment.
                                    if (!e || !Entities::IsAlive(e)) continue;

                                    bool isPlayer = e->IsPlayer();
                                    bool isMonster = e->IsMonster();
                                    bool isNpc = !isPlayer && !isMonster;

                                    if (g_entityFilter == 1 && !isNpc) continue;
                                    if (g_entityFilter == 2 && !isPlayer) continue;
                                    if (g_entityFilter == 3 && !isMonster) continue;

                                    if (deadFilter) {
                                        if (!isPlayer || e->m_idSyndicate != hero->m_idSyndicate) continue;
                                        if (!e->IsDead() && !e->TestState(USERSTATUS_GHOST)) continue;
                                    }

                                    ImGui::PushID((int)i);

                                    const char* type = "NPC";
                                    ImVec4 typeColor(1.0f, 1.0f, 0.4f, 1.0f);
                                    if (isPlayer) {
                                        type = "Player";
                                        typeColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                                    } else if (isMonster) {
                                        type = "Monster";
                                        typeColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                                    }

                                    float dist = hero->m_posMap.DistanceTo(e->m_posMap);

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%u", e->GetID());
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%s", e->GetName());
                                    ImGui::TableNextColumn();
                                    if (e->HasSyndicate()) {
                                        auto* entSet = CEntitySet::GetInstance();
                                        const char* guildName = entSet ? entSet->GetSyndicateName(e->m_idSyndicate) : nullptr;
                                        const char* rankName = GetSyndicateRankName(e->m_nSyndicateRank);
                                        if (guildName && rankName[0])
                                            ImGui::Text("%s [%s]", guildName, rankName);
                                        else if (guildName)
                                            ImGui::Text("%s", guildName);
                                        else
                                            ImGui::Text("ID:%u", e->m_idSyndicate);
                                    }
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(typeColor, "%s", type);
                                    ImGui::TableNextColumn();
                                    if (isMonster || isPlayer)
                                        ImGui::Text("%d", e->GetLevel());
                                    ImGui::TableNextColumn();
                                    if (e->IsBlackName())
                                        ImGui::TextColored(ImVec4(0.6f, 0.1f, 0.1f, 1.0f), "Black");
                                    else if (e->IsRedName())
                                        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Red");
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d, %d)", e->m_posMap.x, e->m_posMap.y);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.0f", dist);
                                    ImGui::PopID();
                                }
                            }

                            // Ground items
                            if (hasMap && (g_entityFilter == 0 || g_entityFilter == 4)) {
                                for (size_t i = 0; i < mapItemList.size() && i < 500; i++) {
                                    CMapItem* item = mapItemList[i];
                                    // Session 11 [CRASH FIX]: same hazard as the minimap's
                                    // ground-item loop above - see its comment.
                                    if (!item || !MapItems::IsAlive(item)) continue;

                                    ImGui::PushID((int)(10000 + i));

                                    std::string name = FormatItemName(item->m_idType, item->GetPlus());
                                    int q = item->GetQuality();
                                    ImVec4 qc = (q >= ItemQuality::SUPER)  ? ImVec4(1,0.8f,0,1) :
                                                (q >= ItemQuality::ELITE)  ? ImVec4(0.6f,0.4f,1,1) :
                                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                                (q >= ItemQuality::REFINED)? ImVec4(0.4f,1,0.4f,1) :
                                                ImVec4(1,1,1,1);
                                    float dist = hero->m_posMap.DistanceTo(item->m_pos);

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%u", item->m_id);
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(qc, "%s", name.c_str());
                                    ImGui::TableNextColumn();
                                    // no guild for items
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(ImVec4(0.86f,0.63f,1,1), "Item");
                                    ImGui::TableNextColumn();
                                    // no level for items
                                    ImGui::TableNextColumn();
                                    // no PK status for items
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d, %d)", item->m_pos.x, item->m_pos.y);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.0f", dist);
                                    ImGui::PopID();
                                }
                            }

                            ImGui::EndTable();
                        }
                    }
}

static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* pSwapChain, UINT sync, UINT flags)
{
    // ── Lazy init (first call only) ──
    if (!g_initialized) {
        g_initialized = true;
        spdlog::info("[overlay] Present hook firing - initializing ImGui (D3D10 backend)");

        // Get the game's D3D10 device from the swapchain
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&g_pDevice);
        if (FAILED(hr) || !g_pDevice) {
            spdlog::error("[overlay] GetDevice(ID3D10Device) failed: 0x{:08X}", (unsigned long)hr);
            return OrigPresent(pSwapChain, sync, flags);
        }

        // Get game HWND from swapchain desc
        DXGI_SWAP_CHAIN_DESC desc{};
        pSwapChain->GetDesc(&desc);
        g_hGameWnd = desc.OutputWindow;
        spdlog::info("[overlay] D3D10 Device=0x{:X}, HWND=0x{:X}", (uintptr_t)g_pDevice, (uintptr_t)g_hGameWnd);

        // Init ImGui with D3D10 backend
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(g_hGameWnd);
        ImGui_ImplDX10_Init(g_pDevice);

        // Subclass game WndProc for input forwarding
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hGameWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        spdlog::info("[overlay] Initialized (WndProc=0x{:X})", (uintptr_t)g_origWndProc);
    }

    if (!g_pDevice) return OrigPresent(pSwapChain, sync, flags);

    // ── Background logic (runs every frame, even with overlay hidden) ──
    TickBackgroundLogic();

    // ── Render ImGui frame ──
    if (g_showOverlay) {
        // Create a fresh RTV from the current back buffer each frame
        ID3D10Texture2D* backBuffer = nullptr;
        HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&backBuffer);
        if (FAILED(hr) || !backBuffer)
            return OrigPresent(pSwapChain, sync, flags);

        ID3D10RenderTargetView* rtv = nullptr;
        hr = g_pDevice->CreateRenderTargetView(backBuffer, nullptr, &rtv);
        backBuffer->Release();
        if (FAILED(hr) || !rtv)
            return OrigPresent(pSwapChain, sync, flags);

        ImGui_ImplDX10_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        TickOverlayRecording();

        // Bot control panel
        ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("CoClassic Bot", &g_showOverlay)) {
            ImGui::Text("Press INSERT to toggle overlay.");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            if (ImGui::Button("Save Settings"))
                SaveConfig();
            ImGui::SameLine();
            ImGui::TextDisabled("Autosaves shortly after changes");
            ImGui::Separator();

            CHero* hero = Game::GetHero();
            if (!hero) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                    "Waiting for player entity...");
                ImGui::Text("(Log in to a character first)");
            } else {
                if (ImGui::BeginTabBar("##tabs")) {
                // ── Player tab ──
                if (ImGui::BeginTabItem("Player")) {
                    RenderPlayerTab(hero);
                    ImGui::EndTabItem();
                }

                // ── Map tab (minimap + travel + entity table) ──
                if (ImGui::BeginTabItem("Map")) {
                    CGameMap* map = Game::GetMap();
                    // Session 9: entity source is the heap scan — see entities.h.
                    const std::vector<CRole*>& entityList = Entities::Get();
                    // Session 12: fetched once here and reused by both the minimap and
                    // the table view below, instead of each doing its own independent
                    // MapItems::Get() heap-scan copy + IsAlive() pass over the same items.
                    const std::vector<CMapItem*>& mapItemList = MapItems::Get();
                    bool hasMgr = !entityList.empty();
                    bool hasMap = map && map->m_sizeMap.iWidth > 0;

                    MapSettings& ms = GetMapSettings();

                    // ── Map info header ──
                    OBJID curMapId = Game::GetCurrentMapId();
                    auto& gateways = GetGateways(curMapId);
                    RenderMapOverviewSection(hero, map, hasMap, curMapId, gateways, ms);

                    if (auto* travel = PluginManager::Get().GetPlugin<TravelPlugin>()) {
                        if (ImGui::CollapsingHeader("Travel", ImGuiTreeNodeFlags_DefaultOpen)) {
                            travel->RenderUI();
                        }
                    }

                    RenderMinimapSection(hero, map, entityList, mapItemList, hasMgr, hasMap, ms, gateways);
                    RenderEntitiesTableSection(hero, entityList, mapItemList, hasMgr, hasMap);
                    ImGui::EndTabItem();
                }

                // ── Packets tab ──
                if (ImGui::BeginTabItem("Packets")) {
                    RenderPacketsTab();
                    ImGui::EndTabItem();
                }

                // ── Misc tab ──
                if (ImGui::BeginTabItem("Misc")) {
                    RenderMiscDiscordWebhookSection(hero);
                    RenderMiscWhisperNotificationsSection(hero);
                    RenderMiscLoggingDiagnosticsSection(hero);
                    RenderMiscNativePickupTestSection(hero);
                    RenderMiscNativeJumpTestSection(hero);
                    RenderMiscNativeWalkTestSection(hero);
                    RenderMiscMapProbeSection(hero);
                    RenderMiscMonsterStatScanSection(hero);
                    RenderMiscCombatTestSection(hero);
                    RenderMiscLootDropNotificationsSection(hero);
                    RenderMiscItemNotificationsSection(hero);

                    ImGui::EndTabItem();
                }

                // ── Plugin tabs ──
                PluginManager::Get().RenderAllUI();

                ImGui::EndTabBar();
            }
            } // else (hero valid)
        }
        ImGui::End();

        // ── Render ──
        ImGui::Render();
        g_pDevice->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
        rtv->Release();
    }

    return OrigPresent(pSwapChain, sync, flags);
}

// =====================================================================
// Public API
// =====================================================================
void InitOverlay()
{
    uintptr_t presentAddr = FindPresentAddress();
    if (!presentAddr) {
        spdlog::error("[overlay] Failed to find Present address");
        return;
    }

    OrigPresent = (PresentFn)presentAddr;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)OrigPresent, HkPresent);
    LONG err = DetourTransactionCommit();

    spdlog::info("[overlay] Present hook @ 0x{:X}: {}", presentAddr, err == NO_ERROR ? "OK" : "FAILED");
}

void ShutdownOverlay()
{
    // Restore WndProc
    if (g_origWndProc && g_hGameWnd) {
        SetWindowLongPtrW(g_hGameWnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_origWndProc = nullptr;
    }

    // Unhook Present
    if (OrigPresent) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)OrigPresent, HkPresent);
        DetourTransactionCommit();
        OrigPresent = nullptr;
    }

    // Shutdown ImGui
    if (g_initialized) {
        ImGui_ImplDX10_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_initialized = false;
    }

    spdlog::info("[overlay] Shutdown complete");
}

bool IsOverlayVisible()
{
    return g_showOverlay;
}
