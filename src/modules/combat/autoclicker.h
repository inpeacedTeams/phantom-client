#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/click_scheduler.h"
#include <imgui.h>
#include <cstdio>

// =================================================================
// AutoClicker
// =================================================================
// Clicks while you hold the button. That is the whole feature, and
// the previous version got it wrong in three ways worth naming.
//
//   It never clicked at all. Clicks went out through SendInput,
//   which cannot inject a press for a button Windows already
//   considers held. Clicks now go into KeyBinding.pressTime, the
//   counter a real click increments.
//
//   It reached back into this object from the timer thread while
//   the render thread was drawing the same fields. Timing now lives
//   entirely in the engine and the module only publishes a POD.
//
//   It polled the button on the 20 TPS tick, so the first click was
//   late and clicks kept landing for up to 50ms after release. The
//   engine watches the button itself at 1ms.
//
// WHAT THIS MODULE STILL DOES
// Decides whether clicking is ALLOWED. Being in a world, not in a
// menu, optionally only with something in front of you. The button
// itself is the engine's business.
//
// THE PANEL IS ONE SLIDER
// You want a CPS. Everything else has a defensible default derived
// from how hands actually behave, and lives behind Advanced.
// =================================================================

class AutoClicker : public Module {
private:
    // ---- Core ----
    float m_cps = 12.0f;

    // 0 attack, 1 use item
    int  m_button = 0;

    // ---- Gating ----
    bool  m_requireTarget = false;
    float m_targetRange   = 4.0f;
    bool  m_blockInGui    = true;

    // ---- Humanisation ----
    float m_variance     = 18.0f;
    bool  m_drift        = true;
    float m_driftAmount  = 12.0f;
    bool  m_fumbles      = true;
    float m_fumbleChance = 3.0f;
    bool  m_bursts       = true;
    float m_burstChance  = 4.0f;
    int   m_floorMs      = 22;

    // ---- Readout ----
    int  m_delivered = 0;
    const char* m_why = "idle";
    mutable char m_status[40] = {};

    ClickerConfig BuildConfig() const {
        ClickerConfig c;
        c.cps          = m_cps;
        c.variance     = m_variance;
        c.drift        = m_drift;
        c.driftAmount  = m_driftAmount;
        c.fumbles      = m_fumbles;
        c.fumbleChance = m_fumbleChance;
        c.bursts       = m_bursts;
        c.burstChance  = m_burstChance;
        c.floorMs      = m_floorMs;
        c.rightButton  = (m_button == 1);
        return c;
    }

    // The fastest rate this configuration can actually reach. Asking
    // for 30 CPS with a 22ms floor gets you 45, and the slider
    // should say so rather than quietly lying.
    float CeilingCPS() const {
        return m_floorMs > 0 ? 1000.0f / (float)m_floorMs : 99.0f;
    }

public:
    AutoClicker()
        : Module("AutoClicker", "Clicks while you hold the button",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("CPS", &m_cps);
        Bind("Button", &m_button);
        Bind("Require Target", &m_requireTarget);
        Bind("Target Range", &m_targetRange);
        Bind("Block In GUI", &m_blockInGui);
        Bind("Variance", &m_variance);
        Bind("Drift", &m_drift);
        Bind("Drift Amount", &m_driftAmount);
        Bind("Fumbles", &m_fumbles);
        Bind("Fumble Chance", &m_fumbleChance);
        Bind("Bursts", &m_bursts);
        Bind("Burst Chance", &m_burstChance);
        Bind("Floor", &m_floorMs);
    }

    void OnEnable(JNIEnv*) override {
        ClickScheduler::SetConfig(BuildConfig());
        m_why = "waiting for the button";
    }

    void OnDisable(JNIEnv* env) override {
        ClickScheduler::SetArmed(false);
        ClickScheduler::ClearPending();
        // Anything the game has not consumed yet would fire on the
        // next tick, after the module is already off.
        if (env) KeyBinds::ClearClickQueue(env);
        m_why = "off";
        m_status[0] = '\0';
    }

