#pragma once
#include "../module.h"
#include <imgui.h>
#include <random>
#include <chrono>
#include <deque>
#include <cmath>

// =================================================================
// Click Assist
// =================================================================
// 20 CPS is not the problem. Plenty of humans butterfly at 18-22.
// What flags is the SHAPE of the click stream.
//
// Anticheats fingerprint clicking with:
//   1. Standard deviation of intervals. Too low = machine.
//   2. Kurtosis / outlier count. Humans produce stray long gaps.
//   3. Double-click ratio. Butterfly makes tight PAIRS, not an
//      even stream. A flat 50ms cadence at 20 CPS is impossible
//      for a human hand.
//   4. Drift over time. Real hands fatigue; CPS sags after a few
//      seconds of sustained clicking.
//   5. Sub-20ms intervals. Physically unreachable, instant flag.
//
// PATTERNS:
//   0 Normal    — single stream, gaussian intervals. Cap ~14 CPS
//   1 Butterfly — two-finger pairs. The only honest way past 16
//   2 Drag      — dense bursts with longer recovery gaps
//   3 Jitter    — high variance single stream, mid CPS
// =================================================================

class ClickAssist : public Module {
private:
    // ---- Core ----
    int   m_pattern       = 1;        // Butterfly
    float m_minCPS        = 15.0f;
    float m_maxCPS        = 20.0f;
    bool  m_onlyInFight   = true;

    // ---- Butterfly ----
    int   m_pairGapMin    = 22;       // ms between the two clicks of a pair
    int   m_pairGapMax    = 38;
    int   m_restGapMin    = 62;       // ms between pairs
    int   m_restGapMax    = 98;
    float m_pairSkipChance = 6.0f;    // Sometimes a finger misses

    // ---- Drag ----
    int   m_burstLenMin   = 3;
    int   m_burstLenMax   = 7;
    int   m_burstGapMin   = 90;
    int   m_burstGapMax   = 170;

    // ---- Humanization ----
    bool  m_jitter        = true;
    float m_jitterAmount  = 26.0f;
    bool  m_fatigue       = true;     // CPS sags during long holds
    float m_fatigueRate   = 12.0f;    // % drop over the fatigue window
    int   m_fatigueAfterMs = 2600;    // When the sag begins
    bool  m_outliers      = true;     // Inject rare long gaps
    float m_outlierChance = 4.0f;
    int   m_outlierAddMin = 40;
    int   m_outlierAddMax = 120;

    // ---- Safety ----
    int   m_hardFloorMs   = 24;       // Never click faster than this
    bool  m_entropyGuard  = true;     // Force-inject variance if too flat
    float m_minStdDev     = 9.0f;     // Target ms std-dev floor
    bool  m_breakPatterns = true;
    float m_breakChance   = 8.0f;
    int   m_breakDuration = 5;

    // ---- Internal ----
    std::chrono::steady_clock::time_point m_lastClick;
    std::chrono::steady_clock::time_point m_holdStart;
    bool  m_wasHolding    = false;
    bool  m_inPair        = false;    // Second click of a butterfly pair pending
    int   m_burstLeft     = 0;
    bool  m_inBreak       = false;
    int   m_breakCounter  = 0;
    long long m_nextDelay = 60;
    std::deque<long long> m_history;  // Recent intervals for entropy check

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

    float CurrentStdDev() const {
        if (m_history.size() < 6) return 999.f;
        double mean = 0;
        for (auto v : m_history) mean += v;
        mean /= m_history.size();
        double var = 0;
        for (auto v : m_history) { double d = v - mean; var += d * d; }
        var /= m_history.size();
        return (float)std::sqrt(var);
    }

