#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Sprint Reset (More KB)
// =================================================================
// Every hit while sprinting deals extra knockback, but the server
// cancels your sprint after the first one (MC-69459). The client
// still believes it is sprinting, so every hit after that lands
// with reduced knockback and you never see why.
//
// Sprint resetting drops and re-applies sprint so each hit counts
// as a fresh sprint hit.
//
// WHAT CHANGED AND WHY IT MATTERS
//
// This used to fire on GetAsyncKeyState(VK_LBUTTON). That is the
// mouse button, not an attack: clicking at the sky, at a block or
// at empty air all reset your sprint for nothing. Losing forward
// momentum for a swing that hit no one is strictly worse than not
// having the module at all.
//
// It now triggers on CombatState::AttackedThisTick(), which means
// the arm actually swung AND something was in reach. Same event the
// server processes, so the reset lands where it is worth having.
//
// EVERYTHING GOES THROUGH KEYBINDS
//   1. setSprinting() alone does not hold: onLivingUpdate recomputes
//      sprint from the keys every tick and overwrites it.
//   2. KeyBinds records which keys we forced, so ReleaseAll() frees
//      them on disconnect or eject. A key set behind its back stays
//      stuck down with no way for the player to clear it.
//
// METHODS
//   0 W-Tap     release forward for a tick
//   1 S-Tap     tap back while forward stays held
//   2 Blockhit  hold use-item, which also resets sprint
//   3 Sneak Tap tap sneak, no speed loss
//   4 Ctrl Spam re-press the sprint key
//   5 Packet    setSprinting false then true inside one tick
// =================================================================

class SprintReset : public Module {
private:
    // ---- Behaviour ----
    int   m_method          = 0;
    float m_chance          = 100.0f;
    int   m_resetTicksMin   = 1;
    int   m_resetTicksMax   = 1;
    int   m_hitDelay        = 0;

    // ---- Gating ----
    bool  m_onlyWhileMoving = true;
    bool  m_onlyOnHit       = true;   // a real attack, not a click
    bool  m_requireSprint   = true;   // pointless if not sprinting

    // ---- State ----
    bool m_resetting       = false;
    int  m_resetCountdown  = 0;
    int  m_delayCountdown  = 0;
    bool m_waitingForDelay = false;
    bool m_lastLMB         = false;
    int  m_activeMethod    = 0;   // method that started the current reset

    // ---- Readout ----
    int m_resets  = 0;
    int m_skipped = 0;
    const char* m_why = "idle";

    jmethodID m_setSprinting = nullptr;
    jmethodID m_isSprinting  = nullptr;
    bool m_resolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