    // Only decides whether clicking is permitted. The engine handles
    // the button and the timing.
    void OnTick(JNIEnv* env) override {
        ClickScheduler::SetConfig(BuildConfig());

        jobject player = Minecraft::GetPlayer(env);
        if (!player) {
            ClickScheduler::SetArmed(false);
            m_why = "no player";
            m_status[0] = '\0';
            return;
        }

        if (m_blockInGui && Minecraft::IsInGui(env)) {
            // runTick does not drain pressTime while a screen is
            // open, so a backlog would fire all at once on close.
            ClickScheduler::SetArmed(false);
            KeyBinds::ClearClickQueue(env);
            m_why = "menu";
            m_status[0] = '\0';
            return;
        }

        bool allowed = true;
        const char* why = "ready";

        if (m_requireTarget) {
            if (CombatState::TargetDist() > (double)m_targetRange) {
                allowed = false;
                why = "nothing in range";
            }
        }

        if (!KeyBinds::HasClickQueue()) {
            allowed = false;
            why = "click queue unresolved";
        }

        ClickScheduler::SetArmed(allowed);

        if (!allowed)                        m_why = why;
        else if (ClickScheduler::IsHolding()) m_why = "clicking";
        else                                  m_why = "holding the button starts it";

        if (ClickScheduler::IsClicking())
            snprintf(m_status, sizeof(m_status), "%.1f CPS", ClickScheduler::LiveCPS());
        else
            snprintf(m_status, sizeof(m_status), "%.0f CPS ready", m_cps);
    }

    void NoteDelivered(int n) { m_delivered += n; }

    const char* StatusLine() const override {
        return m_status[0] ? m_status : nullptr;
    }

    bool HasAdvanced() const override { return true; }

    // =============================================================
    // Core panel
    // =============================================================
    void RenderSettings() override {
        if (!ClickScheduler::IsRunning()) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "Click timer is not running");
        }
        if (!KeyBinds::HasClickQueue()) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "pressTime unresolved: clicks cannot reach the game");
        }

        ImGui::SliderFloat("CPS", &m_cps, 1.0f, 22.0f, "%.0f");

        float ceiling = CeilingCPS();
        if (m_cps > ceiling) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "The %d ms floor caps this at %.0f CPS", m_floorMs, ceiling);
        } else if (m_cps > 16.0f) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Above 16 is butterfly territory. Reachable, but people notice.");
        }

        const char* buttons[] = { "Left (attack)", "Right (use)" };
        ImGui::Combo("Button", &m_button, buttons, 2);

        // Live measurement, because the only honest answer to "is
        // this working" is what actually left the client.
        if (ClickScheduler::IsClicking()) {
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f),
                "%.1f CPS, spread %.1f ms",
                ClickScheduler::LiveCPS(), ClickScheduler::LiveStdDev());
        } else {
            ImGui::TextDisabled("%s", m_why);
        }
    }

    // =============================================================
    // Advanced
    // =============================================================
    void RenderAdvanced() override {
        ImGui::TextDisabled("Delivered %d | last gap %lld ms",
            m_delivered, ClickScheduler::LastGap());

        ImGui::SeparatorText("Timing shape");
        ImGui::SliderFloat("Variance", &m_variance, 2.f, 45.f, "%.0f%%");
        ImGui::TextDisabled("Spread of the bell curve around your CPS. "
                            "Under about 8%% the stream reads as machine.");

        ImGui::Checkbox("Drift", &m_drift);
        if (m_drift) {
            ImGui::SliderFloat("Drift Amount", &m_driftAmount, 2.f, 30.f, "%.0f%%");
            ImGui::TextDisabled("Slow wander so neighbouring clicks are related, "
                                "the way a real hand speeds up and eases off. "
                                "Plain randomness has no memory and that shows.");
        }

        ImGui::SeparatorText("Slips");
        ImGui::Checkbox("Fumbles", &m_fumbles);
        if (m_fumbles)
            ImGui::SliderFloat("Fumble Chance", &m_fumbleChance, 0.5f, 12.f, "%.1f%%");
        ImGui::Checkbox("Bursts", &m_bursts);
        if (m_bursts)
            ImGui::SliderFloat("Burst Chance", &m_burstChance, 0.5f, 12.f, "%.1f%%");
        ImGui::TextDisabled("Occasional long and short gaps. Hands produce "
                            "both; a loop produces neither.");

        ImGui::SeparatorText("Gating");
        ImGui::Checkbox("Only With A Target", &m_requireTarget);
        if (m_requireTarget)
            ImGui::SliderFloat("Target Range", &m_targetRange, 2.f, 8.f, "%.1f");
        ImGui::Checkbox("Stop In Menus", &m_blockInGui);

        ImGui::SeparatorText("Safety");
        ImGui::SliderInt("Floor (ms)", &m_floorMs, 18, 60);
        ImGui::TextDisabled("Hard minimum between clicks, shared with Hit "
                            "Select so the two cannot stack. Nothing under "
                            "20 ms is physically reachable.");

        long long dropped = ClickScheduler::Dropped();
        if (dropped > 30) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "%lld clicks dropped: asking for more than the game can "
                "consume. Lower the CPS.", dropped);
        }
    }
};