    float FatigueFactor() {
        if (!m_fatigue || !m_wasHolding) return 1.0f;
        auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_holdStart).count();
        if (held < m_fatigueAfterMs) return 1.0f;
        // Ramp the sag in over ~4s past the threshold, then hold
        float over = (float)(held - m_fatigueAfterMs) / 4000.f;
        if (over > 1.f) over = 1.f;
        return 1.0f - (m_fatigueRate / 100.f) * over;
    }

    long long ApplyNoise(long long delay) {
        if (m_jitter && m_jitterAmount > 0.f) {
            float range = delay * (m_jitterAmount / 100.f);
            std::uniform_real_distribution<float> d(-range, range);
            delay += (long long)d(m_rng);
        }
        if (m_outliers && Roll(m_outlierChance)) {
            delay += Rand(m_outlierAddMin, m_outlierAddMax);
        }
        // Entropy guard: if the recent stream is too regular, widen it
        if (m_entropyGuard && CurrentStdDev() < m_minStdDev) {
            std::uniform_real_distribution<float> d(-m_minStdDev * 1.6f,
                                                     m_minStdDev * 1.6f);
            delay += (long long)d(m_rng);
        }
        if (delay < m_hardFloorMs) delay = m_hardFloorMs;
        return delay;
    }

    long long ComputeNextDelay() {
        float fatigue = FatigueFactor();
        long long delay;

        if (m_inBreak) {
            delay = (long long)(1000.f / (m_minCPS * 0.55f));
            return ApplyNoise(delay);
        }

        switch (m_pattern) {
            case 1: { // Butterfly: tight pair, then a rest
                if (m_inPair) {
                    delay = Rand(m_pairGapMin, m_pairGapMax);
                    m_inPair = false;
                } else {
                    delay = Rand(m_restGapMin, m_restGapMax);
                    m_inPair = !Roll(m_pairSkipChance);
                }
                delay = (long long)(delay / fatigue);
                break;
            }
            case 2: { // Drag: dense burst, longer recovery
                if (m_burstLeft > 0) {
                    m_burstLeft--;
                    delay = (long long)(1000.f / m_maxCPS);
                } else {
                    m_burstLeft = Rand(m_burstLenMin, m_burstLenMax);
                    delay = Rand(m_burstGapMin, m_burstGapMax);
                }
                delay = (long long)(delay / fatigue);
                break;
            }
            case 3: { // Jitter: wide gaussian, single stream
                float mean = (m_minCPS + m_maxCPS) * 0.5f * fatigue;
                float sd   = (m_maxCPS - m_minCPS) * 0.42f;
                std::normal_distribution<float> d(mean, sd);
                float cps = d(m_rng);
                cps = std::max(1.f, cps);
                delay = (long long)(1000.f / cps);
                break;
            }
            default: { // Normal
                float mean = (m_minCPS + m_maxCPS) * 0.5f * fatigue;
                float sd   = (m_maxCPS - m_minCPS) * 0.25f;
                std::normal_distribution<float> d(mean, sd);
                float cps = d(m_rng);
                cps = std::max(m_minCPS * 0.6f, std::min(m_maxCPS, cps));
                delay = (long long)(1000.f / cps);
                break;
            }
        }

        return ApplyNoise(delay);
    }

    void Click() {
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in, sizeof(INPUT));
    }

