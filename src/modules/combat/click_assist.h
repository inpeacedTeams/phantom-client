#pragma once
#include "../module.h"
#include <imgui.h>
#include <Windows.h>
#include <random>
#include <chrono>
#include <deque>
#include <cmath>
#include <algorithm>

// =================================================================
// Click Assist
// =================================================================
// 20 CPS is not the problem. Plenty of humans butterfly at 18-22.
// What flags is the SHAPE of the click stream.
//
// Anticheats fingerprint clicking with:
//   1. Standard deviation of intervals. Too low means machine.
//   2. Outlier count. Humans produce stray long gaps.
//   3. Double-click ratio. Butterfly makes tight PAIRS, not an even
//      stream. A flat 50ms cadence at 20 CPS is not producible by
//      a human hand.
//   4. Drift over time. Real hands fatigue and CPS sags.
//   5. Sub-20ms intervals, which are physically unreachable.
//
// PATTERNS
//   0 Normal    single stream, gaussian intervals. Cap around 14
//   1 Butterfly two-finger pairs. The only honest way past 16
//   2 Drag      dense bursts with longer recovery gaps
//   3 Jitter    high-variance single stream, mid CPS
// =================================================================

class ClickAssist : public Module {
private:
    int   m_pattern      = 1;
    float m_minCPS       = 15.0f;
    float m_maxCPS       = 20.0f;
    bool  m_onlyInFight  = true;

    int   m_pairGapMin     = 22;
    int   m_pairGapMax     = 38;
    int   m_restGapMin     = 62;
    int   m_restGapMax     = 98;
    float m_pairSkipChance = 6.0f;

    int   m_burstLenMin = 3;
    int   m_burstLenMax = 7;
    int   m_burstGapMin = 90;
    int   m_burstGapMax = 170;

    bool  m_jitter         = true;
    float m_jitterAmount   = 26.0f;
    bool  m_fatigue        = true;
    float m_fatigueRate    = 12.0f;
    int   m_fatigueAfterMs = 2600;
    bool  m_outliers       = true;
    float m_outlierChance  = 4.0f;
    int   m_outlierAddMin  = 40;
    int   m_outlierAddMax  = 120;

    int   m_hardFloorMs   = 24;
    bool  m_entropyGuard  = true;
    float m_minStdDev     = 9.0f;
    bool  m_breakPatterns = true;
    float m_breakChance   = 8.0f;
    int   m_breakDuration = 5;

