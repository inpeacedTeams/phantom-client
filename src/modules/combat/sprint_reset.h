#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <cstdio>
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
// WHY YOU FROZE AFTER EVERY HIT: THREE SEPARATE BUGS
// -----------------------------------------------------------------
// FIRST, the key never came back.
//
// W-tap sets keyBindForward.pressed to false for a tick. In 1.8
// that field is only written when the keyboard fires an EVENT, and
// holding W produces exactly one event, at the moment you press it.
// Once we cleared the field the game had no reason to set it back:
// no event was coming. You stood still until you let go of W and
// pressed it again. That half is fixed in KeyBinds, which now
// restores the key from the live hardware state, and backed up by
// the per-tick reconcile pass in ModuleManager.
//
// SECOND, even with forward restored, THE SPRINT DID NOT COME BACK.
//
// EntityPlayerSP.onLivingUpdate only starts sprinting again when:
//
//     moveForward >= 0.8 && !isSprinting() && ...
//         && gameSettings.keyBindSprint.isKeyDown()
//
// or when the double-tap timer is running. If you sprint by
// double-tapping W, as most people in PvP do, the sprint key is not
// held and that timer only advances on a real key event. So after
// the reset you kept walking, at walking speed, forever.
//
// The fix is to re-arm: hold the sprint key for a tick afterwards.
// That satisfies the game's own condition, so IT decides to sprint
// and sends the packet itself. Identical to a player tapping ctrl
// after a W-tap, which is exactly what good players do.
//
// THIRD, a "1 tick" reset held the key for two.
//
// The countdown was set to N and then tested before decrementing,
// so the minimum hold was 2 ticks, 100ms. That is long enough to
// visibly stall you on every single hit, and it doubled the
// momentum the reset costs for no extra knockback.
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
    // ---- Core ----
    int   m_method = 0;
    float m_chance = 100.0f;

    // ---- Advanced ----
    int   m_resetTicksMin = 1;
    int   m_resetTicksMax = 1;
    int   m_hitDelay      = 0;
    bool  m_rearmSprint   = true;
    int   m_rearmTicks    = 1;
    bool  m_onlyWhileMoving = true;
    bool  m_onlyOnHit       = true;   // a real attack, not a click
    bool  m_requireSprint   = true;   // pointless if not sprinting

    // ---- State ----
    bool m_resetting       = false;
    int  m_resetCountdown  = 0;
    int  m_heldTicks       = 0;
    bool m_rearming        = false;
    int  m_rearmLeft       = 0;
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
    mutable char m_status[48] = "";

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

    // Only these three actually stop the sprint by cutting movement,
    // so only these need it started again afterwards.
    static bool MethodBreaksSprint(int m) {
        return m == 0 || m == 1 || m == 4;
    }

    void StopRearm(JNIEnv* env) {
        if (!m_rearming) return;
        KeyBinds::ReleaseSprint(env);
        m_rearming = false;
        m_rearmLeft = 0;
    }

    // Hold the sprint key so the game's own check fires and it
    // decides to sprint. We never call setSprinting(true) for this:
    // that only flips a client flag which onLivingUpdate overwrites
    // on the same tick, and it desyncs the sprint packet.
    void BeginRearm(JNIEnv* env) {
        if (!m_rearmSprint) return;
        if (!KeyBinds::HasSprint()) return;
        if (!MethodBreaksSprint(m_activeMethod)) return;

        // Pointless if they stopped walking during the reset
        if (!KeyBinds::PhysForward(env)) return;

        KeyBinds::SetSprint(env, true);
        m_rearming  = true;
        m_rearmLeft = m_rearmTicks < 1 ? 1 : m_rearmTicks;
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

        m_resetting = false;
        m_resetCountdown = 0;
        m_heldTicks = 0;

        if (env && player) BeginRearm(env);
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

        // Cancel the sprint now rather than waiting for the game to
        // notice movement stopped. One tick earlier is the whole
        // point of the module.
        if (MethodBreaksSprint(m_method))
            SetSprintFlag(env, player, false);

        m_resetting = true;
        m_heldTicks = 0;

        // Minus one because the tick that starts the reset IS the
        // first tick the key is held. Without this a setting of 1
        // held for two ticks, which is 100ms of standing still on
        // every hit.
        m_resetCountdown = RandTicks() - 1;
        if (m_resetCountdown < 0) m_resetCountdown = 0;
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
        Bind("Rearm Sprint", &m_rearmSprint);
        Bind("Rearm Ticks", &m_rearmTicks);
        Bind("Only While Moving", &m_onlyWhileMoving);
        Bind("Only On Hit", &m_onlyOnHit);
        Bind("Require Sprint", &m_requireSprint);
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);

        // Packet mode is the only one that does not need keybinds
        if (!KeyBinds::Init(env) && m_method != 5) return;

        jobject player = Minecraft::GetPlayer(env);

        // The mouse edge is tracked every tick, not only inside the
        // branch that uses it. Updating it lazily meant switching
        // trigger modes with the button already held read as a fresh
        // press and fired one reset for free.
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool lmbEdge = lmb && !m_lastLMB;
        m_lastLMB = lmb;

        // No player means we cannot even read state. Unwind first so
        // a respawn or a dimension change cannot strand a key.
        if (!player) {
            if (m_resetting) EndReset(env, nullptr);
            StopRearm(env);
            m_why = "no player";
            return;
        }

        if (Minecraft::IsInGui(env)) {
            if (m_resetting) EndReset(env, player);
            StopRearm(env);
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

        // ---- Re-arm: hold ctrl briefly so the sprint restarts ----
        if (m_rearming) {
            if (m_rearmLeft > 0) {
                m_rearmLeft--;
                m_why = "restarting sprint";
                return;
            }
            StopRearm(env);
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
            trigger = lmbEdge;
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
        StopRearm(env);

        // Backstop: whatever state we thought we were in, hand every
        // key we touched back to the player.
        if (env) KeyBinds::ReleaseAll(env);

        m_waitingForDelay = false;
        m_resetCountdown  = 0;
        m_delayCountdown  = 0;
        m_heldTicks       = 0;
        m_why = "off";
    }

    // -------------------------------------------------------------
    // World change, respawn, reconnect.
    //
    // A reset is at most a few ticks long, so one is almost
    // certainly in flight when a fight ends in a death. The keys
    // have already been released underneath us; what is left is the
    // bookkeeping, and leaving that set means the next world starts
    // with the module convinced it is mid-reset and refusing to
    // start another one.
    // -------------------------------------------------------------
    void OnReset(JNIEnv* env) override {
        m_resetting       = false;
        m_rearming        = false;
        m_rearmLeft       = 0;
        m_resetCountdown  = 0;
        m_delayCountdown  = 0;
        m_waitingForDelay = false;
        m_heldTicks       = 0;
        m_lastLMB         = false;
        m_why             = "idle";
        (void)env;   // keys are already released by the manager
    }

    const char* StatusLine() const override {
        static const char* names[] = { "W-Tap", "S-Tap", "Blockhit",
                                       "Sneak", "Ctrl", "Packet" };
        int m = (m_method >= 0 && m_method < 6) ? m_method : 0;
        snprintf(m_status, sizeof(m_status), "%s  \xc2\xb7  %d resets",
                 names[m], m_resets);
        return m_status;
    }

    bool HasAdvanced() const override { return true; }

    // -------------------------------------------------------------
    // Core panel: which reset, how often. Nothing else changes the
    // outcome enough to be worth a slider on the front page.
    // -------------------------------------------------------------
    void RenderSettings() override {
        const char* methods[] = {
            "W-Tap", "S-Tap", "Blockhit", "Sneak Tap", "Ctrl Spam", "Packet"
        };
        ImGui::Combo("Method", &m_method, methods, 6);

        switch (m_method) {
            case 0: ImGui::TextDisabled("Releases forward for a tick. Works everywhere."); break;
            case 1: ImGui::TextDisabled("Taps back. Stops you faster and opens distance."); break;
            case 2: ImGui::TextDisabled("Holds block. Also halves damage. Sword only."); break;
            case 3: ImGui::TextDisabled("Taps sneak. No speed loss, common in sumo."); break;
            case 4: ImGui::TextDisabled("Re-presses sprint. Loses the least momentum."); break;
            case 5: ImGui::TextDisabled("Toggles sprint inside one tick. Fastest."); break;
        }
        if (m_method == 5) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "Packet mode is detected by Polar and AGC");
        }

        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");

        // Failure states belong up front: without these the module
        // silently does nothing.
        if (!CombatState::IsUsable() && m_onlyOnHit) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Swing detection unresolved: turn off Only On Landed Hits");
        }
        if (!KeyBinds::HasMovement()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Keybinds unresolved: only Packet mode works");
        }
        if (m_rescued > 0) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Watchdog freed a stuck key %d time(s)", m_rescued);
        }
    }

    void RenderAdvanced() override {
        const char* state = m_resetting ? "RESETTING"
                          : m_rearming  ? "RE-ARMING"
                                        : "idle";
        ImGui::TextDisabled("%s (%s) | air swings ignored %d | your CPS %.1f",
            state, m_why, m_skipped, CombatState::CPS());
        if (KeyBinds::Repairs() > 0) {
            ImGui::TextDisabled("Key reconcile repaired %d desync(s)",
                KeyBinds::Repairs());
        }
        if (!KeyBinds::CanReadHardware()) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Cannot read the keyboard directly: keys restore from "
                "the saved value instead");
        }

        ImGui::SeparatorText("Timing");
        ImGui::SliderInt("Hold Min (ticks)", &m_resetTicksMin, 1, 5);
        ImGui::SliderInt("Hold Max (ticks)", &m_resetTicksMax, 1, 5);
        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;
        ImGui::SliderInt("Hit Delay (ticks)", &m_hitDelay, 0, 5);
        ImGui::TextDisabled("1 tick is 50ms. Longer holds cost more momentum "
                            "than they gain in knockback.");

        ImGui::SeparatorText("Sprint recovery");
        ImGui::Checkbox("Re-arm Sprint", &m_rearmSprint);
        if (m_rearmSprint) {
            ImGui::SliderInt("Re-arm Ticks", &m_rearmTicks, 1, 4);
            ImGui::TextDisabled("Taps the sprint key after the reset so the "
                                "game restarts your sprint itself.");
            if (!KeyBinds::HasSprint()) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                    "Sprint keybind unresolved: cannot re-arm");
            }
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Off: if you sprint by double-tapping W you will stay "
                "at walking speed after every hit");
        }

        ImGui::SeparatorText("Trigger");
        ImGui::Checkbox("Only On Landed Hits", &m_onlyOnHit);
        ImGui::TextDisabled(m_onlyOnHit
            ? "Fires on a real swing with a target in reach."
            : "Fires on any mouse click, including air swings.");
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);
        ImGui::Checkbox("Require Sprint", &m_requireSprint);
    }
};
