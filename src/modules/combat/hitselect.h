#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../input/click_scheduler.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Hit Select
// =================================================================
// Fires a click on the tick you take knockback. Landing your own
// hit on the same tick you receive one makes the server weigh the
// two against each other and shaves part of the push off. Strong
// players do this by feel; this does it by counting hurtTime.
//
// hurtTime counts DOWN from 10 after a hit, so:
//   10  the tick the damage arrived
//    9  one tick later
//    8  two ticks later, and so on
//
// The useful window is the first two or three ticks. Later than
// that and the knockback has already been applied and resolved.
//
// WHY IT GOES THROUGH THE SCHEDULER
// The autoclicker is usually running too. Two sources calling
// SendInput independently can land microseconds apart, and a
// sub-20ms interval is the one thing no hand can produce. Every
// click in the client leaves through ClickScheduler, which holds a
// shared floor and drops anything that would break it.
//
// WHAT CHANGED
// The old version required the physical mouse button to be held,
// which meant it never fired for anyone who lets go between
// swings. It now keys off whether you are actually in a fight,
// and refuses to fire when nothing is in reach, because a click
// into empty air on the exact tick you take damage is a pattern
// rather than a technique.
// =================================================================

class HitSelect : public Module {
private:
    // ---- Window ----
    int  m_windowStart = 10;   // fire from this hurtTime
    int  m_windowEnd   = 8;    // down to this one
    float m_chance     = 85.0f;

    // ---- Gating ----
    bool  m_requireTarget = true;
    float m_targetRange   = 3.5f;
    bool  m_requireCombat = true;   // must be an actual exchange
    bool  m_respectRhythm = true;   // do not outpace your own clicking
    int   m_minGapMs      = 90;

    // ---- State ----
    int  m_lastHurtTime = 0;
    bool m_firedThisHit = false;

    // Readout
    int m_fired   = 0;
    int m_dropped = 0;
    int m_skipped = 0;
    const char* m_why = "idle";

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

public:
    HitSelect() : Module("Hit Select", "Click on the tick you take knockback",
                         ModuleCategory::COMBAT, 0)
    {
        Bind("Window Start", &m_windowStart);
        Bind("Window End", &m_windowEnd);
        Bind("Chance", &m_chance);
        Bind("Require Target", &m_requireTarget);
        Bind("Target Range", &m_targetRange);
        Bind("Require Combat", &m_requireCombat);
        Bind("Respect Rhythm", &m_respectRhythm);
        Bind("Min Gap", &m_minGapMs);
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) { m_why = "menu"; return; }

        int hurtTime = CombatState::HurtTime();

        // A fresh hit arrived: the window reopens
        if (hurtTime > m_lastHurtTime) m_firedThisHit = false;
        m_lastHurtTime = hurtTime;

        // Outside the damage window there is nothing to cancel
        if (hurtTime > m_windowStart || hurtTime < m_windowEnd) {
            m_why = hurtTime == 0 ? "not hurt" : "outside window";
            return;
        }

        // One click per hit. Spraying across the whole window is a
        // burst of clicks locked to incoming damage, which is a far
        // louder signal than the technique is worth.
        if (m_firedThisHit) { m_why = "already fired"; return; }

        if (m_requireCombat && !CombatState::InCombat()) {
            m_why = "not fighting";
            return;
        }

        // Swinging at nothing on the exact tick you take damage is
        // not something a player does.
        if (m_requireTarget) {
            if (!EntityList::Init(env)) return;
            if (EntityList::GetPlayers(env, m_targetRange).empty()) {
                m_why = "nothing in reach";
                return;
            }
        }

        // Do not fire faster than you were already clicking. An
        // interval that only ever appears right after damage is its
        // own fingerprint.
        if (m_respectRhythm) {
            long long since = CombatState::MsSinceSwing();
            if (since < m_minGapMs) {
                m_skipped++;
                m_why = "too soon after your last swing";
                return;
            }
        }

        if (!Roll(m_chance)) { m_why = "missed one"; return; }

        if (ClickScheduler::RequestClick(false)) {
            m_fired++;
            m_firedThisHit = true;
            m_why = "fired";
        } else {
            m_dropped++;
            m_why = "blocked by click floor";
        }
    }

    void OnDisable(JNIEnv*) override {
        m_firedThisHit = false;
        m_lastHurtTime = 0;
    }

    void RenderSettings() override {
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.6f, 1.f), "%s", m_why);
        ImGui::TextDisabled("Fired %d | dropped %d | skipped %d",
            m_fired, m_dropped, m_skipped);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Window");
        ImGui::SliderInt("Window Start", &m_windowStart, 1, 10);
        ImGui::SliderInt("Window End", &m_windowEnd, 1, 10);
        if (m_windowEnd > m_windowStart) m_windowEnd = m_windowStart;
        ImGui::TextDisabled("hurtTime counts down from 10. Ticks %d to %d.",
            m_windowStart, m_windowEnd);
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Gating");
        ImGui::Checkbox("Require Target", &m_requireTarget);
        if (m_requireTarget)
            ImGui::SliderFloat("Target Range", &m_targetRange, 2.f, 6.f, "%.1f");
        ImGui::Checkbox("Require Combat", &m_requireCombat);
        ImGui::Checkbox("Respect Your Rhythm", &m_respectRhythm);
        if (m_respectRhythm) {
            ImGui::SliderInt("Min Gap (ms)", &m_minGapMs, 40, 200);
            ImGui::TextDisabled("Never clicks sooner than this after your own swing.");
        }

        if (m_dropped > m_fired && m_fired > 4) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Mostly dropped: the autoclicker is already saturating the rate");
        }
        if (!CombatState::IsUsable()) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Swing detection unresolved: rhythm check is blind");
        }
    }
};
