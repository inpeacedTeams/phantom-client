#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <deque>
#include <chrono>
#include <random>

// =================================================================
// Backtrack — packet-delay reach extension
// =================================================================
// Holds incoming entity-position packets so enemies render at
// their PAST position. You hit where they WERE; the server's lag
// compensation still counts it. Reach value is never touched.
//
// WHY AGC/KARHU CATCHES NAIVE BACKTRACK:
//   1. Constant delay. A fixed 100ms hold produces a packet
//      arrival histogram with zero variance. Real jitter is noisy.
//   2. Delay that never stops. Humans don't have 100ms of lag
//      only during fights and 0ms while walking.
//   3. Delay that survives a lagback. If the server corrects you
//      and you keep buffering, the desync compounds into a flag.
//   4. Hit registration outside the compensation window.
//      Karhu rejects hits older than its configured window.
//
// HOW THIS BUILD AVOIDS IT:
//   - Per-packet random delay inside a band (not a constant)
//   - Pulse mode: buffer in short bursts, flush between them
//   - Range window: only active in the 2.5-4.5 block band where
//     backtrack actually helps. Outside it, packets pass through
//   - Ping-aware cap: total (ping + delay) stays under the window
//   - Instant flush on damage taken, on flag, and on target swap
//   - Smooth ramp: delay grows over several ticks instead of
//     snapping from 0 to max
// =================================================================

struct DelayedPacket {
    void* packetRef;                                  // jobject global ref
    std::chrono::steady_clock::time_point arrivedAt;
    long long releaseAfterMs;
};

class Backtrack : public Module {
private:
    // ---- Core ----
    int   m_mode            = 1;      // 0=Constant, 1=Pulse, 2=Adaptive
    int   m_delayMinMs      = 40;     // Lower bound of the random band
    int   m_delayMaxMs      = 90;     // Upper bound
    int   m_hardCapMs       = 140;    // Never exceed, regardless of mode

    // ---- Range window ----
    bool  m_useRangeWindow  = true;
    float m_rangeMin        = 2.4f;   // Below this, backtrack hurts you
    float m_rangeMax        = 4.6f;   // Above this, it does nothing useful

    // ---- Pulse mode ----
    int   m_pulseOnMin      = 6;      // Ticks of buffering
    int   m_pulseOnMax      = 14;
    int   m_pulseOffMin     = 8;      // Ticks of pass-through
    int   m_pulseOffMax     = 20;

    // ---- Ping awareness ----
    bool  m_pingAware       = true;
    int   m_compensationMs  = 200;    // Server lag-comp window estimate
    int   m_safetyMarginMs  = 40;     // Stay this far under the window

    // ---- Safety ----
    bool  m_flushOnDamage   = true;   // Dump buffer the tick you get hit
    bool  m_flushOnTargetSwap = true;
    int   m_pauseAfterFlagTicks = 40; // Freeze after a lagback
    bool  m_onlyWhileClicking = true; // No buffering while walking around
    int   m_rampTicks       = 4;      // Ticks to ramp delay up from zero

    // ---- Randomization ----
    float m_perPacketJitter = 22.0f;  // % variance applied per packet

    // ---- Internal ----
    std::deque<DelayedPacket> m_queue;
    bool  m_buffering       = false;
    int   m_pulseCounter    = 0;
    int   m_pulseTarget     = 0;
    int   m_pauseCounter    = 0;
    int   m_rampCounter     = 0;
    int   m_lastHurtTime    = 0;
    int   m_currentDelayMs  = 0;
    int   m_measuredPing    = 30;

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    // Effective ceiling given ping and the server's comp window
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

        // Per-packet jitter so the arrival histogram stays noisy
        if (m_perPacketJitter > 0.f) {
            float range = base * (m_perPacketJitter / 100.f);
            std::uniform_real_distribution<float> jd(-range, range);
            base += (int)jd(m_rng);
        }

        // Ramp: first few ticks after enabling use a fraction of the delay
        if (m_rampCounter < m_rampTicks && m_rampTicks > 0) {
            float t = (float)m_rampCounter / (float)m_rampTicks;
            base = (int)(base * t);
        }

        int cap = EffectiveCap();
        if (base > cap) base = cap;
        if (base < 0)   base = 0;
        return base;
    }

