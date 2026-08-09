#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../input/click_scheduler.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Hit Select
// =================================================================
// Fires a click on the exact tick you take knockback. Landing a hit
// on the same tick you receive one makes the server weigh your
// outgoing swing against the incoming push, shaving part of it off.
// Strong players do this by feel.
//
// Pure click timing, so there is nothing for a server-side anticheat
// to see beyond a well-timed player.
//
// The click goes out through ClickScheduler rather than SendInput.
// With the autoclicker also running, two independent sources firing
// SendInput can land microseconds apart, and a sub-20ms interval is
// the one thing no hand can produce. The scheduler holds a shared
// floor and drops this click if it would break it.
// =================================================================

class HitSelect : public Module {
private:
    int   m_hurtTimeTarget = 9;   // 10 is the hit tick, 9 is one later
    float m_chance         = 90.0f;
    bool  m_requireTarget  = true;
    float m_targetRange    = 3.6f;

    int m_lastHurtTime = 0;
    int m_fired   = 0;
    int m_dropped = 0;
    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

public:
    HitSelect() : Module("Hit Select", "Click on the tick you take knockback",
                         ModuleCategory::COMBAT, 0)
    {
        Bind("HurtTime Target", &m_hurtTimeTarget);
        Bind("Chance", &m_chance);
        Bind("Require Target", &m_requireTarget);
        Bind("Target Range", &m_targetRange);
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        int hurtTime = Minecraft::GetHurtTime(env, player);
        bool onTargetTick = (hurtTime == m_hurtTimeTarget
                          && m_lastHurtTime != m_hurtTimeTarget);
        m_lastHurtTime = hurtTime;

        if (!onTargetTick) return;
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

        if (m_requireTarget) {
            if (!EntityList::Init(env)) return;
            if (EntityList::GetPlayers(env, m_targetRange).empty()) return;
        }

        if (!Roll(m_chance)) return;

        if (ClickScheduler::RequestClick(false)) m_fired++;
        else                                     m_dropped++;
    }

    void RenderSettings() override {
        ImGui::SliderInt("HurtTime Target", &m_hurtTimeTarget, 1, 10);
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::Checkbox("Require Target", &m_requireTarget);
        if (m_requireTarget)
            ImGui::SliderFloat("Target Range", &m_targetRange, 2.f, 6.f, "%.1f");

        ImGui::Separator();
        ImGui::TextWrapped("Clicks the exact tick you take KB, cancelling part of it.");
        ImGui::TextDisabled("Fired %d | dropped by floor %d", m_fired, m_dropped);
        if (m_dropped > m_fired && m_fired > 4) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Mostly dropped: the autoclicker is already saturating the rate");
        }
    }
};
