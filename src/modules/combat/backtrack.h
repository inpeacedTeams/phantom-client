#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <deque>
#include <chrono>
#include <random>
#include <cmath>

// =================================================================
// Backtrack
// =================================================================
// STATUS: NOT WIRED UP.
//
// Backtrack works by holding incoming entity-position packets so
// enemies render where they WERE. That requires intercepting the
// Netty pipeline, which this client does not do yet. Until that
// hook exists this module changes nothing in game.
//
// Everything below is the tuning layer, kept ready for the hook:
//   ShouldHoldPacket()   decides per packet
//   GetRolledDelay()     how long to hold it
//   OnServerCorrection() called from the S08 position-reset handler
//
// -----------------------------------------------------------------
// WHY NAIVE BACKTRACK GETS CAUGHT
//   1. Constant delay produces a packet-arrival histogram with zero
//      variance. Real network jitter is noisy.
//   2. Delay that only appears during fights. Humans do not have
//      100ms of lag exclusively while holding left click.
//   3. Delay that survives a lagback, compounding the desync.
//   4. Hits landing outside the server's lag-compensation window.
//
// WHAT THIS DESIGN DOES INSTEAD
//   - per-packet random delay inside a band, never a constant
//   - pulse mode: buffer in short bursts, pass through between them
//   - range window: only active where backtrack actually helps
//   - ping-aware cap so ping + delay stays inside the window
//   - instant flush on damage, on flag, and on target swap
//   - ramp-in so the delay grows over several ticks
// =================================================================

class Backtrack : public Module {
private:
    // Flip this once a real packet hook calls into the module.
    static constexpr bool kPacketHookAvailable = false;

    int   m_mode       = 1;    // 0=Constant 1=Pulse 2=Adaptive
    int   m_delayMinMs = 40;
    int   m_delayMaxMs = 90;
    int   m_hardCapMs  = 140;

    bool  m_useRangeWindow = true;
    float m_rangeMin = 2.4f;
    float m_rangeMax = 4.6f;

    int   m_pulseOnMin  = 6;
    int   m_pulseOnMax  = 14;
    int   m_pulseOffMin = 8;
    int   m_pulseOffMax = 20;

    bool  m_pingAware      = true;
    int   m_compensationMs = 200;
    int   m_safetyMarginMs = 40;

    bool  m_flushOnDamage     = true;
    bool  m_flushOnTargetSwap = true;
    int   m_pauseAfterFlagTicks = 40;
    bool  m_onlyWhileClicking = true;
    int   m_rampTicks = 4;

    float m_perPacketJitter = 22.0f;

    // State
    bool m_buffering   = false;
    int  m_pulseCounter = 0;
    int  m_pulseTarget  = 0;
    int  m_pauseCounter = 0;
    int  m_rampCounter  = 0;
    int  m_lastHurtTime = 0;
    int  m_currentDelayMs = 0;
    int  m_measuredPing = 30;
    int  m_heldPackets  = 0;

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    int EffectiveCap() const {
        int cap = m_hardCapMs;
        if (m_pingAware) {
            int budget = m_compensationMs - m_measuredPing - m_safetyMarginMs;
            if (budget < cap) cap = budget;
        }
        return cap < 0 ? 0 : cap;
    }

    int RollDelay() {
        int base = Rand(m_delayMinMs, m_delayMaxMs);

        if (m_perPacketJitter > 0.f) {
            float r = base * (m_perPacketJitter / 100.f);
            std::uniform_real_distribution<float> d(-r, r);
            base += (int)d(m_rng);
        }

        if (m_rampTicks > 0 && m_rampCounter < m_rampTicks) {
            base = (int)(base * ((float)m_rampCounter / (float)m_rampTicks));
        }

        int cap = EffectiveCap();
        if (base > cap) base = cap;
        if (base < 0)   base = 0;
        return base;
    }

public:
    Backtrack() : Module("Backtrack", "Hit players at their past positions",
                         ModuleCategory::COMBAT, 0) {}

    static constexpr bool IsFunctional() { return kPacketHookAvailable; }

    void OnEnable(JNIEnv*) override {
        m_rampCounter  = 0;
        m_pulseCounter = 0;
        m_pulseTarget  = Rand(m_pulseOnMin, m_pulseOnMax);
        m_buffering    = true;
        m_pauseCounter = 0;
        m_heldPackets  = 0;
    }

    void OnDisable(JNIEnv*) override { FlushAll(); }

    // ---- Called by the packet hook, once one exists ----
    bool ShouldHoldPacket(JNIEnv* env, double ex, double ey, double ez) {
        if (!kPacketHookAvailable) return false;
        if (!m_enabled || m_pauseCounter > 0 || !m_buffering) return false;

        if (m_onlyWhileClicking && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return false;

        if (m_useRangeWindow) {
            jobject player = Minecraft::GetPlayer(env);
            if (!player) return false;
            double dx = ex - Minecraft::GetPosX(env, player);
            double dy = ey - Minecraft::GetPosY(env, player);
            double dz = ez - Minecraft::GetPosZ(env, player);
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < m_rangeMin || dist > m_rangeMax) return false;
        }

        m_currentDelayMs = RollDelay();
        return m_currentDelayMs > 0;
    }

