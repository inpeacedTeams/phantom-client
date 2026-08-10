#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/focus.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
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
    enum Method { WTAP = 0, STAP, BLOCKHIT, SNEAK, CTRL, PACKET };

    static constexpr const char* kMethods[] = {
        "W-Tap", "S-Tap", "Block", "Sneak", "Ctrl", "Packet"
    };

    // ---- Core ----
    int   m_method = WTAP;
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
    int  m_activeMethod    = WTAP;   // method that started the current reset

    // No reset has any business lasting longer than this. If one
    // does, something ate a tick and a key is about to get stuck.
    static constexpr int kMaxHoldTicks = 6;

    // ---- Readout ----
    int m_resets  = 0;
    int m_skipped = 0;
    int m_rescued = 0;
    const char* m_why = "idle";
    mutable char m_status[48] = "";
    mutable char m_notice[192] = "";

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
        return m == WTAP || m == STAP || m == CTRL;
    }

    void StopRearm(JNIEnv* env) {
        if (!m_rearming) return;
        if (env) KeyBinds::ReleaseSprint(env);
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
        if (env) {
            switch (m_activeMethod) {
                case WTAP:     KeyBinds::ReleaseForward(env); break;
                case STAP:     KeyBinds::ReleaseBack(env);    break;
                case BLOCKHIT: KeyBinds::ReleaseUseItem(env); break;
                case SNEAK:    KeyBinds::ReleaseSneak(env);   break;
                case CTRL:     KeyBinds::ReleaseSprint(env);  break;
                default: break;
            }
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
            case WTAP:     KeyBinds::SetForward(env, false); break;
            case STAP:     KeyBinds::SetBack(env, true);     break;
            case BLOCKHIT: KeyBinds::SetUseItem(env, true);  break;
            case SNEAK:    KeyBinds::SetSneak(env, true);    break;
            case CTRL:     KeyBinds::SetSprint(env, false);  break;
            case PACKET:
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
        BindMode("Method", &m_method, kMethods, 6,
                 "How the sprint is broken. W-Tap works everywhere; Ctrl "
                 "loses the least momentum; Packet is the fastest and the "
                 "only one a server can see.");

        Bind("Chance", &m_chance, 10.0f, 100.0f, "%.0f%%",
             "How many of your hits get a reset");

        Bind("Hold Min", &m_resetTicksMin, 1, 5,
             "Ticks the key is held. One tick is 50ms, and longer costs more "
             "momentum than it gains in knockback.")
            .Advanced();

        Bind("Hold Max", &m_resetTicksMax, 1, 5)
            .Advanced();

        Bind("Hit Delay", &m_hitDelay, 0, 5,
             "Ticks between the hit landing and the reset starting")
            .Advanced();

        Bind("Re-arm Sprint", &m_rearmSprint,
             "Taps the sprint key afterwards so the game restarts your "
             "sprint itself. Without this, double-tap sprinters walk.")
            .Advanced();

        Bind("Re-arm Ticks", &m_rearmTicks, 1, 4,
             "How long the sprint key is held afterwards")
            .When("Re-arm Sprint", 1).Advanced();

        Bind("Only On Landed Hits", &m_onlyOnHit,
             "Fire on a real swing with a target in reach, not on every "
             "click at thin air")
            .Advanced();

        Bind("Only While Moving", &m_onlyWhileMoving,
             "There is nothing to reset if you are standing still")
            .Advanced();

        Bind("Require Sprint", &m_requireSprint,
             "Resetting a sprint you do not have just costs momentum")
            .Advanced();
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);

        // Packet mode is the only one that does not need keybinds
        if (!KeyBinds::Init(env) && m_method != PACKET) return;

        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;

        jobject player = Minecraft::GetPlayer(env);

        // The mouse edge is tracked every tick, not only inside the
        // branch that uses it. Updating it lazily meant switching
        // trigger modes with the button already held read as a fresh
        // press and fired one reset for free.
        //
        // Focus::KeyHeld rather than a bare GetAsyncKeyState: the
        // latter is global, so a click in another window while
        // alt-tabbed produced a rising edge and a phantom reset.
        bool lmb = Focus::KeyHeld(VK_LBUTTON);
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
            jobject player = env ? Minecraft::GetPlayer(env) : nullptr;
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
        m_status[0] = '\0';
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
        m_resets          = 0;
        m_why             = "idle";
        (void)env;   // keys are already released by the manager
    }

    const char* StatusLine() const override {
        int m = (m_method >= 0 && m_method < 6) ? m_method : 0;
        snprintf(m_status, sizeof(m_status), "%s  \xc2\xb7  %d resets",
                 kMethods[m], m_resets);
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!KeyBinds::HasMovement() && m_method != PACKET) {
            *text = "The movement keybinds could not be found in this build "
                    "of the game. Only Packet works here.";
            return NoticeLevel::Danger;
        }
        if (m_method == PACKET) {
            *text = "Packet mode toggles the sprint flag directly, which "
                    "Polar and AGC both look for. Every other method is "
                    "indistinguishable from a good player.";
            return NoticeLevel::Danger;
        }
        if (m_onlyOnHit && !CombatState::IsUsable()) {
            *text = "Swing detection could not be resolved, so nothing will "
                    "ever trigger. Turn off Only On Landed Hits.";
            return NoticeLevel::Warning;
        }
        if (!m_rearmSprint && MethodBreaksSprint(m_method)) {
            *text = "Re-arm Sprint is off. If you sprint by double-tapping W "
                    "you will stay at walking speed after every hit.";
            return NoticeLevel::Warning;
        }
        if (m_rescued > 0) {
            snprintf(m_notice, sizeof(m_notice),
                     "The watchdog freed a stuck key %d time(s) this session. "
                     "Nothing is broken, but a tick was lost somewhere.",
                     m_rescued);
            *text = m_notice;
            return NoticeLevel::Info;
        }

        switch (m_method) {
            case WTAP:  *text = "Releases forward for a tick. Works on every "
                                "server and costs a little momentum."; break;
            case STAP:  *text = "Taps back. Stops you faster and opens "
                                "distance, which suits reach fights."; break;
            case BLOCKHIT: *text = "Holds block, which also halves the damage "
                                   "you take. Sword only."; break;
            case SNEAK: *text = "Taps sneak. No speed loss at all, which is "
                                "why it is the sumo standard."; break;
            case CTRL:  *text = "Re-presses the sprint key. Loses the least "
                                "momentum of any real method."; break;
            default:    return NoticeLevel::None;
        }
        return NoticeLevel::Info;
    }
};
