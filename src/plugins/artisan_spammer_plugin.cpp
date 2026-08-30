#include "artisan_spammer_plugin.h"
#include "game.h"
#include "CHero.h"
#include "CItem.h"
#include "inventory_utils.h"
#include "packets.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <string>

// =====================================================================
// Varint writer (same encoding as packets.cpp)
// =====================================================================
static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = static_cast<uint8_t>(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = static_cast<uint8_t>(value);
    return n;
}

// =====================================================================
// Packet type 0x03F1 — Artisan compose/upgrade
//
// Field 1 (0x08): target item instance ID
// Field 2 (0x10): material item instance ID
// Field 5 (0x28): action (19 = upgrade quality via DB, 20 = improve via Meteor/Scroll)
// =====================================================================
bool ArtisanSpammerPlugin::SendArtisanPacket(OBJID targetItemId, OBJID materialItemId, uint32_t action)
{
    uint8_t buf[32] = {};
    int off = 4; // skip header

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, targetItemId);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, materialItemId);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, action);

    // Write header: [u16 size][u16 type]
    *reinterpret_cast<uint16_t*>(buf)     = static_cast<uint16_t>(off);
    *reinterpret_cast<uint16_t*>(buf + 2) = 0x03F1;

    return SendPacket(buf, off);
}

// =====================================================================
// Material matching helper
// =====================================================================
static bool IsMaterialMatch(CItem* item, int materialType)
{
    switch (materialType) {
        case 0: return item->IsMeteor();
        case 1: return item->IsMeteorScroll();
        case 2: return item->IsDragonBall();
        default: return false;
    }
}

// Action 19 = upgrade quality (DB), 20 = improve (Meteor/MeteorScroll)
static uint32_t GetArtisanAction(int materialType)
{
    return materialType == 2 ? 19 : 20;
}

// Session 14: how long to keep polling for an outcome (level-up or
// durability-drop) before giving up on THIS attempt and moving on anyway —
// mirrors hunt_town.cpp's repair-wait convention (~2.5s, a few retries)
// rather than inventing a new magnitude. The server action here is
// presumably near-instant like the rest of this game's actions, so these
// are generous, not tight.
static constexpr DWORD kArtisanWaitStepMs   = 800;
static constexpr int   kArtisanMaxWaitSteps = 3;   // ~2.4s total per outcome

