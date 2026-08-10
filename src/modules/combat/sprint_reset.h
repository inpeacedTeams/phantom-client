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
// -----------------------------------------------------------------
// THE BUG THAT FROZE YOU AFTER EVERY HIT
// -----------------------------------------------------------------
// W-tap sets keyBindForward.pressed to false for a tick. In 1.8
// that field is only written when the keyboard fires an EVENT, and
// holding W produces exactly one event. So once we cleared it, the
// game had no reason to ever set it back: no event was coming. The
// player stood still until they let go of W and pressed it again.
//
// The real fix is in KeyBinds, which now restores the key from the
// live hardware state instead of blindly writing false. What lives
// here is the second line of defence:
//
//   * every exit path ends the reset, including losing the player
//     or the keybinds going away
//   * a watchdog force-ends any reset that has run too long, so a
//     dropped tick can never strand a key
//   * the movement gate asks what the PLAYER is holding, not what
//     the keybind currently says, because during our own reset the
//     keybind says false and the module would deadlock itself
//
// -----------------------------------------------------------------
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
    int   m_method        = 0;
    float m_chance        = 100.0f;
    int   m_resetTicksMin = 1;
    int   m_resetTicksMax = 1;
    int   m_hitDelay      = 0;

    // ---- Gating ----
    bool  m_onlyWhileMoving = true;
    bool  m_onlyOnHit       = true;   // a real attack, not a click
    bool  m_requireSprint   = true;   // pointless if not sprinting

    // ---- State ----
    bool m_resetting       = false;
    int  m_resetCountdown  = 0;
    int  m_heldTicks       = 0;
    int  m_delayCountdown  = 0;
    bool m_waitingForDelay = false;
    bool m_lastLMB         = false;
    int  m_activeMethod    = 0;   // method that started the current reset

    // No reset has any business lasting longer than this. If one
    // does, something ate a tick and a key is about to get stuck.
    static constexpr int kMaxHoldTicks = 6;

    // ---- Readout ----
    int m_resets  = 0;
    int m_skipped = 0;
    int m_rescued = 0;
    const char* m_why = "idle";

    jmethodID m_setSprinting = nullptr;
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
        }
        m_resolved = true;
    }

    void SetSprintFlag(JNIEnv* env, jobject player, bool on) {
        if (!m_setSprinting || !player) return;
        env->CallVoidMethod(player, m_setSprinting, (jboolean)on);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    bool IsSprinting(JNIEnv* env, jobject player) {
        if (!Minecraft::HasSprintCheck()) return true;   // cannot tell, allow
        return Minecraft::IsSprinting(env, player);
    }

    // Undo whatever the reset did. Keyed on m_activeMethod rather
    // than m_method, so changing the method mid-reset cannot leave a
    // key held down forever.
    //
    // Release() puts the key back to whatever the hardware says now,
    // so letting go of W mid-reset does not strand you moving.
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
        m_resetCountdown = 0;
        m_heldTicks = 0;
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
        m_heldTicks      = 0;
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

        // No player means we cannot even read state. Unwind first so
        // a respawn or a dimension change cannot strand a key.
        if (!player) {
            if (m_resetting) EndReset(env, nullptr);
            m_why = "no player";
            return;
        }

        if (Minecraft::IsInGui(env)) {
            if (m_resetting) EndReset(env, player);
            m_why = "menu";
            return;
        }

        // ---- Finish an in-flight reset before considering another ----
        if (m_resetting) {
            m_heldTicks++;

            // Watchdog. Nothing here should ever take this long, so
            // if it has, give the key back rather than debug it live.
            if (m_heldTicks > kMaxHoldTicks) {
                m_rescued++;
                EndReset(env, player);
                m_why = "watchdog released a stuck key";
                return;
            }

            if (m_resetCountdown <= 0) EndReset(env, player);
            else m_resetCountdown--;

            m_why = "resetting";
            return;
        }

        // Ask what the PLAYER is holding. Reading the keybind would
        // return our own override during a reset, and the module
        // would refuse to ever start another one.
        if (m_onlyWhileMoving && !KeyBinds::PhysForward(env)) {
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
            // A real swing with something in reach. Air swings no
            // longer cost you momentum.
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
        // Backstop: whatever state we thought we were in, hand every
        // key we touched back to the player.
        if (env) KeyBinds::ReleaseAll(env);

        m_waitingForDelay = false;
        m_resetCountdown  = 0;
        m_delayCountdown  = 0;
        m_heldTicks       = 0;
        m_why = "off";
    }

    void RenderSettings() override {
        // ---- Live ----
        ImGui::TextColored(m_resetting ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                       : ImVec4(0.55f, 0.55f, 0.6f, 1.f),
            "%s  (%s)", m_resetting ? "RESETTING" : "idle", m_why);
        ImGui::TextDisabled("Resets %d | air swings ignored %d | your CPS %.1f",
            m_resets, m_skipped, CombatState::CPS());

        if (m_rescued > 0) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Watchdog fired %d time(s): the game is dropping ticks", m_rescued);
        }
        if (!KeyBinds::CanReadHardware()) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Cannot read the keyboard directly: keys restore from "
                "the saved value instead");
        }

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
        ImGui::TextDisabled("Longer holds cost more momentum than they "
                            "gain in knockback.");

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