    std::chrono::steady_clock::time_point m_lastClick;
    std::chrono::steady_clock::time_point m_holdStart;
    bool m_wasHolding = false;
    bool m_inPair     = false;
    int  m_burstLeft  = 0;
    bool m_inBreak    = false;
    int  m_breakCounter = 0;
    long long m_nextDelay = 60;
    std::deque<long long> m_history;

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
        double mean = 0.0;
        for (auto v : m_history) mean += (double)v;
        mean /= (double)m_history.size();
        double var = 0.0;
        for (auto v : m_history) { double d = (double)v - mean; var += d * d; }
        var /= (double)m_history.size();
        return (float)std::sqrt(var);
    }

    float FatigueFactor() {
        if (!m_fatigue || !m_wasHolding) return 1.0f;
        auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_holdStart).count();
        if (held < m_fatigueAfterMs) return 1.0f;
        float over = (float)(held - m_fatigueAfterMs) / 4000.0f;
        if (over > 1.0f) over = 1.0f;
        return 1.0f - (m_fatigueRate / 100.0f) * over;
    }

    long long ApplyNoise(long long delay) {
        if (m_jitter && m_jitterAmount > 0.f) {
            float range = (float)delay * (m_jitterAmount / 100.0f);
            std::uniform_real_distribution<float> d(-range, range);
            delay += (long long)d(m_rng);
        }
        if (m_outliers && Roll(m_outlierChance)) {
            delay += Rand(m_outlierAddMin, m_outlierAddMax);
        }
        // If the recent stream flattened out, widen it back up.
        if (m_entropyGuard && CurrentStdDev() < m_minStdDev) {
            std::uniform_real_distribution<float> d(-m_minStdDev * 1.6f, m_minStdDev * 1.6f);
            delay += (long long)d(m_rng);
        }
        if (delay < (long long)m_hardFloorMs) delay = m_hardFloorMs;
        return delay;
    }

    long long ComputeNextDelay() {
        float fatigue = FatigueFactor();
        if (fatigue < 0.3f) fatigue = 0.3f;
        long long delay;

        if (m_inBreak) {
            float c = m_minCPS * 0.55f;
            if (c < 1.f) c = 1.f;
            return ApplyNoise((long long)(1000.0f / c));
        }

        switch (m_pattern) {
            case 1: {   // Butterfly
                if (m_inPair) {
                    delay = Rand(m_pairGapMin, m_pairGapMax);
                    m_inPair = false;
                } else {
                    delay = Rand(m_restGapMin, m_restGapMax);
                    m_inPair = !Roll(m_pairSkipChance);
                }
                delay = (long long)((float)delay / fatigue);
                break;
            }
            case 2: {   // Drag
                if (m_burstLeft > 0) {
                    m_burstLeft--;
                    float c = m_maxCPS < 1.f ? 1.f : m_maxCPS;
                    delay = (long long)(1000.0f / c);
                } else {
                    m_burstLeft = Rand(m_burstLenMin, m_burstLenMax);
                    delay = Rand(m_burstGapMin, m_burstGapMax);
                }
                delay = (long long)((float)delay / fatigue);
                break;
            }
            case 3: {   // Jitter
                float mean = (m_minCPS + m_maxCPS) * 0.5f * fatigue;
                float sd   = (m_maxCPS - m_minCPS) * 0.42f;
                if (sd < 0.1f) sd = 0.1f;
                std::normal_distribution<float> d(mean, sd);
                float cps = d(m_rng);
                if (cps < 1.f) cps = 1.f;
                delay = (long long)(1000.0f / cps);
                break;
            }
            default: {  // Normal
                float mean = (m_minCPS + m_maxCPS) * 0.5f * fatigue;
                float sd   = (m_maxCPS - m_minCPS) * 0.25f;
                if (sd < 0.1f) sd = 0.1f;
                std::normal_distribution<float> d(mean, sd);
                float cps = d(m_rng);
                cps = std::max(m_minCPS * 0.6f, std::min(m_maxCPS, cps));
                if (cps < 1.f) cps = 1.f;
                delay = (long long)(1000.0f / cps);
                break;
            }
        }

        return ApplyNoise(delay);
    }

    static void Click() {
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in, sizeof(INPUT));
    }

public:
    ClickAssist() : Module("Click Assist", "High CPS with human click timing",
                           ModuleCategory::COMBAT, 0)
    {
        m_lastClick = std::chrono::steady_clock::now();
        m_holdStart = m_lastClick;

        Bind("Pattern", &m_pattern);
        Bind("Min CPS", &m_minCPS);
        Bind("Max CPS", &m_maxCPS);
        Bind("Only While Clicking", &m_onlyInFight);
        Bind("Pair Gap Min", &m_pairGapMin);
        Bind("Pair Gap Max", &m_pairGapMax);
        Bind("Rest Gap Min", &m_restGapMin);
        Bind("Rest Gap Max", &m_restGapMax);
        Bind("Pair Skip Chance", &m_pairSkipChance);
        Bind("Burst Len Min", &m_burstLenMin);
        Bind("Burst Len Max", &m_burstLenMax);
        Bind("Burst Gap Min", &m_burstGapMin);
        Bind("Burst Gap Max", &m_burstGapMax);
        Bind("Jitter", &m_jitter);
        Bind("Jitter Amount", &m_jitterAmount);
        Bind("Fatigue", &m_fatigue);
        Bind("Fatigue Rate", &m_fatigueRate);
        Bind("Fatigue After", &m_fatigueAfterMs);
        Bind("Outliers", &m_outliers);
        Bind("Outlier Chance", &m_outlierChance);
        Bind("Hard Floor", &m_hardFloorMs);
        Bind("Entropy Guard", &m_entropyGuard);
        Bind("Min Std Dev", &m_minStdDev);
        Bind("Break Patterns", &m_breakPatterns);
        Bind("Break Chance", &m_breakChance);
        Bind("Break Duration", &m_breakDuration);
    }

    void OnTick(JNIEnv*) override {
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
                "! A flat stream above 15 CPS is not humanly reachable");
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

            float avgPair = (m_pairGapMin + m_pairGapMax) * 0.5f;
            float avgRest = (m_restGapMin + m_restGapMax) * 0.5f;
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f),
                "Estimated actual CPS: %.1f", 2000.0f / (avgPair + avgRest));
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
            ImVec4 col = (sd < m_minStdDev)
                ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                : ImVec4(0.4f, 1.f, 0.6f, 1.f);
            ImGui::TextColored(col, "Live std-dev: %.1f ms", sd);
        }
    }
};
