#pragma once
#include "../module.h"
#include <imgui.h>
#include <random>
#include <chrono>

// =================================================================
// Click Assist
// =================================================================
// Legit autoclicker that adds extra clicks between your real
// clicks, boosting CPS while maintaining natural patterns.
//
// Anti-detection features:
//   - Gaussian CPS distribution (not flat random)
//   - Click timing jitter
//   - Spike/drop patterns (humans aren't consistent)
//   - Only clicks when LMB is held
//   - Break patterns (occasional pauses)
// =================================================================

class ClickAssist : public Module {
private:
    float m_minCPS = 10.0f;
    float m_maxCPS = 14.0f;
    bool m_onlyInFight = true;      // Only while LMB held
    bool m_breakPatterns = true;     // Occasional CPS drops
    float m_breakChance = 5.0f;      // % chance per second
    int m_breakDuration = 3;         // Ticks of low CPS
    bool m_jitter = true;            // Random delay variation
    float m_jitterAmount = 15.0f;    // % of delay to randomize

    // Internal
    std::chrono::steady_clock::time_point m_lastClick;
    bool m_inBreak = false;
    int m_breakCounter = 0;
    std::mt19937 m_rng{ std::random_device{}() };

    long long GetNextDelay() {
        float cps;
        if (m_inBreak) {
            // During break: lower CPS
            cps = m_minCPS * 0.6f;
        } else {
            // Gaussian distribution centered between min and max
            float mean = (m_minCPS + m_maxCPS) / 2.0f;
            float stddev = (m_maxCPS - m_minCPS) / 4.0f;
            std::normal_distribution<float> dist(mean, stddev);
            cps = dist(m_rng);
            cps = std::max(m_minCPS, std::min(m_maxCPS, cps));
        }

        long long delayMs = (long long)(1000.0f / cps);

        // Jitter
        if (m_jitter) {
            float jitterRange = delayMs * (m_jitterAmount / 100.0f);
            std::uniform_real_distribution<float> jd(-jitterRange, jitterRange);
            delayMs += (long long)jd(m_rng);
        }

        return std::max(20LL, delayMs); // Minimum 20ms
    }

public:
    ClickAssist() : Module("Click Assist", "Boost CPS with natural click patterns",
                           ModuleCategory::COMBAT, 0) {
        m_lastClick = std::chrono::steady_clock::now();
    }

    void OnTick(JNIEnv* env) override {
        // Only when LMB is held
        if (m_onlyInFight && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastClick).count();

        // Break pattern check
        if (m_breakPatterns) {
            if (m_inBreak) {
                m_breakCounter--;
                if (m_breakCounter <= 0) m_inBreak = false;
            } else {
                std::uniform_real_distribution<float> d(0.f, 100.f);
                if (d(m_rng) < m_breakChance * 0.05f) { // per-tick chance
                    m_inBreak = true;
                    m_breakCounter = m_breakDuration;
                }
            }
        }

        long long delay = GetNextDelay();

        if (elapsed >= delay) {
            // Send click via SendInput
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, inputs, sizeof(INPUT));

            m_lastClick = now;
        }
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Min CPS", &m_minCPS, 1.f, 20.f, "%.0f");
        ImGui::SliderFloat("Max CPS", &m_maxCPS, 1.f, 25.f, "%.0f");
        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;

        ImGui::Checkbox("Only While Clicking", &m_onlyInFight);
        ImGui::Checkbox("Jitter", &m_jitter);
        if (m_jitter) {
            ImGui::SliderFloat("Jitter Amount", &m_jitterAmount, 5.f, 40.f, "%.0f%%");
        }
        ImGui::Checkbox("Break Patterns", &m_breakPatterns);
        if (m_breakPatterns) {
            ImGui::SliderFloat("Break Chance", &m_breakChance, 1.f, 20.f, "%.0f%%");
            ImGui::SliderInt("Break Length", &m_breakDuration, 1, 10);
        }
    }
};
