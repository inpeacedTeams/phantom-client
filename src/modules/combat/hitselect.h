#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Hit Select
// =================================================================
// Fires your click on the exact tick you receive knockback.
// Landing a hit on the same tick you take one makes the server
// apply your attacker's knockback against your own outgoing swing,
// shaving off part of the push. Strong players do this by feel.
//
// This is pure click timing, so there is nothing for a server-side
// anticheat to see beyond a well-timed player.
// =================================================================

class HitSelect : public Module {
private:
    int   m_hurtTimeTarget = 9;     // 10 = hit tick, 9 = one tick later
    float m_chance         = 90.0f;
    bool  m_requireTarget  = true;  // Only fire with someone in reach
    float m_targetRange    = 3.6f;

    int m_lastHurtTime = 0;
    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

public:
    HitSelect() : Module("Hit Select", "Click on the tick you take knockback",
                         ModuleCategory::COMBAT, 0) {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        int hurtTime = Minecraft::GetHurtTime(env, player);
        bool onTargetTick = (hurtTime == m_hurtTimeTarget && m_lastHurtTime != m_hurtTimeTarget);
        m_lastHurtTime = hurtTime;

        if (!onTargetTick) return;
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

        if (m_requireTarget) {
            if (!EntityList::Init(env)) return;
            auto ents = EntityList::GetPlayers(env, m_targetRange);
            if (ents.empty()) return;
        }

        if (!Roll(m_chance)) return;

        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in, sizeof(INPUT));
    }

    void RenderSettings() override {
        ImGui::SliderInt("HurtTime Target", &m_hurtTimeTarget, 1, 10);
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::Checkbox("Require Target", &m_requireTarget);
        if (m_requireTarget)
            ImGui::SliderFloat("Target Range", &m_targetRange, 2.f, 6.f, "%.1f");
        ImGui::Separator();
        ImGui::TextWrapped("Clicks the exact tick you take KB, cancelling part of it.");
    }
};
