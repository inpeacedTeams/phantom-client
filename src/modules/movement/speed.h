#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/movement.h"
#include "../../mc/combat_state.h"
#include <cstdio>
#include <cmath>
#include <random>

// =================================================================
// Speed
// =================================================================
// There are two completely different things hiding under this name
// and mixing them up is why most clients get people banned.
//
//   Sprint Jump is not a cheat. In 1.8 a sprinting player who jumps
//   gets a 1.3x forward boost on takeoff and keeps almost all of it
//   through the air, because air friction is far lower than ground
//   friction. Continuous hopping is measurably faster than running
//   flat. Every good 1.8 player already does it by hand. We drive
//   GameSettings.keyBindJump.pressed and nothing else, so the game
//   builds the movement itself and the packet stream is identical
//   to a human spamming space. There is nothing for an anticheat to
//   see, because nothing about the movement is abnormal.
//
//   Strafe and BHop overwrite the motion vector. A prediction
//   anticheat re-simulates your movement from your inputs and
//   compares the result to where you actually are. Any multiplier
//   shows up on the first tick. No amount of tuning fixes this.
//   They are here for unprotected servers and labelled as such.
//
// MODES
//   0 Sprint Jump   input only, undetectable, default
//   1 Strafe        rewrites the motion vector, detected
//   2 BHop          hop plus a motion multiplier, detected
// =================================================================

class Speed : public Module {
private:
    static constexpr const char* kModes[] = { "Sprint Jump", "Strafe", "BHop" };

    // ---- Core ----
    int   m_mode       = 0;
    float m_multiplier = 1.6f;

    // ---- Advanced ----
    bool  m_groundOnly     = false;
    bool  m_requireSprint  = true;   // only hop when actually sprinting
    bool  m_forwardOnly    = true;   // no hopping while strafing sideways
    bool  m_pauseInCombat  = true;   // leave the ground to More KB / Velocity
    int   m_combatPause    = 6;      // ticks of quiet after taking a hit
    float m_skipChance     = 7.0f;   // human hop streaks are not perfect
    int   m_groundTicksMin = 1;      // ticks on the ground between hops
    int   m_groundTicksMax = 2;

    bool m_holdingJump   = false;
    int  m_groundTicks   = 0;
    int  m_targetGround  = 1;
    int  m_hops          = 0;

    mutable char m_status[40] = "";

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        return std::uniform_int_distribution<int>(lo, hi)(m_rng);
    }
    bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(m_rng) < pct;
    }

    void ReleaseJump(JNIEnv* env) {
        if (!m_holdingJump) return;
        KeyBinds::ReleaseJump(env);
        m_holdingJump = false;
    }

    // -------------------------------------------------------------
    // Sprint Jump
    // -------------------------------------------------------------
    // The jump key has to go back up between hops. EntityPlayerSP
    // only jumps on the tick the key is held while on the ground,
    // and holding it down permanently produces a rigid hop every
    // landing tick, which is a pattern in itself. A short random
    // pause on the ground breaks that up and costs almost no speed.
    void TickSprintJump(JNIEnv* env, jobject player) {
        bool onGround = Minecraft::IsOnGround(env, player);

        if (!onGround) {                    // airborne: key must be up
            ReleaseJump(env);
            m_groundTicks = 0;
            return;
        }

        ReleaseJump(env);                   // land first, then decide
        m_groundTicks++;

        if (KeyBinds::GetSneak(env)) return;

        // Knockback and sprint reset both need the ground. Hopping
        // through an exchange throws away the reset and turns your
        // own knockback into a floaty arc.
        if (m_pauseInCombat && CombatState::TicksSinceHit() < m_combatPause)
            return;

        Movement::Input in = Movement::Read(env);
        bool moving = m_forwardOnly ? (in.forward > 0.f) : in.moving;
        if (!moving) { m_hops = 0; return; }

        // Actually travelling, not walking into a wall
        if (Movement::Speed2D(env, player) < 0.08) return;

        // If the flag could not be resolved we hop anyway rather
        // than silently doing nothing forever.
        if (m_requireSprint && Minecraft::HasSprintCheck()
            && !Minecraft::IsSprinting(env, player)) return;

        if (m_groundTicks < m_targetGround) return;

        if (Roll(m_skipChance)) {           // miss one like a person does
            m_targetGround = Rand(m_groundTicksMin, m_groundTicksMax) + 2;
            m_groundTicks  = 0;
            return;
        }

        KeyBinds::SetJump(env, true);
        m_holdingJump  = true;
        m_groundTicks  = 0;
        m_targetGround = Rand(m_groundTicksMin, m_groundTicksMax);
        m_hops++;
    }

    // -------------------------------------------------------------
    // Motion modes
    // -------------------------------------------------------------
    void TickMotion(JNIEnv* env, jobject player) {
        bool onGround = Minecraft::IsOnGround(env, player);
        if (m_groundOnly && !onGround) return;

        if (!Movement::Read(env).moving) return;

        if (m_mode == 2) {                  // BHop
            if (onGround) Minecraft::SetMotionY(env, player, 0.4);
            double mx = Minecraft::GetMotionX(env, player);
            double mz = Minecraft::GetMotionZ(env, player);
            Minecraft::SetMotionX(env, player, mx * m_multiplier);
            Minecraft::SetMotionZ(env, player, mz * m_multiplier);
            return;
        }

        Movement::SetHorizontal(env, player,
                                Movement::kSprintSpeed * m_multiplier);
    }