// =====================================================================
// Update — drives one target item at a time through repair/upgrade
// =====================================================================
void ArtisanSpammerPlugin::Update()
{
    if (!m_spamming)
        return;

    CHero* hero = Game::GetHero();
    if (!hero) {
        m_spamming = false;
        return;
    }

    const DWORD now = GetTickCount();

    switch (m_phase) {
        case ArtisanPhase::Idle: {
            // Need a new current target item?
            if (m_currentTargetId == 0) {
                if (m_targetQueue.empty()) {
                    m_spamming = false;
                    spdlog::info("[artisan] Done — {} attempts, {} successes, {} repairs",
                        m_sentCount, m_successCount, m_repairCount);
                    return;
                }
                m_currentTargetId = m_targetQueue.back();
                m_targetQueue.pop_back();
            }

            CItem* item = FindInventoryItemById(hero, m_currentTargetId);
            if (!item) {
                // Item vanished from the bag (unexpected — moved on regardless
                // of why, rather than getting stuck on a ghost id).
                spdlog::warn("[artisan] Item {} no longer in bag, skipping", m_currentTargetId);
                m_currentTargetId = 0;
                return;
            }

            // Needs repair before another attempt can be made.
            if (item->GetDurabilityRaw() < item->GetMaxDurabilityRaw()) {
                if (!m_autoRepair) {
                    spdlog::info("[artisan] Item {} needs repair and Auto-Repair is off, skipping",
                        m_currentTargetId);
                    m_currentTargetId = 0;
                    return;
                }
                if (now - m_lastActionTick < (DWORD)m_delayMs)
                    return;
                hero->RepairItem(m_currentTargetId);
                m_lastActionTick = now;
                m_phaseStartTick = now;
                m_phaseFailCount = 0;
                m_phase = ArtisanPhase::WaitRepairResult;
                spdlog::info("[artisan] Repairing item {} (durability {}/{})",
                    m_currentTargetId, item->GetDurabilityRaw(), item->GetMaxDurabilityRaw());
                return;
            }

            // Full durability — send another material if any remain.
            if (m_materialQueue.empty()) {
                m_currentTargetId = 0;
                if (m_targetQueue.empty()) {
                    m_spamming = false;
                    spdlog::info("[artisan] Done — out of materials. {} attempts, {} successes, {} repairs",
                        m_sentCount, m_successCount, m_repairCount);
                }
                return;
            }

            if (now - m_lastActionTick < (DWORD)m_delayMs)
                return;

            m_prevPlus = item->GetPlus();
            m_prevDurabilityRaw = item->GetDurabilityRaw();
            const OBJID materialId = m_materialQueue.back();
            m_materialQueue.pop_back();

            if (SendArtisanPacket(m_currentTargetId, materialId, GetArtisanAction(m_materialType))) {
                m_sentCount++;
                spdlog::debug("[artisan] Sent #{} target={} mat={} prevPlus={} prevDur={}/{}",
                    m_sentCount, m_currentTargetId, materialId, m_prevPlus,
                    m_prevDurabilityRaw, item->GetMaxDurabilityRaw());
            }
            m_lastActionTick = now;
            m_phaseStartTick = now;
            m_phaseFailCount = 0;
            m_phase = ArtisanPhase::WaitUpgradeResult;
            return;
        }

        case ArtisanPhase::WaitUpgradeResult: {
            CItem* item = FindInventoryItemById(hero, m_currentTargetId);
            if (!item) {
                spdlog::warn("[artisan] Item {} vanished mid-attempt, moving on", m_currentTargetId);
                m_currentTargetId = 0;
                m_phase = ArtisanPhase::Idle;
                return;
            }

            const int curPlus = item->GetPlus();
            const int curDur = item->GetDurabilityRaw();

            if (curPlus > m_prevPlus) {
                m_successCount++;
                spdlog::info("[artisan] SUCCESS: item {} +{} -> +{}", m_currentTargetId, m_prevPlus, curPlus);
                m_phase = ArtisanPhase::Idle;
                // Durability stays full on success, so staying on this item
                // (m_currentTargetId left set) just re-enters Idle and sends
                // the next material immediately — unless the user wants to
                // bank this win and move to the next item instead.
                if (m_stopOnLevelChange)
                    m_currentTargetId = 0;
                return;
            }

            if (curDur < m_prevDurabilityRaw) {
                spdlog::info("[artisan] FAILED: item {} durability {} -> {}",
                    m_currentTargetId, m_prevDurabilityRaw, curDur);
                // Idle will see the reduced durability next pass and repair
                // (or skip, if Auto-Repair is off) before trying again.
                m_phase = ArtisanPhase::Idle;
                return;
            }

            // No change observed yet — keep polling up to the timeout.
            if (now - m_phaseStartTick > kArtisanWaitStepMs) {
                if (++m_phaseFailCount >= kArtisanMaxWaitSteps) {
                    spdlog::warn("[artisan] No result observed for item {} after {} polls, moving on",
                        m_currentTargetId, m_phaseFailCount);
                    m_phase = ArtisanPhase::Idle;
                    return;
                }
                m_phaseStartTick = now;
            }
            return;
        }

        case ArtisanPhase::WaitRepairResult: {
            CItem* item = FindInventoryItemById(hero, m_currentTargetId);
            if (!item) {
                spdlog::warn("[artisan] Item {} vanished mid-repair, moving on", m_currentTargetId);
                m_currentTargetId = 0;
                m_phase = ArtisanPhase::Idle;
                return;
            }

            if (item->GetDurabilityRaw() >= item->GetMaxDurabilityRaw()) {
                m_repairCount++;
                spdlog::info("[artisan] Repaired item {}", m_currentTargetId);
                m_phase = ArtisanPhase::Idle;
                return;
            }

            if (now - m_phaseStartTick > kArtisanWaitStepMs) {
                if (++m_phaseFailCount >= kArtisanMaxWaitSteps) {
                    spdlog::warn("[artisan] Repair of item {} never confirmed, skipping item",
                        m_currentTargetId);
                    m_currentTargetId = 0;
                    m_phase = ArtisanPhase::Idle;
                    return;
                }
                if (now - m_lastActionTick >= (DWORD)m_delayMs) {
                    hero->RepairItem(m_currentTargetId);
                    m_lastActionTick = now;
                }
                m_phaseStartTick = now;
            }
            return;
        }
    }
}

