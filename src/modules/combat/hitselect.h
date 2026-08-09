#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>

// =================================================================
// Hit Select
// =================================================================
// Sends your click on the EXACT tick you receive knockback.
// When you hit someone the same tick you take KB, the game
// partially cancels your knockback. Top players do this manually
// by timing their clicks. This module automates it.
//
// 100% legit: you're just clicking at the right moment.
// =================================================================

class HitSelect : public Module {
private:
    int m_hurtTimeTarget = 9;  // hurtTime value to click on (10 = just hit, 9 = 1 tick after)
    float m_chance = 90.0f;
    bool m_onlyInCombat = true;

    int m_lastHurtTime = 0;
    std::mt19937 m_rng{ std::random_device{}() };

public:
    HitSelect() : Module("Hit Select", "Click at the perfect tick to reduce incoming KB",
                         ModuleCategory::COMBAT, 0) {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        int hurtTime = Minecraft::GetHurtTime(env, player);

        // Detect the target hurtTime tick
        if (hurtTime == m_hurtTimeTarget && m_lastHurtTime != m_hurtTimeTarget) {
            // Check if LMB is being held (player wants to attack)
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
                std::uniform_real_distribution<float> d(0.f, 100.f);
                if (d(m_rng) < m_chance) {
                    // Simulate a click via SendInput
                    INPUT input = {};
                    input.type = INPUT_MOUSE;
                    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    SendInput(1, &input, sizeof(INPUT));

                    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &input, sizeof(INPUT));
                }
            }
        }

        m_lastHurtTime = hurtTime;
    }

    void RenderSettings() override {
        ImGui::SliderInt("HurtTime Target", &m_hurtTimeTarget, 1, 10);
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::Checkbox("Only In Combat", &m_onlyInCombat);
        ImGui::Separator();
        ImGui::TextWrapped("Clicks the exact tick you take KB to cancel part of it. Fully legit.");
    }
};
