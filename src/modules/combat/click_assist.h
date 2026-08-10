#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/click_scheduler.h"
#include <imgui.h>
#include <Windows.h>

// =================================================================
// Click Assist
// =================================================================
// 20 CPS is not the problem. Plenty of people butterfly at 18-22.
// What flags is the SHAPE of the click stream, so all of the shape
// logic lives in ClickScheduler and this module only decides when
// clicking is allowed and what the stream should look like.
//
// WHY THE MODULE NO LONGER OWNS THE TIMING
// It used to hand the scheduler a std::function that reached back
// into this object from the timer thread, under a lock, while the
// render thread was reading the same fields to draw this panel.
// Three threads on one object, plus a captured pointer that dies
// on eject. Now the module pushes a POD profile and never gets
// called back, which removes the whole class of crash.
//
// WHY THE CLICKS THEMSELVES CHANGED
// The scheduler used to call SendInput. That cannot work while you
// are holding the physical button: Windows already has the button
// down, so the injected press is not a state change and never
// arrives, while the injected release does. The game saw releases
// and no presses. Clicks are now queued into KeyBinding.pressTime,
// the same counter a real click increments.
//
// PATTERNS
//   0 Normal    single stream, gaussian gaps. Honest cap around 14
//   1 Butterfly two-finger pairs. The only real way past 16
//   2 Drag      dense bursts with longer recovery
//   3 Jitter    high variance, mid rate
// =================================================================

class ClickAssist : public Module {
private:
    // ---- Shape ----
    int   m_pattern = 1;
    float m_minCPS  = 15.0f;
    float m_maxCPS  = 20.0f;

    // ---- When to run ----
    // 0 while holding LMB, 1 while holding and a target is near,
    // 2 always
    int   m_trigger      = 0;
    bool  m_requireTarget = false;
    float m_targetRange   = 4.0f;

    // Butterfly
    int   m_pairGapMin     = 22;
    int   m_pairGapMax     = 38;
    int   m_restGapMin     = 62;
    int   m_restGapMax     = 98;
    float m_pairSkipChance = 6.0f;

    // Drag
    int   m_burstLenMin = 3;
    int   m_burstLenMax = 7;
    int   m_burstGapMin = 90;
    int   m_burstGapMax = 170;

    // Humanisation
    bool  m_jitter         = true;
    float m_jitterAmount   = 26.0f;
    bool  m_fatigue        = true;
    float m_fatigueRate    = 12.0f;
    int   m_fatigueAfterMs = 2600;
    bool  m_outliers       = true;
    float m_outlierChance  = 4.0f;
    int   m_outlierAddMin  = 40;
    int   m_outlierAddMax  = 120;

    // Safety
    int   m_hardFloorMs   = 24;
    bool  m_entropyGuard  = true;
    float m_minStdDev     = 9.0f;
    bool  m_breakPatterns = true;
    float m_breakChance   = 8.0f;
    int   m_breakLenMin   = 2;
    int   m_breakLenMax   = 5;

    // Readout, written only on the client thread
    const char* m_why = "idle";
    int m_delivered = 0;

    ClickProfile BuildProfile() const {
        ClickProfile p;
        p.pattern        = m_pattern;
        p.minCPS         = m_minCPS;
        p.maxCPS         = m_maxCPS;
        p.pairGapMin     = m_pairGapMin;
        p.pairGapMax     = m_pairGapMax;
        p.restGapMin     = m_restGapMin;
        p.restGapMax     = m_restGapMax;
        p.pairSkipChance = m_pairSkipChance;
        p.burstLenMin    = m_burstLenMin;
        p.burstLenMax    = m_burstLenMax;
        p.burstGapMin    = m_burstGapMin;
        p.burstGapMax    = m_burstGapMax;
        p.jitter         = m_jitter;
        p.jitterAmount   = m_jitterAmount;
        p.fatigue        = m_fatigue;
        p.fatigueRate    = m_fatigueRate;
        p.fatigueAfterMs = m_fatigueAfterMs;
        p.outliers       = m_outliers;
        p.outlierChance  = m_outlierChance;
        p.outlierAddMin  = m_outlierAddMin;
        p.outlierAddMax  = m_outlierAddMax;
        p.floorMs        = m_hardFloorMs;
        p.entropyGuard   = m_entropyGuard;
        p.minStdDev      = m_minStdDev;
        p.breaks         = m_breakPatterns;
        p.breakChance    = m_breakChance;
        p.breakLenMin    = m_breakLenMin;
        p.breakLenMax    = m_breakLenMax;
        return p;
    }

public:
    ClickAssist() : Module("Click Assist", "High CPS with human click timing",
                           ModuleCategory::COMBAT, 0)
    {
        Bind("Pattern", &m_pattern);
        Bind("Min CPS", &m_minCPS);
        Bind("Max CPS", &m_maxCPS);
        Bind("Trigger", &m_trigger);
        Bind("Require Target", &m_requireTarget);
        Bind("Target Range", &m_targetRange);
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
        Bind("Outlier Add Min", &m_outlierAddMin);
        Bind("Outlier Add Max", &m_outlierAddMax);
        Bind("Hard Floor", &m_hardFloorMs);
        Bind("Entropy Guard", &m_entropyGuard);
        Bind("Min Std Dev", &m_minStdDev);
        Bind("Break Patterns", &m_breakPatterns);
        Bind("Break Chance", &m_breakChance);
        Bind("Break Len Min", &m_breakLenMin);
        Bind("Break Len Max", &m_breakLenMax);
    }