    int RandTicks() {
        if (m_resetTicksMin >= m_resetTicksMax) return m_resetTicksMin;
        std::uniform_int_distribution<int> d(m_resetTicksMin, m_resetTicksMax);
        return d(m_rng);
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entity) {
            m_setSprinting = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_70031_b", "setSprinting" }, 1);
            m_isSprinting = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_70051_ag", "isSprinting" }, 0);
        }
        m_resolved = true;
    }

    void SetSprintFlag(JNIEnv* env, jobject player, bool on) {
        if (!m_setSprinting || !player) return;
        env->CallVoidMethod(player, m_setSprinting, (jboolean)on);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    bool IsSprinting(JNIEnv* env, jobject player) {
        if (!m_isSprinting || !player) return true;   // cannot tell, allow
        jboolean v = env->CallBooleanMethod(player, m_isSprinting);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        return v != 0;
    }

    // Undo whatever the reset did. Keyed on m_activeMethod rather
    // than m_method, so changing the method mid-reset cannot leave a
    // key held down forever.
    void EndReset(JNIEnv* env, jobject player) {
        switch (m_activeMethod) {
            case 0: KeyBinds::ReleaseForward(env); break;
            case 1: KeyBinds::ReleaseBack(env);    break;
            case 2: KeyBinds::ReleaseUseItem(env); break;
            case 3: KeyBinds::ReleaseSneak(env);   break;
            case 4: KeyBinds::ReleaseSprint(env);  break;
            default: break;
        }

        if (m_activeMethod == 0 || m_activeMethod == 1 || m_activeMethod == 4)
            SetSprintFlag(env, player, true);

        m_resetting = false;
    }

    void BeginReset(JNIEnv* env, jobject player) {
        m_activeMethod = m_method;
        m_resets++;

        switch (m_method) {
            case 0: KeyBinds::SetForward(env, false); break;
            case 1: KeyBinds::SetBack(env, true);     break;
            case 2: KeyBinds::SetUseItem(env, true);  break;
            case 3: KeyBinds::SetSneak(env, true);    break;
            case 4: KeyBinds::SetSprint(env, false);  break;
            case 5:
                SetSprintFlag(env, player, false);
                SetSprintFlag(env, player, true);
                return;   // instant, nothing to unwind
        }

        if (m_method == 0 || m_method == 1 || m_method == 4)
            SetSprintFlag(env, player, false);

        m_resetting      = true;
        m_resetCountdown = RandTicks();
    }

public:
    SprintReset()
        : Module("Sprint Reset", "Reset sprint on every landed hit for full knockback",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("Method", &m_method);
        Bind("Chance", &m_chance);
        Bind("Reset Ticks Min", &m_resetTicksMin);
        Bind("Reset Ticks Max", &m_resetTicksMax);
        Bind("Hit Delay", &m_hitDelay);
        Bind("Only While Moving", &m_onlyWhileMoving);
        Bind("Only On Hit", &m_onlyOnHit);
        Bind("Require Sprint", &m_requireSprint);
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);

        // Packet mode is the only one that does not need keybinds
        if (!KeyBinds::Init(env) && m_method != 5) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) {
            if (m_resetting) EndReset(env, player);
            m_why = "menu";
            return;
        }

        // Always finish an in-flight reset before considering another
        if (m_resetting) {
            if (m_resetCountdown <= 0) EndReset(env, player);
            else m_resetCountdown--;
            m_why = "resetting";
            return;
        }

        // Read the game's forward state rather than the physical key.
        // The player may have rebound movement, and other modules may
        // be driving forward themselves.
        if (m_onlyWhileMoving && !KeyBinds::GetForward(env)) {
            m_why = "not moving";
            return;
        }

        // Resetting a sprint you do not have costs momentum and
        // gains nothing.
        if (m_requireSprint && !IsSprinting(env, player)) {
            m_why = "not sprinting";
            return;
        }

        if (m_waitingForDelay) {
            if (m_delayCountdown > 0) { m_delayCountdown--; return; }
            m_waitingForDelay = false;
            if (Roll(m_chance)) BeginReset(env, player);
            return;
        }

        // ---- The trigger ----
        bool trigger;
        if (m_onlyOnHit) {
            // A real swing with something in reach. This is the whole
            // point: air swings no longer cost you momentum.
            trigger = CombatState::AttackedThisTick();
            if (CombatState::SwungThisTick() && !trigger) m_skipped++;
        } else {
            bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            trigger = lmb && !m_lastLMB;
            m_lastLMB = lmb;
        }

        if (!trigger) {
            m_why = "waiting for a hit";
            return;
        }

        if (m_hitDelay > 0) {
            m_delayCountdown  = m_hitDelay;
            m_waitingForDelay = true;
            return;
        }

        if (Roll(m_chance)) BeginReset(env, player);
        else m_why = "missed one";
    }

    void OnDisable(JNIEnv* env) override {
        if (m_resetting) {
            jobject player = Minecraft::GetPlayer(env);
            EndReset(env, player);
        }
        m_waitingForDelay = false;
        m_resetCountdown  = 0;
        m_delayCountdown  = 0;
    }

    void RenderSettings() override {
        // ---- Live ----
        ImGui::TextColored(m_resetting ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                       : ImVec4(0.55f, 0.55f, 0.6f, 1.f),
            "%s  (%s)", m_resetting ? "RESETTING" : "idle", m_why);
        ImGui::TextDisabled("Resets %d | air swings ignored %d | your CPS %.1f",
            m_resets, m_skipped, CombatState::CPS());

        ImGui::Separator();
        const char* methods[] = {
            "W-Tap", "S-Tap", "Blockhit", "Sneak Tap", "Ctrl Spam", "Packet"
        };
        ImGui::Combo("Method", &m_method, methods, 6);

        if (m_method == 5) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "! Packet mode is detected by Polar and AGC");
        }

        ImGui::Separator();
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Reset Ticks Min", &m_resetTicksMin, 1, 5);
        ImGui::SliderInt("Reset Ticks Max", &m_resetTicksMax, 1, 5);
        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;
        ImGui::SliderInt("Hit Delay (ticks)", &m_hitDelay, 0, 5);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Trigger");
        ImGui::Checkbox("Only On Landed Hits", &m_onlyOnHit);
        ImGui::TextDisabled(m_onlyOnHit
            ? "Fires on a real swing with a target in reach."
            : "Fires on any mouse click, including air swings.");
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);
        ImGui::Checkbox("Require Sprint", &m_requireSprint);

        ImGui::Separator();
        switch (m_method) {
            case 0: ImGui::TextWrapped("Releases forward for a tick. The standard reset, works everywhere."); break;
            case 1: ImGui::TextWrapped("Taps back for a tick. Stops you faster and opens more distance."); break;
            case 2: ImGui::TextWrapped("Holds block. Resets sprint and halves incoming damage. Sword only."); break;
            case 3: ImGui::TextWrapped("Taps sneak. No speed loss, common in sumo."); break;
            case 4: ImGui::TextWrapped("Re-presses the sprint key. Loses the least momentum."); break;
            case 5: ImGui::TextWrapped("Toggles sprint within a single tick. Fastest, but detectable."); break;
        }

        if (!CombatState::IsUsable() && m_onlyOnHit) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Swing field unresolved: turn off Only On Landed Hits");
        }
        if (!KeyBinds::HasMovement()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Keybinds unresolved: only Packet mode works");
        }
    }
};