public:
    Backtrack() : Module("Backtrack", "Hit players at their past positions",
                         ModuleCategory::COMBAT, 0) {}

    void OnEnable(JNIEnv* env) override {
        m_queue.clear();
        m_rampCounter = 0;
        m_pulseCounter = 0;
        m_pulseTarget = Rand(m_pulseOnMin, m_pulseOnMax);
        m_buffering = true;
        m_pauseCounter = 0;
    }

    void OnDisable(JNIEnv* env) override {
        FlushAll();
    }

    // Called by the network hook for every entity-position packet.
    // Return true to hold the packet, false to let it through now.
    bool ShouldHoldPacket(JNIEnv* env, double entX, double entY, double entZ) {
        if (!m_enabled) return false;
        if (m_pauseCounter > 0) return false;
        if (!m_buffering) return false;

        if (m_onlyWhileClicking && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return false;

        if (m_useRangeWindow) {
            jobject player = Minecraft::GetPlayer(env);
            if (!player) return false;
            double dx = entX - Minecraft::GetPosX(env, player);
            double dy = entY - Minecraft::GetPosY(env, player);
            double dz = entZ - Minecraft::GetPosZ(env, player);
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < m_rangeMin || dist > m_rangeMax) return false;
        }

        m_currentDelayMs = RollDelay();
        return m_currentDelayMs > 0;
    }

    int GetRolledDelay() const { return m_currentDelayMs; }

    void FlushAll() {
        // The network hook drains and processes everything immediately
        m_queue.clear();
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        // --- Pause after a server correction ---
        if (m_pauseCounter > 0) {
            m_pauseCounter--;
            if (m_pauseCounter == 0) m_rampCounter = 0; // Re-ramp after pause
            return;
        }

        // --- Flush on damage ---
        if (m_flushOnDamage) {
            int hurtTime = Minecraft::GetHurtTime(env, player);
            if (hurtTime > 0 && m_lastHurtTime == 0) {
                FlushAll();
            }
            m_lastHurtTime = hurtTime;
        }

        // --- Pulse cycling ---
        if (m_mode == 1) {
            m_pulseCounter++;
            if (m_pulseCounter >= m_pulseTarget) {
                m_buffering = !m_buffering;
                m_pulseCounter = 0;
                m_pulseTarget = m_buffering
                    ? Rand(m_pulseOnMin, m_pulseOnMax)
                    : Rand(m_pulseOffMin, m_pulseOffMax);
                if (!m_buffering) FlushAll();
                else m_rampCounter = 0;
            }
        } else {
            m_buffering = true;
        }

        // --- Adaptive: scale delay to target distance ---
        if (m_mode == 2) {
            EntityList::Init(env);
            auto ents = EntityList::GetPlayers(env, m_rangeMax);
            auto* t = EntityList::FindClosest(ents, m_rangeMax);
            if (t) {
                // Further target = more delay helps, closer = less
                float span = m_rangeMax - m_rangeMin;
                float pos  = (float)(t->distanceToPlayer - m_rangeMin);
                float f    = span > 0 ? (pos / span) : 0.5f;
                f = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
                m_delayMaxMs = m_delayMinMs +
                    (int)((EffectiveCap() - m_delayMinMs) * f);
            }
        }
    }

    // Call from the movement-correction hook (S08PacketPlayerPosLook)
    void OnServerCorrection() {
        FlushAll();
        m_pauseCounter = m_pauseAfterFlagTicks;
        m_buffering = false;
    }

    void SetPing(int ping) { m_measuredPing = ping; }

    void RenderSettings() override {
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
            ImGui::Text("Effective cap: %d ms  (ping %d)", EffectiveCap(), m_measuredPing);
        }
        ImGui::Checkbox("Flush On Damage", &m_flushOnDamage);
        ImGui::Checkbox("Flush On Target Swap", &m_flushOnTargetSwap);
        ImGui::Checkbox("Only While Clicking", &m_onlyWhileClicking);
        ImGui::SliderInt("Pause After Flag", &m_pauseAfterFlagTicks, 0, 100);

        ImGui::Separator();
        ImGui::TextWrapped("Buffered packets: %d | State: %s",
            (int)m_queue.size(), m_buffering ? "BUFFERING" : "pass-through");
    }
};