    int  GetRolledDelay() const { return m_currentDelayMs; }
    void FlushAll() { m_heldPackets = 0; }
    void SetPing(int ping) { m_measuredPing = ping; }

    void OnServerCorrection() {
        FlushAll();
        m_pauseCounter = m_pauseAfterFlagTicks;
        m_buffering = false;
    }

    void OnTick(JNIEnv* env) override {
        if (!kPacketHookAvailable) return;   // nothing to drive yet

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        if (m_pauseCounter > 0) {
            if (--m_pauseCounter == 0) m_rampCounter = 0;
            return;
        }

        if (m_flushOnDamage) {
            int hurt = Minecraft::GetHurtTime(env, player);
            if (hurt > 0 && m_lastHurtTime == 0) FlushAll();
            m_lastHurtTime = hurt;
        }

        if (m_mode == 1) {
            if (++m_pulseCounter >= m_pulseTarget) {
                m_buffering = !m_buffering;
                m_pulseCounter = 0;
                m_pulseTarget = m_buffering ? Rand(m_pulseOnMin, m_pulseOnMax)
                                            : Rand(m_pulseOffMin, m_pulseOffMax);
                if (!m_buffering) FlushAll();
                else m_rampCounter = 0;
            }
        } else {
            m_buffering = true;
        }

        if (m_mode == 2) {
            if (!EntityList::Init(env)) return;
            auto ents = EntityList::GetPlayers(env, m_rangeMax);
            auto* t = EntityList::FindClosest(ents, m_rangeMax);
            if (t) {
                float span = m_rangeMax - m_rangeMin;
                float f = span > 0.f
                    ? (float)((t->distanceToPlayer - m_rangeMin) / span) : 0.5f;
                if (f < 0.f) f = 0.f;
                if (f > 1.f) f = 1.f;
                m_delayMaxMs = m_delayMinMs + (int)((EffectiveCap() - m_delayMinMs) * f);
            }
        }
    }

    void RenderSettings() override {
        if (!kPacketHookAvailable) {
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "NOT ACTIVE");
            ImGui::TextWrapped(
                "Backtrack needs a Netty packet hook to delay entity position "
                "updates. That hook is not implemented, so enabling this module "
                "currently changes nothing in game. The settings below are the "
                "tuning layer, kept ready for when it lands.");
            ImGui::Separator();
        }

        const char* modes[] = { "Constant", "Pulse", "Adaptive" };
        ImGui::Combo("Mode", &m_mode, modes, 3);
        if (m_mode == 0) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "! Constant delay is the easiest pattern to fingerprint");
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Delay band");
        ImGui::SliderInt("Delay Min (ms)", &m_delayMinMs, 10, 200);
        ImGui::SliderInt("Delay Max (ms)", &m_delayMaxMs, 10, 250);
        if (m_delayMinMs > m_delayMaxMs) m_delayMinMs = m_delayMaxMs;
        ImGui::SliderInt("Hard Cap (ms)", &m_hardCapMs, 40, 300);
        ImGui::SliderFloat("Per-Packet Jitter", &m_perPacketJitter, 0.f, 50.f, "%.0f%%");
        ImGui::SliderInt("Ramp Ticks", &m_rampTicks, 0, 12);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Range window");
        ImGui::Checkbox("Use Range Window", &m_useRangeWindow);
        if (m_useRangeWindow) {
            ImGui::SliderFloat("Range Min", &m_rangeMin, 1.f, 4.f, "%.1f");
            ImGui::SliderFloat("Range Max", &m_rangeMax, 3.f, 8.f, "%.1f");
            if (m_rangeMin > m_rangeMax) m_rangeMin = m_rangeMax;
        }

        if (m_mode == 1) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1.f), "Pulse timing");
            ImGui::SliderInt("Buffer Ticks Min", &m_pulseOnMin, 2, 30);
            ImGui::SliderInt("Buffer Ticks Max", &m_pulseOnMax, 2, 40);
            if (m_pulseOnMin > m_pulseOnMax) m_pulseOnMin = m_pulseOnMax;
            ImGui::SliderInt("Idle Ticks Min", &m_pulseOffMin, 2, 40);
            ImGui::SliderInt("Idle Ticks Max", &m_pulseOffMax, 2, 60);
            if (m_pulseOffMin > m_pulseOffMax) m_pulseOffMin = m_pulseOffMax;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Safety");
        ImGui::Checkbox("Ping Aware", &m_pingAware);
        if (m_pingAware) {
            ImGui::SliderInt("Comp Window (ms)", &m_compensationMs, 100, 400);
            ImGui::SliderInt("Safety Margin (ms)", &m_safetyMarginMs, 0, 120);
            ImGui::Text("Effective cap: %d ms (ping %d)", EffectiveCap(), m_measuredPing);
        }
        ImGui::Checkbox("Flush On Damage", &m_flushOnDamage);
        ImGui::Checkbox("Flush On Target Swap", &m_flushOnTargetSwap);
        ImGui::Checkbox("Only While Clicking", &m_onlyWhileClicking);
        ImGui::SliderInt("Pause After Flag", &m_pauseAfterFlagTicks, 0, 100);

        ImGui::Separator();
        ImGui::TextDisabled("Held: %d | %s", m_heldPackets,
            m_buffering ? "buffering" : "pass-through");
    }
};