public:
    Speed() : Module("Speed", "Sprint jump automatically, or force raw motion",
                     ModuleCategory::MOVEMENT, 'F')
    {
        BindMode("Mode", &m_mode, kModes, 3,
                 "Sprint Jump only presses space, so the server sees a normal "
                 "player. The other two rewrite your motion and are caught by "
                 "any prediction anticheat.");

        Bind("Multiplier", &m_multiplier, 1.0f, 3.0f, "%.2fx",
             "How much faster than a vanilla sprint")
            .Unless("Mode", 0);

        Bind("Require Sprint", &m_requireSprint,
             "Only hop while actually sprinting")
            .When("Mode", 0).Advanced();

        Bind("Forward Only", &m_forwardOnly,
             "No hopping while strafing sideways")
            .When("Mode", 0).Advanced();

        Bind("Pause After A Hit", &m_pauseInCombat,
             "Keeps the ground free for More KB and Velocity")
            .When("Mode", 0).Advanced();

        Bind("Combat Pause", &m_combatPause, 2, 20,
             "Ticks of quiet after taking a hit")
            .When("Mode", 0).Advanced();

        Bind("Skip Chance", &m_skipChance, 0.0f, 25.0f, "%.0f%%",
             "Miss the occasional hop, the way a person does")
            .When("Mode", 0).Advanced();

        Bind("Ground Ticks Min", &m_groundTicksMin, 1, 6,
             "Shortest pause on the ground between hops")
            .When("Mode", 0).Advanced();

        Bind("Ground Ticks Max", &m_groundTicksMax, 1, 8,
             "Longest pause on the ground between hops")
            .When("Mode", 0).Advanced();

        Bind("Ground Only", &m_groundOnly,
             "Do not touch your motion while airborne")
            .Unless("Mode", 0).Advanced();
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        // A slider cannot produce this, but a hand-edited config can
        if (m_groundTicksMin > m_groundTicksMax)
            m_groundTicksMin = m_groundTicksMax;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { ReleaseJump(env); return; }

        if (Minecraft::IsInGui(env)) { ReleaseJump(env); return; }

        if (m_mode == 0) {
            if (!KeyBinds::HasJump()) return;
            TickSprintJump(env, player);
        } else {
            ReleaseJump(env);
            TickMotion(env, player);
        }
    }

    void OnDisable(JNIEnv* env) override {
        ReleaseJump(env);
        m_groundTicks = 0;
        m_hops = 0;
        m_status[0] = '\0';
    }

    // A world change leaves the jump key state and the hop counter
    // describing a game that no longer exists.
    void OnReset(JNIEnv* env) override {
        ReleaseJump(env);
        m_groundTicks = 0;
        m_targetGround = 1;
        m_hops = 0;
    }

    NoticeLevel Notice(const char** text) const override {
        if (m_mode != 0) {
            *text = "Rewrites your motion vector. Caught on the first tick by "
                    "AGC, Grim and Polar. Unprotected servers only.";
            return NoticeLevel::Danger;
        }
        if (!KeyBinds::HasJump()) {
            *text = "The jump keybind could not be found in this build of the "
                    "game, so the module cannot do anything.";
            return NoticeLevel::Warning;
        }
        if (m_requireSprint && !Minecraft::HasSprintCheck()) {
            *text = "The sprint flag could not be resolved, so Require Sprint "
                    "is being ignored and it hops regardless.";
            return NoticeLevel::Info;
        }
        return NoticeLevel::None;
    }

    const char* StatusLine() const override {
        if (m_mode == 0) snprintf(m_status, sizeof(m_status), "%d hops", m_hops);
        else             snprintf(m_status, sizeof(m_status), "%.2fx motion", m_multiplier);
        return m_status;
    }
};