public:
    ClickAssist() : Module("Click Assist", "High-CPS clicking with human timing shape",
                           ModuleCategory::COMBAT, 0) {
        m_lastClick = std::chrono::steady_clock::now();
        m_holdStart = m_lastClick;
    }

    void OnTick(JNIEnv* env) override {
        bool holding = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        if (m_onlyInFight && !holding) {
            m_wasHolding = false;
            m_inPair = false;
            m_burstLeft = 0;
            m_history.clear();
            return;
        }

        if (holding && !m_wasHolding) {
            m_holdStart = std::chrono::steady_clock::now();
            m_wasHolding = true;
            m_history.clear();
        }

        // Break windows: humans stop mid-fight
        if (m_breakPatterns) {
            if (m_inBreak) {
                if (--m_breakCounter <= 0) m_inBreak = false;
            } else if (Roll(m_breakChance * 0.05f)) {
                m_inBreak = true;
                m_breakCounter = m_breakDuration;
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastClick).count();

        if (elapsed >= m_nextDelay) {
            Click();
            m_history.push_back(elapsed);
            if (m_history.size() > 24) m_history.pop_front();
            m_lastClick = now;
            m_nextDelay = ComputeNextDelay();
        }
    }

    void RenderSettings() override {
        const char* pats[] = { "Normal", "Butterfly", "Drag", "Jitter" };
        ImGui::Combo("Pattern", &m_pattern, pats, 4);

        if (m_pattern == 0 && m_maxCPS > 15.f) {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                "! Normal pattern above 15 CPS is not humanly reachable");
        }

        ImGui::Separator();
        ImGui::SliderFloat("Min CPS", &m_minCPS, 1.f, 24.f, "%.0f");
        ImGui::SliderFloat("Max CPS", &m_maxCPS, 1.f, 26.f, "%.0f");
        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;
        ImGui::Checkbox("Only While Clicking", &m_onlyInFight);

        if (m_pattern == 1) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Butterfly shape");
            ImGui::SliderInt("Pair Gap Min (ms)", &m_pairGapMin, 18, 60);
            ImGui::SliderInt("Pair Gap Max (ms)", &m_pairGapMax, 18, 80);
            if (m_pairGapMin > m_pairGapMax) m_pairGapMin = m_pairGapMax;
            ImGui::SliderInt("Rest Gap Min (ms)", &m_restGapMin, 30, 160);
            ImGui::SliderInt("Rest Gap Max (ms)", &m_restGapMax, 30, 220);
            if (m_restGapMin > m_restGapMax) m_restGapMin = m_restGapMax;
            ImGui::SliderFloat("Pair Skip Chance", &m_pairSkipChance, 0.f, 20.f, "%.0f%%");

            float est = 2000.f /
                ((m_pairGapMin + m_pairGapMax) * 0.5f + (m_restGapMin + m_restGapMax) * 0.5f);
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f),
                "Estimated actual CPS: %.1f", est);
        }

        if (m_pattern == 2) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Drag shape");
            ImGui::SliderInt("Burst Len Min", &m_burstLenMin, 2, 10);
            ImGui::SliderInt("Burst Len Max", &m_burstLenMax, 2, 14);
            if (m_burstLenMin > m_burstLenMax) m_burstLenMin = m_burstLenMax;
            ImGui::SliderInt("Burst Gap Min (ms)", &m_burstGapMin, 50, 250);
            ImGui::SliderInt("Burst Gap Max (ms)", &m_burstGapMax, 50, 350);
            if (m_burstGapMin > m_burstGapMax) m_burstGapMin = m_burstGapMax;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Humanization");
        ImGui::Checkbox("Jitter", &m_jitter);
        if (m_jitter)
            ImGui::SliderFloat("Jitter Amount", &m_jitterAmount, 5.f, 50.f, "%.0f%%");
        ImGui::Checkbox("Fatigue Drift", &m_fatigue);
        if (m_fatigue) {
            ImGui::SliderFloat("Fatigue Rate", &m_fatigueRate, 3.f, 30.f, "%.0f%%");
            ImGui::SliderInt("Fatigue After (ms)", &m_fatigueAfterMs, 800, 6000);
        }
        ImGui::Checkbox("Outlier Gaps", &m_outliers);
        if (m_outliers) {
            ImGui::SliderFloat("Outlier Chance", &m_outlierChance, 1.f, 15.f, "%.0f%%");
            ImGui::SliderInt("Outlier Add Min", &m_outlierAddMin, 20, 150);
            ImGui::SliderInt("Outlier Add Max", &m_outlierAddMax, 20, 300);
            if (m_outlierAddMin > m_outlierAddMax) m_outlierAddMin = m_outlierAddMax;
        }
        ImGui::Checkbox("Break Patterns", &m_breakPatterns);
        if (m_breakPatterns) {
            ImGui::SliderFloat("Break Chance", &m_breakChance, 1.f, 25.f, "%.0f%%");
            ImGui::SliderInt("Break Length", &m_breakDuration, 1, 12);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Entropy guard");
        ImGui::Checkbox("Enforce Variance", &m_entropyGuard);
        if (m_entropyGuard)
            ImGui::SliderFloat("Min Std Dev (ms)", &m_minStdDev, 3.f, 25.f, "%.0f");
        ImGui::SliderInt("Hard Floor (ms)", &m_hardFloorMs, 18, 50);

        float sd = CurrentStdDev();
        if (sd < 900.f) {
            ImVec4 c = sd < m_minStdDev
                ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                : ImVec4(0.4f, 1.f, 0.6f, 1.f);
            ImGui::TextColored(c, "Live std-dev: %.1f ms", sd);
        }
    }
};