    void OnEnable(JNIEnv*) override {
        ClickScheduler::SetRightButton(false);
        ClickScheduler::SetProfile(BuildProfile());
    }

    void OnDisable(JNIEnv* env) override {
        ClickScheduler::SetActive(false);
        ClickScheduler::ClearPending();
        if (env) KeyBinds::ClearClickQueue(env);
        m_why = "off";
    }

    // Decides only whether the stream should be running. The clicks
    // themselves are drained and handed to the game by ModuleManager.
    void OnTick(JNIEnv* env) override {
        ClickScheduler::SetProfile(BuildProfile());

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { ClickScheduler::SetActive(false); m_why = "no player"; return; }

        if (Minecraft::IsInGui(env)) {
            // runTick does not consume pressTime while a screen is
            // open, so anything queued would fire in one burst the
            // moment it closes.
            ClickScheduler::SetActive(false);
            KeyBinds::ClearClickQueue(env);
            m_why = "menu";
            return;
        }

        bool holding = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        bool active;
        switch (m_trigger) {
            case 1:  active = holding && CombatState::HasTarget(); break;
            case 2:  active = true; break;
            default: active = holding; break;
        }

        if (active && m_requireTarget
            && CombatState::TargetDist() > (double)m_targetRange) {
            active = false;
            m_why = "nothing in range";
        }

        ClickScheduler::SetActive(active);

        if (!active && m_why[0] != 'n') m_why = holding ? "waiting" : "not holding";
        else if (active) m_why = "clicking";
    }

    void NoteDelivered(int n) { m_delivered += n; }

    void RenderSettings() override {
        // ---- Live ----
        bool ok = ClickScheduler::IsRunning();
        ImGui::TextColored(ok ? ImVec4(0.4f, 1.f, 0.6f, 1.f)
                              : ImVec4(1.f, 0.4f, 0.4f, 1.f),
            "%s  (%s)", ok ? "Timer running" : "TIMER DEAD", m_why);

        ImGui::TextDisabled("Delivered %d | queued %lld | last gap %lld ms",
            m_delivered, ClickScheduler::Emitted(), ClickScheduler::LastGap());

        float sd = ClickScheduler::LiveStdDev();
        if (sd < 900.f) {
            ImVec4 col = (sd < m_minStdDev) ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                                            : ImVec4(0.4f, 1.f, 0.6f, 1.f);
            ImGui::TextColored(col, "Measured %.1f CPS | std-dev %.1f ms",
                ClickScheduler::LiveCPS(), sd);
        }

        if (!KeyBinds::HasClickQueue()) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "pressTime unresolved: clicks cannot reach the game");
        }

        ImGui::Separator();
        const char* pats[] = { "Normal", "Butterfly", "Drag", "Jitter" };
        ImGui::Combo("Pattern", &m_pattern, pats, 4);

        if (m_pattern == 0 && m_maxCPS > 15.f) {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                "! A flat stream above 15 CPS is not humanly reachable");
        }

        ImGui::SliderFloat("Min CPS", &m_minCPS, 1.f, 24.f, "%.0f");
        ImGui::SliderFloat("Max CPS", &m_maxCPS, 1.f, 26.f, "%.0f");
        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "When to run");
        const char* trig[] = { "Holding LMB", "Holding + target", "Always" };
        ImGui::Combo("Trigger", &m_trigger, trig, 3);
        if (m_trigger == 2) {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                "Always-on clicking in a lobby is the easiest thing to spot");
        }
        ImGui::Checkbox("Require Target In Range", &m_requireTarget);
        if (m_requireTarget)
            ImGui::SliderFloat("Target Range", &m_targetRange, 2.f, 8.f, "%.1f");

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
                "Shape implies about %.1f CPS", 2000.0f / (avgPair + avgRest));
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
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Humanisation");
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
            ImGui::SliderInt("Break Len Min", &m_breakLenMin, 1, 8);
            ImGui::SliderInt("Break Len Max", &m_breakLenMax, 1, 12);
            if (m_breakLenMin > m_breakLenMax) m_breakLenMin = m_breakLenMax;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Entropy guard");
        ImGui::Checkbox("Enforce Variance", &m_entropyGuard);
        if (m_entropyGuard)
            ImGui::SliderFloat("Min Std Dev (ms)", &m_minStdDev, 3.f, 25.f, "%.0f");
        ImGui::SliderInt("Hard Floor (ms)", &m_hardFloorMs, 18, 50);
        ImGui::TextDisabled("Shared with Hit Select, so the two cannot stack.");

        long long dropped = ClickScheduler::Dropped();
        if (dropped > 20) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "%lld clicks dropped: the game is not draining them, "
                "lower your CPS", dropped);
        }
    }
};