// =====================================================================
// UI
// =====================================================================
void ArtisanSpammerPlugin::RenderUI()
{
    CHero* hero = Game::GetHero();
    if (!hero) {
        ImGui::TextDisabled("No hero");
        return;
    }

    // -- Build unique equipment type list (grouped by name) --
    struct EquipType { std::string name; int count; };
    std::vector<EquipType> equipTypes;

    for (auto& pi : hero->m_deqItem) {
        CItem* item = pi.get();
        if (!item || !item->IsEquipment()) continue;
        const char* name = item->GetName();
        auto it = std::find_if(equipTypes.begin(), equipTypes.end(),
            [name](const EquipType& e) { return e.name == name; });
        if (it != equipTypes.end())
            it->count++;
        else
            equipTypes.push_back({ name, 1 });
    }

    // -- Target item type combo --
    if (ImGui::CollapsingHeader("Target Item", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (equipTypes.empty()) {
            ImGui::TextDisabled("No equipment in inventory");
            m_selectedTarget = -1;
        } else {
            if (m_selectedTarget >= (int)equipTypes.size())
                m_selectedTarget = -1;

            const char* preview = m_selectedTarget >= 0
                ? equipTypes[m_selectedTarget].name.c_str()
                : "-- Select --";

            if (ImGui::BeginCombo("Item##target", preview)) {
                for (int i = 0; i < (int)equipTypes.size(); i++) {
                    char label[64];
                    snprintf(label, sizeof(label), "%s (x%d)##%d",
                        equipTypes[i].name.c_str(), equipTypes[i].count, i);
                    if (ImGui::Selectable(label, m_selectedTarget == i))
                        m_selectedTarget = i;
                }
                ImGui::EndCombo();
            }

            if (m_selectedTarget >= 0)
                ImGui::Text("Items: %d", equipTypes[m_selectedTarget].count);
        }
    }

    // -- Material type --
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::RadioButton("Meteor", &m_materialType, 0);
        ImGui::SameLine();
        ImGui::RadioButton("MeteorScroll", &m_materialType, 1);
        ImGui::SameLine();
        ImGui::RadioButton("DragonBall", &m_materialType, 2);

        int matCount = 0;
        for (auto& pi : hero->m_deqItem) {
            CItem* it = pi.get();
            if (it && IsMaterialMatch(it, m_materialType))
                matCount++;
        }
        ImGui::Text("Available: %d", matCount);
    }

    // -- Behavior toggles (the two originally-missing pieces) --
    if (ImGui::CollapsingHeader("Behavior", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Auto-Repair on Failed Upgrade", &m_autoRepair);
        ImGui::TextDisabled("A failed attempt cuts durability in half. When on, repairs automatically (an NPC next to Artisan Wind handles it) before trying that item again.");
        ImGui::Checkbox("Stop Feeding an Item Once It Levels Up", &m_stopOnLevelChange);
        ImGui::TextDisabled("Off: keeps spending materials on the same item after a success, as long as materials remain. On: moves to the next item the moment this one's level increases.");
    }

    // -- Delay --
    ImGui::SliderInt("Delay (ms)", &m_delayMs, 0, 1000);

    ImGui::Separator();

    // -- Spam button --
    if (m_spamming) {
        const char* phaseText = m_phase == ArtisanPhase::WaitRepairResult ? "repairing"
            : m_phase == ArtisanPhase::WaitUpgradeResult ? "awaiting result"
            : "deciding";
        ImGui::Text("Item %d / %d (%s) — %d attempts, %d successes, %d repairs",
            m_totalTargets - (int)m_targetQueue.size(), m_totalTargets, phaseText,
            m_sentCount, m_successCount, m_repairCount);
        if (ImGui::Button("Stop")) {
            m_spamming = false;
            m_phase = ArtisanPhase::Idle;
            m_targetQueue.clear();
            m_materialQueue.clear();
            m_currentTargetId = 0;
        }
    } else {
        const bool canSpam = m_selectedTarget >= 0
                          && m_selectedTarget < (int)equipTypes.size();

        if (!canSpam) ImGui::BeginDisabled();
        if (ImGui::Button("Spam All")) {
            const std::string& targetName = equipTypes[m_selectedTarget].name;

            m_targetQueue.clear();
            for (auto& pi : hero->m_deqItem) {
                CItem* it = pi.get();
                if (it && it->IsEquipment() && it->GetName() == targetName)
                    m_targetQueue.push_back(it->GetID());
            }

            m_materialQueue.clear();
            for (auto& pi : hero->m_deqItem) {
                CItem* it = pi.get();
                if (it && IsMaterialMatch(it, m_materialType))
                    m_materialQueue.push_back(it->GetID());
            }

            if (!m_targetQueue.empty() && !m_materialQueue.empty()) {
                m_totalTargets = (int)m_targetQueue.size();
                m_sentCount = 0;
                m_successCount = 0;
                m_repairCount = 0;
                m_lastActionTick = 0;
                m_currentTargetId = 0;
                m_phase = ArtisanPhase::Idle;
                m_spamming = true;
                spdlog::info("[artisan] Queued {} items x {} materials ('{}')",
                    m_totalTargets, (int)m_materialQueue.size(), targetName);
            }
        }
        if (!canSpam) ImGui::EndDisabled();

        if (m_sentCount > 0 && !m_spamming) {
            ImGui::SameLine();
            ImGui::Text("Last run: %d sent, %d successes, %d repairs",
                m_sentCount, m_successCount, m_repairCount);
        }
    }
}
